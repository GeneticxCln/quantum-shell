// Integration test: the actions that change the session, and whether the model follows them.
//
// Every other test in this project only reads. This one acts: it opens and closes the overview, asks
// niri to switch the keyboard layout, and focuses a workspace. Those are visible on screen, which is
// why it is **not** registered with ctest by default — see tests/CMakeLists.txt, where it needs
// QS_NIRI_SESSION_TESTS to be set as well as $NIRI_SOCKET. A test run must not rearrange the desktop of
// whoever started it.
//
// The actions with effects this test cannot undo without asking — running a process, moving a window,
// taking a screenshot that would land in the clipboard — are pinned by shape in
// tests/unit/niri_actions_test.cpp and deliberately not performed here. FocusWorkspace gets a live case
// because it can be undone exactly: the case focuses another workspace on the same output and then
// focuses the original one back, by id both times, and cleanup restores it too.
//
// "The workspace that is already focused" is deliberately *not* used as a no-op, which is what this test
// tried first and why it no longer does. niri resolves `Index` as a slot and calls `switch_workspace`,
// and on a config that sets `workspace-auto-back-and-forth` — this session's does — asking for the
// workspace already focused switches to the previously focused one instead. A live no-op cannot be built
// out of that action on a config this project does not own.
//
// A live session belongs to whoever started it and they may keep working while this runs. That is not a
// theoretical hazard: this test was written after watching the user switch workspace and focus a window
// in the middle of a run, and the model followed both events. So every assertion here is against the
// compositor's own answer, which holds however the session moved, rather than against a remembered
// snapshot of a still desktop — and where a claim genuinely needs a still desktop, the test checks that
// the compositor says it held still and reports it instead of asserting it.
//
// The session is left as it was found: cleanupTestCase restores the overview to the state it had when
// the test started and puts the keyboard layout index back, and it reports both rather than assuming.
#include "niri/NiriActions.h"
#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"
#include "niri/NiriKeyboardLayouts.h"
#include "niri/NiriState.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>

#include <functional>
#include <memory>
#include <optional>

using quantum::niri::NiriActions;
using quantum::niri::NiriEventStream;
using quantum::niri::NiriIPC;
using quantum::niri::NiriKeyboardLayouts;
using quantum::niri::NiriState;
using quantum::niri::NiriWorkspace;
using quantum::niri::Reply;
using quantum::niri::niriSocketPath;

namespace {

bool waitFor(const std::function<bool()>& condition) {
    return QTest::qWaitFor(condition, 5000);
}

// Storage the handler shares ownership of, rather than a local it refers to. A request that gives up while
// the handler is still registered — `NiriIPC` holds it until a reply arrives — would otherwise be written to
// through a reference once the wait had returned, and a frame that has gone is not a place to write a reply.
std::optional<Reply> request(NiriIPC& client, QStringView name) {
    auto received = std::make_shared<std::optional<Reply>>();
    client.send(name, [received](const Reply& reply) { *received = reply; });
    if (!QTest::qWaitFor([received] { return received->has_value(); }, 5000)) {
        return std::nullopt;
    }
    return *received;
}

}  // namespace

class NiriLiveActionTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void theOverviewActionsDriveTheModelBothWays();
    void anActionTheCompositorDoesNotKnowIsReportedNotIgnored();
    void focusingAnotherWorkspaceMovesFocusAndFocusingItBackRestoresIt();
    void theWheelActionsMoveThroughTheCompositorsOwnWorkspaces();
    void switchingTheKeyboardLayoutKeepsTheModelValid();
    void theKeyboardLayoutSwitchMovesTheActiveLayout();

private:
    using Handler = NiriActions::ResultHandler;

    // Runs one action and waits for its result.
    std::optional<NiriActions::Result> perform(const std::function<void(const Handler&)>& call);
    std::optional<bool> compositorOverviewState();
    std::optional<quint64> compositorFocusedWorkspaceId();
    // The workspace this test moved focus to, while it is responsible for it. Cleanup uses it to put
    // focus back, and its emptiness is what says the test has no outstanding change to undo.
    std::optional<quint64> focusMovedTo_;
    std::optional<quint64> focusedWorkspaceAtStart_;
    void ensureOverviewClosed();
    void restoreOverview();

    NiriIPC client_;
    NiriActions actions_{client_};
    NiriEventStream stream_;
    NiriState state_;
    QString transportFailure_;
    QString streamFailure_;
    bool overviewWasOpen_ = false;
    int layoutIndexAtStart_ = -1;
};

void NiriLiveActionTest::initTestCase() {
    if (niriSocketPath().isEmpty()) {
        QSKIP("$NIRI_SOCKET is not set: this test needs a running niri session");
    }

    state_.observe(stream_);
    connect(&stream_, &NiriEventStream::transportError, this,
            [this](const QString& reason) { transportFailure_ = reason; });
    connect(&stream_, &NiriEventStream::streamFailed, this,
            [this](const QString& reason) { streamFailure_ = reason; });

    QSignalSpy requestConnected(&client_, &NiriIPC::connected);
    connect(&client_, &NiriIPC::transportError, this,
            [this](const QString& reason) { transportFailure_ = reason; });
    client_.connectToCompositor();
    QTRY_VERIFY_WITH_TIMEOUT(requestConnected.count() == 1 || !transportFailure_.isEmpty(), 5000);
    QVERIFY2(requestConnected.count() == 1,
             qPrintable(QStringLiteral("no request connection to %1: %2")
                            .arg(niriSocketPath(), transportFailure_)));

    QSignalSpy streaming(&stream_, &NiriEventStream::streaming);
    stream_.connectToCompositor();
    QTRY_VERIFY_WITH_TIMEOUT(streaming.count() == 1 || !streamFailure_.isEmpty(), 5000);
    QVERIFY2(streaming.count() == 1,
             qPrintable(QStringLiteral("the stream never started: %1").arg(streamFailure_)));

    QVERIFY2(waitFor([this] { return state_.keyboardLayouts().isValid(); }),
             "the compositor never reported its keyboard layouts");

    // What the session looked like before this test touched it, so cleanup can put it back.
    const std::optional<bool> overview = compositorOverviewState();
    QVERIFY2(overview.has_value(), "the compositor never answered an OverviewState request");
    overviewWasOpen_ = *overview;
    layoutIndexAtStart_ = state_.keyboardLayouts().currentIndex();
    focusedWorkspaceAtStart_ = compositorFocusedWorkspaceId();

    qInfo("session before: overview %s, keyboard layout %d of %d (%s)", overviewWasOpen_ ? "open" : "closed",
          layoutIndexAtStart_, state_.keyboardLayouts().size(),
          qPrintable(state_.keyboardLayouts().currentName()));
}

void NiriLiveActionTest::cleanupTestCase() {
    restoreOverview();

    // Focus is only pulled back while this test still owns it: if the focused workspace is no longer the
    // one this test moved to, the session has been used since and the user's own choice outranks a
    // restoration the test would otherwise perform.
    if (focusMovedTo_.has_value() && focusedWorkspaceAtStart_.has_value()) {
        const std::optional<quint64> focusedNow = compositorFocusedWorkspaceId();
        if (focusedNow.has_value() && *focusedNow == *focusMovedTo_) {
            perform([this](const Handler& handler) {
                actions_.focusWorkspace(NiriActions::WorkspaceReference::withId(*focusedWorkspaceAtStart_),
                                        handler);
            });
            waitFor([this] {
                return compositorFocusedWorkspaceId() == focusedWorkspaceAtStart_;
            });
            qInfo("focus restored to workspace %llu",
                  static_cast<unsigned long long>(*focusedWorkspaceAtStart_));
        } else {
            qInfo("focus was left on workspace %llu: this test had moved it to %llu but the session has "
                  "moved on, so restoring would override a newer choice",
                  static_cast<unsigned long long>(focusedNow.value_or(0)),
                  static_cast<unsigned long long>(*focusMovedTo_));
        }
    }

    // Put the keyboard layout back if a switch moved it. The action, not the config, is what this test
    // changed, so the action is what undoes it — and by index, which is exact, where "previous" would
    // depend on how many switches ran.
    if (state_.keyboardLayouts().isValid()
        && state_.keyboardLayouts().currentIndex() != layoutIndexAtStart_) {
        perform([this](const Handler& handler) {
            actions_.switchLayout(NiriActions::LayoutTarget::atIndex(layoutIndexAtStart_), handler);
        });
        waitFor([this] { return state_.keyboardLayouts().currentIndex() == layoutIndexAtStart_; });
    }

    // Report the outcome rather than assuming it: if the session could not be restored, that is worth
    // seeing in the test log.
    const bool restored = state_.isOverviewOpen() == overviewWasOpen_
                          && state_.keyboardLayouts().currentIndex() == layoutIndexAtStart_;
    qInfo("session after: overview %s (started %s), keyboard layout %d (started %d)%s",
          state_.isOverviewOpen() ? "open" : "closed", overviewWasOpen_ ? "open" : "closed",
          state_.keyboardLayouts().currentIndex(), layoutIndexAtStart_,
          restored ? "" : "  <-- NOT RESTORED");
}

std::optional<NiriActions::Result> NiriLiveActionTest::perform(
    const std::function<void(const Handler&)>& call) {
    // Shared for the same reason as `request` above: an action that is still in flight when this gives up
    // will call the handler afterwards, and the result has to land somewhere that still exists.
    auto received = std::make_shared<std::optional<NiriActions::Result>>();
    call([received](const NiriActions::Result& result) { *received = result; });
    if (!QTest::qWaitFor([received] { return received->has_value(); }, 5000)) {
        return std::nullopt;
    }
    return *received;
}

std::optional<bool> NiriLiveActionTest::compositorOverviewState() {
    const std::optional<Reply> reply = request(client_, QStringLiteral("OverviewState"));
    if (!reply.has_value() || !reply->isOk()) {
        return std::nullopt;
    }
    return reply->variant(QStringLiteral("OverviewState")).toObject().value(QStringLiteral("is_open")).toBool();
}

std::optional<quint64> NiriLiveActionTest::compositorFocusedWorkspaceId() {
    const std::optional<Reply> reply = request(client_, QStringLiteral("Workspaces"));
    if (!reply.has_value() || !reply->isOk()) {
        return std::nullopt;
    }
    const QJsonArray workspaces = reply->variant(QStringLiteral("Workspaces")).toArray();
    for (const QJsonValue& value : workspaces) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("is_focused")).toBool(false)) {
            const std::optional<quint64> id = quantum::niri::idFromJson(object.value(QStringLiteral("id")));
            if (id.has_value()) {
                return id;
            }
        }
    }
    // No focused workspace is a real answer — during the overview, for instance — and it is not an
    // error, so it is reported as an absent id rather than invented.
    return std::nullopt;
}

void NiriLiveActionTest::ensureOverviewClosed() {
    const std::optional<bool> overview = compositorOverviewState();
    if (!overview.has_value() || !*overview) {
        return;
    }
    perform([this](const Handler& handler) { actions_.closeOverview(handler); });
    waitFor([this] { return !state_.isOverviewOpen(); });
}

void NiriLiveActionTest::restoreOverview() {
    const std::optional<bool> overview = compositorOverviewState();
    if (!overview.has_value() || *overview == overviewWasOpen_) {
        return;
    }
    perform([this](const Handler& handler) {
        if (overviewWasOpen_) {
            actions_.openOverview(handler);
        } else {
            actions_.closeOverview(handler);
        }
    });
    waitFor([this] { return state_.isOverviewOpen() == overviewWasOpen_; });
}

void NiriLiveActionTest::theOverviewActionsDriveTheModelBothWays() {
    // Start from closed, so both directions are a real transition rather than a no-op.
    ensureOverviewClosed();
    QVERIFY2(!state_.isOverviewOpen(), "the overview could not be closed to begin with");

    const std::optional<NiriActions::Result> opened =
        perform([this](const Handler& handler) { actions_.openOverview(handler); });
    QVERIFY2(opened.has_value(), "the compositor never answered an OpenOverview action");
    QVERIFY2(opened->isHandled(), qPrintable(opened->describe()));

    // The model's answer comes from the OverviewOpenedOrClosed event; the compositor's comes from a
    // request. Both have to say open.
    QVERIFY2(waitFor([this] { return state_.isOverviewOpen(); }),
             "the model never reported the overview as open");
    const std::optional<bool> afterOpening = compositorOverviewState();
    QVERIFY2(afterOpening.has_value(), "the compositor never answered an OverviewState request");
    QCOMPARE(*afterOpening, true);

    const std::optional<NiriActions::Result> closed =
        perform([this](const Handler& handler) { actions_.closeOverview(handler); });
    QVERIFY2(closed.has_value(), "the compositor never answered a CloseOverview action");
    QVERIFY2(closed->isHandled(), qPrintable(closed->describe()));

    QVERIFY2(waitFor([this] { return !state_.isOverviewOpen(); }),
             "the model never reported the overview as closed");
    const std::optional<bool> afterClosing = compositorOverviewState();
    QVERIFY2(afterClosing.has_value(), "the compositor never answered an OverviewState request");
    QCOMPARE(*afterClosing, false);

    qInfo("the overview opened and closed, and the model followed both ways");
}

void NiriLiveActionTest::anActionTheCompositorDoesNotKnowIsReportedNotIgnored() {
    const bool overviewBefore = state_.isOverviewOpen();

    // An action outside niri's `Action` enum is not something the typed layer can express, so it goes
    // out by hand: this is a fact about the compositor, not about the API. niri answers a name it does
    // not know the same way it answers any request it cannot parse.
    std::optional<Reply> reply;
    client_.sendObject(QJsonObject{{QStringLiteral("Action"),
                                    QJsonObject{{QStringLiteral("QuantumShellProbe"), QJsonObject{}}}}},
                       [&reply](const Reply& received) { reply = received; });
    QVERIFY2(QTest::qWaitFor([&reply] { return reply.has_value(); }, 5000),
             "the compositor never answered an unknown action");

    QVERIFY(!reply->isOk());
    QCOMPARE(reply->text, QStringLiteral("error parsing request"));
    // And nothing changed: a rejected action is not a performed one.
    //
    // When the two differ, what is asked is which side moved — because this case runs on a session a person
    // is sitting at, niri's own default config binds the overview to a key, and somebody pressing it inside
    // this 150 ms wait changes exactly the value being compared. Comparing the model with the model as it was
    // would fail here and name the shell for something the person did, which is the same mistake as comparing
    // a width against one taken before the clock ticked: a value remembered across a wait is not a baseline
    // the outside world has agreed to hold still.
    //
    // The compositor is the discriminator, and it is the right one because the request being refused is the
    // only thing this shell sent: niri answers an action it does not know with a parse error and performs
    // nothing, so a model that agrees with the compositor was told about a change that happened, while a model
    // that disagrees with it has moved on its own — which is the defect this case exists for, and it still
    // fails.
    QTest::qWait(150);
    const bool overviewNow = state_.isOverviewOpen();
    if (overviewNow != overviewBefore) {
        const std::optional<bool> compositorNow = compositorOverviewState();
        QVERIFY2(compositorNow.has_value(), "the compositor never answered an OverviewState request");
        QVERIFY2(*compositorNow == overviewNow,
                 qPrintable(QStringLiteral("the overview is %1 on the model where the compositor reports %2, "
                                           "after a request the compositor refused")
                                .arg(overviewNow ? QStringLiteral("open") : QStringLiteral("closed"),
                                     *compositorNow ? QStringLiteral("open") : QStringLiteral("closed"))));
        qInfo("the overview changed while the refused request was in flight and the compositor reports the "
              "same change (%s): the desktop was acted on rather than the shell",
              overviewNow ? "open" : "closed");
    }
}

void NiriLiveActionTest::focusingAnotherWorkspaceMovesFocusAndFocusingItBackRestoresIt() {
    // FocusWorkspace is a switch in niri rather than a lookup, and this session's config sets
    // `workspace-auto-back-and-forth`, so a no-op cannot be built from it — see the note at the top of this
    // file. A real switch with a real switch back can, and it exercises more: niri has to resolve a
    // workspace by the id this build sent, and focus has to end up where the id named.
    constexpr int attempts = 3;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        // The origin is taken from the compositor, and the model has to already agree with it, so a
        // change that follows can be attributed to the action instead of to a model that was already
        // wrong.
        const std::optional<quint64> origin = compositorFocusedWorkspaceId();
        QVERIFY2(origin.has_value(), "the compositor never answered a Workspaces request");
        const NiriWorkspace originWorkspace = state_.workspace(*origin);
        QVERIFY2(originWorkspace.isValid(), "the focused workspace is not in the model");
        QCOMPARE(state_.focusedWorkspace().id(), *origin);

        // A different workspace on the same output. Any would do, and the first by index is picked so the
        // test does the same thing every run.
        std::optional<quint64> target;
        for (const NiriWorkspace& candidate : state_.workspacesOn(originWorkspace.output())) {
            if (candidate.id() != *origin) {
                target = candidate.id();
                break;
            }
        }
        if (!target.has_value()) {
            QSKIP("this output holds a single workspace, so there is no second one to focus and return from");
        }

        const std::optional<NiriActions::Result> moved =
            perform([this, &target](const Handler& handler) {
                actions_.focusWorkspace(NiriActions::WorkspaceReference::withId(*target), handler);
            });
        QVERIFY2(moved.has_value(), "the compositor never answered a FocusWorkspace action");
        QVERIFY2(moved->isHandled(), qPrintable(moved->describe()));
        // From here this test owns focus until it gives it back, so cleanup knows to as well.
        focusMovedTo_ = target;

        // The compositor has to report the workspace the id named, and the model has to agree with it.
        // Neither is read out of a remembered snapshot, so both stay true however the session is used.
        const bool landed = QTest::qWaitFor(
            [this, &target] { return compositorFocusedWorkspaceId() == target; }, 3000);
        if (!landed) {
            const std::optional<quint64> now = compositorFocusedWorkspaceId();
            QVERIFY2(now.has_value(), "the compositor never answered a Workspaces request");
            if (*now == *origin) {
                // Nothing moved and nothing can explain it except the action: this is the failure the
                // test exists to catch, not a busy session.
                QFAIL(qPrintable(QStringLiteral("the compositor accepted FocusWorkspace for workspace %1 "
                                                "and left the focused workspace at %2")
                                     .arg(*target)
                                     .arg(*origin)));
            }
            qInfo("attempt %d of %d: the session moved focus to workspace %llu while this attempt ran, so "
                  "nothing could be concluded; the action was accepted",
                  attempt, attempts, static_cast<unsigned long long>(*now));
            focusMovedTo_.reset();
            continue;
        }

        QTRY_VERIFY_WITH_TIMEOUT(state_.focusedWorkspace().id() == compositorFocusedWorkspaceId(), 5000);
        QCOMPARE(state_.focusedWorkspace().id(), *target);
        qInfo("FocusWorkspace by id focused workspace %llu, and the model followed",
              static_cast<unsigned long long>(*target));

        // And back, by id again: the action that moved focus is the action that puts it back.
        const std::optional<NiriActions::Result> returned =
            perform([this, &origin](const Handler& handler) {
                actions_.focusWorkspace(NiriActions::WorkspaceReference::withId(*origin), handler);
            });
        QVERIFY2(returned.has_value(), "the compositor never answered a FocusWorkspace action");
        QVERIFY2(returned->isHandled(), qPrintable(returned->describe()));

        const bool restored = QTest::qWaitFor(
            [this, &origin] { return compositorFocusedWorkspaceId() == origin; }, 3000);
        if (!restored) {
            // The same code path has just been verified in the other direction, so a session that moved
            // during the return is reported rather than failing the test; cleanup still holds the change.
            qInfo("the session did not return to workspace %llu: cleanup will report what focus was left "
                  "on",
                  static_cast<unsigned long long>(*origin));
            return;
        }

        QTRY_VERIFY_WITH_TIMEOUT(state_.focusedWorkspace().id() == compositorFocusedWorkspaceId(), 5000);
        QCOMPARE(state_.focusedWorkspace().id(), *origin);
        focusMovedTo_.reset();
        qInfo("focus returned to workspace %llu, and the model followed",
              static_cast<unsigned long long>(*origin));
        return;
    }

    qInfo("every attempt was overtaken by the session moving: the action is verified as accepted each "
          "time, but where it sent focus could not be observed");
}


void NiriLiveActionTest::theWheelActionsMoveThroughTheCompositorsOwnWorkspaces() {
    // The two requests the bar's wheel sends, and the one pair whose *name* only the compositor can
    // confirm: `niri_actions_test` pins the bytes this build writes, and this case is what says niri
    // 26.04 accepts them as the actions its own default config binds to the wheel. A name niri did not
    // have would come back as a parse error, which is what the failure list below catches.
    //
    // They move focus the way the desktop's own wheel binding does — down to the workspace below, up to
    // the one above — so the case reads as a person scrolling over the bar: one step down, and one step
    // back. Where it lands is the compositor's answer rather than an index computed here, which is the
    // point of using these actions at all.
    QStringList failures;
    const QMetaObject::Connection failure =
        connect(&actions_, &NiriActions::actionFailed, this,
                [&failures](const QString& action, const NiriActions::Result&) {
                    failures.append(action);
                });

    const std::optional<quint64> origin = compositorFocusedWorkspaceId();
    QVERIFY2(origin.has_value(), "the compositor never answered a Workspaces request");
    QCOMPARE(state_.focusedWorkspace().id(), *origin);

    actions_.focusWorkspaceDown();
    const bool moved = QTest::qWaitFor(
        [this, &origin] {
            const std::optional<quint64> now = compositorFocusedWorkspaceId();
            return now.has_value() && *now != *origin;
        },
        3000);
    QVERIFY2(failures.isEmpty(),
             qPrintable(QStringLiteral("the compositor refused %1").arg(failures.join(", "))));
    QVERIFY2(moved,
             qPrintable(QStringLiteral("the compositor accepted FocusWorkspaceDown and left the focused "
                                       "workspace at %1")
                            .arg(*origin)));

    const std::optional<quint64> below = compositorFocusedWorkspaceId();
    QVERIFY2(below.has_value(), "the compositor never answered a Workspaces request");
    // Focus is now somewhere this test moved it to, so cleanup owns putting it back if anything below
    // fails before it is returned.
    focusMovedTo_ = below;
    QTRY_COMPARE_WITH_TIMEOUT(state_.focusedWorkspace().id(), *below, 5000);
    qInfo("FocusWorkspaceDown moved focus to workspace %llu, and the model followed",
          static_cast<unsigned long long>(*below));

    actions_.focusWorkspaceUp();
    const bool returned = QTest::qWaitFor(
        [this, &origin] { return compositorFocusedWorkspaceId() == origin; }, 3000);
    QVERIFY2(failures.isEmpty(),
             qPrintable(QStringLiteral("the compositor refused %1").arg(failures.join(", "))));
    if (returned) {
        QTRY_COMPARE_WITH_TIMEOUT(state_.focusedWorkspace().id(), *origin, 5000);
        focusMovedTo_.reset();
        qInfo("FocusWorkspaceUp returned focus to workspace %llu, and the model followed",
              static_cast<unsigned long long>(*origin));
    } else {
        // FocusWorkspaceDown has just been verified in the same run, so a session that moved during the
        // return is reported: cleanup still holds the change and will say what it did with it.
        qInfo("the session did not return to workspace %llu: cleanup will report what focus was left on",
              static_cast<unsigned long long>(*origin));
    }

    disconnect(failure);
}

void NiriLiveActionTest::switchingTheKeyboardLayoutKeepsTheModelValid() {
    const NiriKeyboardLayouts before = state_.keyboardLayouts();
    QVERIFY(before.isValid());

    const std::optional<NiriActions::Result> result = perform([this](const Handler& handler) {
        actions_.switchLayout(NiriActions::LayoutTarget::next(), handler);
    });
    QVERIFY2(result.has_value(), "the compositor never answered a SwitchLayout action");
    QVERIFY2(result->isHandled(), qPrintable(result->describe()));

    // Whatever the switch produced, let the stream deliver it.
    QTest::qWait(200);

    // The model still names an active layout, and that name agrees with the compositor's own answer.
    const NiriKeyboardLayouts after = state_.keyboardLayouts();
    QVERIFY(after.isValid());
    QVERIFY2(!after.currentName().isEmpty(),
             "the model could not name the active keyboard layout after a switch");

    const std::optional<Reply> snapshot = request(client_, QStringLiteral("KeyboardLayouts"));
    QVERIFY2(snapshot.has_value(), "the compositor never answered a KeyboardLayouts request");
    QVERIFY2(snapshot->isOk(), qPrintable(snapshot->describe()));
    const QJsonObject layouts = snapshot->variant(QStringLiteral("KeyboardLayouts")).toObject();
    QCOMPARE(after.currentIndex(), layouts.value(QStringLiteral("current_idx")).toInt(-1));

    if (before.size() < 2) {
        // One configured layout: niri accepts the switch and changes nothing — verified on this
        // machine, where no KeyboardLayoutSwitched event arrives at all. The model must not invent a
        // change that the compositor did not make.
        QCOMPARE(after.currentIndex(), before.currentIndex());
        qInfo("one keyboard layout configured (%s): the switch was accepted and changed nothing",
              qPrintable(after.currentName()));
    }
}

void NiriLiveActionTest::theKeyboardLayoutSwitchMovesTheActiveLayout() {
    const NiriKeyboardLayouts before = state_.keyboardLayouts();
    QVERIFY(before.isValid());
    if (before.size() < 2) {
        QSKIP(qPrintable(QStringLiteral(
            "only %1 keyboard layout is configured (%2), so a switch has nothing to move to: "
            "SwitchLayout Next is accepted by niri and emits no KeyboardLayoutSwitched event, which "
            "this test verified by sending it. The transition needs a session with two or more "
            "keyboard layouts configured.")
                             .arg(before.size())
                             .arg(before.names().join(QLatin1Char(',')))));
    }

    const std::optional<NiriActions::Result> result = perform([this](const Handler& handler) {
        actions_.switchLayout(NiriActions::LayoutTarget::next(), handler);
    });
    QVERIFY2(result.has_value(), "the compositor never answered a SwitchLayout action");
    QVERIFY2(result->isHandled(), qPrintable(result->describe()));

    // The index moves, driven by the KeyboardLayoutSwitched event and not by this test.
    QVERIFY2(waitFor([this, before] {
                 return state_.keyboardLayouts().currentIndex() != before.currentIndex();
             }),
             "the model never reported the switched keyboard layout");
    QVERIFY(!state_.keyboardLayouts().currentName().isEmpty());

    const std::optional<Reply> snapshot = request(client_, QStringLiteral("KeyboardLayouts"));
    QVERIFY2(snapshot.has_value(), "the compositor never answered a KeyboardLayouts request");
    const QJsonObject layouts = snapshot->variant(QStringLiteral("KeyboardLayouts")).toObject();
    QCOMPARE(state_.keyboardLayouts().currentIndex(),
             layouts.value(QStringLiteral("current_idx")).toInt(-1));

    qInfo("the keyboard layout moved from index %d (%s) to %d (%s)", before.currentIndex(),
          qPrintable(before.currentName()), state_.keyboardLayouts().currentIndex(),
          qPrintable(state_.keyboardLayouts().currentName()));
}

QTEST_GUILESS_MAIN(NiriLiveActionTest)
#include "niri_live_action_test.moc"
