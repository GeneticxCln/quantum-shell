// Integration test: the event stream and the state model it feeds, against the compositor running
// this session.
//
// The unit tests check the decoder against lines this repository wrote. This test checks it against
// lines niri wrote, and — the part no unit test can do — checks that the model built from events
// agrees with what the compositor answers a direct request for the same facts. The events and the
// requests are two independent paths to one truth on the same running compositor, so they agreeing is
// evidence and disagreeing is a real bug.
//
// The two paths are read at two different moments, though, and the desktop can change between them. A
// window renamed after the model's last event, or a workspace focused after the reply was composed, makes
// the two disagree while both are right — which is not a defect, but is indistinguishable from one by a
// single pair of readings. Every comparison below therefore goes through `qstest::reconcile`, which takes
// readings until the two agree and, when they never do, says whether the compositor's own answer held
// still (a model that missed an event) or kept changing (a desktop that never settled). The mechanism and
// its contract are in `tests/support/SnapshotReconcile.h` and pinned by `snapshot-reconcile-test`;
// the slots here are the assertions themselves, still field for field what they always compared.
//
// Registered with ctest only when $NIRI_SOCKET is set (see tests/CMakeLists.txt).
#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"
#include "niri/NiriOutput.h"
#include "niri/NiriOutputs.h"
#include "niri/NiriState.h"

#include "SnapshotReconcile.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSignalSpy>
#include <QTest>

#include <functional>
#include <memory>
#include <optional>

using quantum::niri::NiriEventStream;
using quantum::niri::NiriIPC;
using quantum::niri::NiriOutput;
using quantum::niri::NiriOutputMode;
using quantum::niri::NiriOutputs;
using quantum::niri::NiriState;
using quantum::niri::NiriWindow;
using quantum::niri::NiriWorkspace;
using quantum::niri::Reply;
using quantum::niri::niriSocketPath;

namespace {

// Sends one read-only request on the request connection and waits for its reply, so the test can
// compare the live model against the compositor's own answer.
//
// The reply is caught into storage the handler shares ownership of rather than into a local the handler
// refers to, because a request that times out returns while `NiriIPC` still holds the handler: a reply
// arriving after the wait gave up would then be written through a reference to a frame that has gone. That is
// what crashed the layer-shell test twice — a corrupted stack canary and a segmentation fault in
// `QString::operator=` — and this file has the same helper, so it has the same hazard.
std::optional<Reply> requestSnapshot(NiriIPC& client, QStringView name) {
    auto received = std::make_shared<std::optional<Reply>>();
    client.send(name, [received](const Reply& reply) { *received = reply; });
    if (!QTest::qWaitFor([received] { return received->has_value(); }, 5000)) {
        return std::nullopt;
    }
    return *received;
}

quint64 idOf(const QJsonObject& object) {
    return static_cast<quint64>(object.value(QStringLiteral("id")).toDouble(-1));
}

// The object in `objects` whose id is `id`, or an empty object when the snapshot does not have it.
QJsonObject findById(const QJsonArray& objects, quint64 id) {
    for (const QJsonValue& value : objects) {
        const QJsonObject object = value.toObject();
        if (idOf(object) == id) {
            return object;
        }
    }
    return QJsonObject{};
}

std::optional<quint64> optionalId(const QJsonValue& value) {
    if (value.isNull() || value.isUndefined() || !value.isDouble()) {
        return std::nullopt;
    }
    return static_cast<quint64>(value.toDouble());
}

// `current_mode` is an index or null, never an id, so it needs its own reader.
std::optional<int> optionalIndex(const QJsonValue& value) {
    if (!value.isDouble() || value.toDouble() < 0.0) {
        return std::nullopt;
    }
    return static_cast<int>(value.toDouble());
}

// A text field as the model represents it. niri spells "no name" and "no output" as JSON null, and
// the model spells the same thing as an empty string, so the two are compared as one value rather
// than through a sentinel that would fail on a real machine with an unnamed workspace.
QString textOrEmpty(const QJsonObject& object, QStringView field) {
    const QJsonValue value = object.value(field);
    return value.isString() ? value.toString() : QString{};
}

// The compositor's own answer, compactly, used as the stamp that tells one reading from another. It is the
// compositor's words rather than the model's, so two readings a settled desktop gives are recognisable as
// the same reading — which is what separates a model that has missed an event from a desktop that has not
// held still long enough to be matched.
QString stampOf(const QJsonValue& payload) {
    const QJsonDocument document = payload.isArray() ? QJsonDocument(payload.toArray())
                                                     : QJsonDocument(payload.toObject());
    return QString::fromUtf8(document.toJson(QJsonDocument::Compact));
}

// --- what the model and the compositor have to agree about -------------------------------------------------
//
// Each returns what it found rather than failing on the spot, because a comparison that is about to be
// taken again must not mark the test failed on its first disagreement. The first disagreement found is the
// one kept, and both sides of it are named.

qstest::Disagreement compareWorkspaces(const NiriState& state, const QJsonArray& snapshot) {
    qstest::Disagreement disagreement;
    disagreement.check(QStringLiteral("the compositor's own Workspaces reply was empty"), !snapshot.isEmpty());
    disagreement.expectEqual(QStringLiteral("the number of workspaces"), state.workspaces().size(),
                             snapshot.size());

    int comparedFocused = 0;
    for (const NiriWorkspace& workspace : state.workspaces()) {
        const QJsonObject object = findById(snapshot, workspace.id());
        disagreement.check(QStringLiteral("workspace %1 is missing from the compositor's own Workspaces "
                                          "reply")
                               .arg(workspace.id()),
                           !object.isEmpty());
        if (object.isEmpty()) {
            continue;
        }

        const QString where = QStringLiteral("workspace %1").arg(workspace.id());
        disagreement.expectEqual(where + QStringLiteral(" idx"), workspace.idx(),
                                 object.value(QStringLiteral("idx")).toInt(-1));
        disagreement.expectEqual(where + QStringLiteral(" output"), workspace.output(),
                                 textOrEmpty(object, QStringLiteral("output")));
        disagreement.expectEqual(where + QStringLiteral(" name"), workspace.name(),
                                 textOrEmpty(object, QStringLiteral("name")));
        disagreement.expectEqual(where + QStringLiteral(" is_active"), workspace.isActive(),
                                 object.value(QStringLiteral("is_active")).toBool());
        disagreement.expectEqual(where + QStringLiteral(" is_urgent"), workspace.isUrgent(),
                                 object.value(QStringLiteral("is_urgent")).toBool());
        disagreement.expectEqual(where + QStringLiteral(" is_focused"), workspace.isFocused(),
                                 object.value(QStringLiteral("is_focused")).toBool());
        disagreement.expectEqual(where + QStringLiteral(" active_window_id"), workspace.activeWindowId(),
                                 optionalId(object.value(QStringLiteral("active_window_id"))));
        comparedFocused += workspace.isFocused() ? 1 : 0;
    }

    // The two paths also agree about which one is focused, and exactly one is.
    disagreement.check(QStringLiteral("the model holds %1 focused workspace(s) and exactly one is expected")
                           .arg(comparedFocused),
                       comparedFocused == 1);
    for (const QJsonValue& value : snapshot) {
        if (value.toObject().value(QStringLiteral("is_focused")).toBool()) {
            disagreement.expectEqual(QStringLiteral("the focused workspace"),
                                     state.focusedWorkspace().id(), idOf(value.toObject()));
        }
    }
    return disagreement;
}

qstest::Disagreement compareWindows(const NiriState& state, const QJsonArray& snapshot) {
    qstest::Disagreement disagreement;
    // An empty session is a real state, so the count is compared rather than required to be non-zero.
    disagreement.expectEqual(QStringLiteral("the number of windows"), state.windows().size(),
                             snapshot.size());

    int comparedFocused = 0;
    for (const NiriWindow& window : state.windows()) {
        const QJsonObject object = findById(snapshot, window.id());
        disagreement.check(QStringLiteral("window %1 is missing from the compositor's own Windows reply")
                               .arg(window.id()),
                           !object.isEmpty());
        if (object.isEmpty()) {
            continue;
        }

        const QString where = QStringLiteral("window %1").arg(window.id());
        disagreement.expectEqual(where + QStringLiteral(" title"), window.title(),
                                 textOrEmpty(object, QStringLiteral("title")));
        disagreement.expectEqual(where + QStringLiteral(" app_id"), window.appId(),
                                 textOrEmpty(object, QStringLiteral("app_id")));
        disagreement.expectEqual(where + QStringLiteral(" is_focused"), window.isFocused(),
                                 object.value(QStringLiteral("is_focused")).toBool());
        disagreement.expectEqual(where + QStringLiteral(" is_floating"), window.isFloating(),
                                 object.value(QStringLiteral("is_floating")).toBool());
        disagreement.expectEqual(where + QStringLiteral(" is_urgent"), window.isUrgent(),
                                 object.value(QStringLiteral("is_urgent")).toBool());
        disagreement.expectEqual(where + QStringLiteral(" workspace_id"), window.workspaceId(),
                                 optionalId(object.value(QStringLiteral("workspace_id"))));
        if (object.value(QStringLiteral("pid")).isDouble()) {
            disagreement.expectEqual(where + QStringLiteral(" pid"), window.pid(),
                                     std::optional<qint32>(static_cast<qint32>(
                                         object.value(QStringLiteral("pid")).toDouble())));
        }
        comparedFocused += window.isFocused() ? 1 : 0;
    }

    // niri reports at most one focused window, and the null-id case is a real state: focus can sit on
    // a layer-shell surface instead. Either way both paths must agree.
    disagreement.check(QStringLiteral("the model holds %1 focused window(s); niri reports at most one")
                           .arg(comparedFocused),
                       comparedFocused <= 1);
    disagreement.expectEqual(QStringLiteral("whether a window is focused"),
                             state.focusedWindow().isValid(), comparedFocused == 1);
    if (comparedFocused == 1) {
        for (const QJsonValue& value : snapshot) {
            if (value.toObject().value(QStringLiteral("is_focused")).toBool()) {
                disagreement.expectEqual(QStringLiteral("the focused window"),
                                         state.focusedWindow().id(), idOf(value.toObject()));
            }
        }
    }
    return disagreement;
}

qstest::Disagreement compareOutputs(const NiriState& state, const QJsonObject& snapshot) {
    qstest::Disagreement disagreement;
    disagreement.check(QStringLiteral("the compositor's own Outputs reply was empty"), !snapshot.isEmpty());
    // Same set of outputs down both paths. The state's came from NiriOutputs asking; these came from
    // this test asking.
    disagreement.expectEqual(QStringLiteral("the number of outputs"), state.outputs().size(),
                             snapshot.size());

    for (const NiriOutput& output : state.outputs()) {
        const QJsonObject object = snapshot.value(output.name()).toObject();
        disagreement.check(QStringLiteral("output %1 is missing from the compositor's own Outputs reply")
                               .arg(output.name()),
                           !object.isEmpty());
        if (object.isEmpty()) {
            continue;
        }

        const QString where = QStringLiteral("output %1").arg(output.name());
        disagreement.expectEqual(where + QStringLiteral(" name"), output.name(),
                                 object.value(QStringLiteral("name")).toString());
        // A null logical output is how niri says the output is disabled, so this also checks that a
        // disabled output was not mistaken for a missing one.
        disagreement.expectEqual(where + QStringLiteral(" is enabled"), output.isEnabled(),
                                 object.value(QStringLiteral("logical")).isObject());
        disagreement.expectEqual(where + QStringLiteral(" current_mode"), output.currentModeIndex(),
                                 optionalIndex(object.value(QStringLiteral("current_mode"))));
        disagreement.expectEqual(where + QStringLiteral(" vrr_supported"), output.isVrrSupported(),
                                 object.value(QStringLiteral("vrr_supported")).toBool());
        disagreement.expectEqual(where + QStringLiteral(" vrr_enabled"), output.isVrrEnabled(),
                                 object.value(QStringLiteral("vrr_enabled")).toBool());
        disagreement.expectEqual(where + QStringLiteral(" number of modes"), output.modes().size(),
                                 object.value(QStringLiteral("modes")).toArray().size());

        if (!output.logical().has_value()) {
            continue;
        }
        const QJsonObject logical = object.value(QStringLiteral("logical")).toObject();
        disagreement.expectEqual(where + QStringLiteral(" logical x"), output.logical()->x,
                                 logical.value(QStringLiteral("x")).toInt());
        disagreement.expectEqual(where + QStringLiteral(" logical y"), output.logical()->y,
                                 logical.value(QStringLiteral("y")).toInt());
        disagreement.expectEqual(where + QStringLiteral(" logical width"), output.logical()->width,
                                 static_cast<quint32>(logical.value(QStringLiteral("width")).toInt()));
        disagreement.expectEqual(where + QStringLiteral(" logical height"), output.logical()->height,
                                 static_cast<quint32>(logical.value(QStringLiteral("height")).toInt()));
        // The real fractional scale, compared as one: this is the reading that would be wrong if
        // anything had rounded it.
        disagreement.expectEqual(where + QStringLiteral(" logical scale"), output.logical()->scale,
                                 logical.value(QStringLiteral("scale")).toDouble());
        disagreement.expectEqual(where + QStringLiteral(" logical transform"), output.logical()->transform,
                                 logical.value(QStringLiteral("transform")).toString());

        const std::optional<NiriOutputMode> mode = output.currentMode();
        disagreement.expectEqual(where + QStringLiteral(" has a current mode"), mode.has_value(),
                                 output.currentModeIndex().has_value());
        if (!mode.has_value()) {
            continue;
        }
        const QJsonObject current = object.value(QStringLiteral("modes"))
                                        .toArray()
                                        .at(*output.currentModeIndex())
                                        .toObject();
        disagreement.expectEqual(where + QStringLiteral(" mode width"), mode->width,
                                 static_cast<quint16>(current.value(QStringLiteral("width")).toInt()));
        disagreement.expectEqual(where + QStringLiteral(" mode height"), mode->height,
                                 static_cast<quint16>(current.value(QStringLiteral("height")).toInt()));
        disagreement.expectEqual(where + QStringLiteral(" mode refresh_rate"), mode->refreshRate,
                                 static_cast<quint32>(
                                     current.value(QStringLiteral("refresh_rate")).toDouble()));
        disagreement.expectEqual(where + QStringLiteral(" mode is_preferred"), mode->isPreferred,
                                 current.value(QStringLiteral("is_preferred")).toBool());
    }
    return disagreement;
}

qstest::Disagreement compareKeyboardLayouts(const NiriState& state, const QJsonObject& snapshot) {
    qstest::Disagreement disagreement;
    // The model's layouts arrived as a KeyboardLayoutsChanged event in the initial burst; these came
    // from a request. They have to be the same layouts with the same one active.
    disagreement.check(QStringLiteral("the model holds no keyboard layouts"),
                       state.keyboardLayouts().isValid());

    QStringList names;
    for (const QJsonValue& name : snapshot.value(QStringLiteral("names")).toArray()) {
        names.append(name.toString());
    }
    disagreement.check(QStringLiteral("the compositor reports no keyboard layouts"), !names.isEmpty());
    disagreement.expectEqual(QStringLiteral("the keyboard layout names"),
                             state.keyboardLayouts().names(), names);
    disagreement.expectEqual(QStringLiteral("the active layout index"),
                             state.keyboardLayouts().currentIndex(),
                             snapshot.value(QStringLiteral("current_idx")).toInt(-1));
    // And the active one resolves to a name, rather than to an empty string.
    disagreement.expectEqual(QStringLiteral("the active layout name"),
                             state.keyboardLayouts().currentName(),
                             names.value(state.keyboardLayouts().currentIndex()));
    disagreement.check(QStringLiteral("the active layout has no name"),
                       !state.keyboardLayouts().currentName().isEmpty());
    return disagreement;
}

qstest::Disagreement compareOverview(const NiriState& state, const QJsonObject& snapshot) {
    qstest::Disagreement disagreement;
    // The model's answer came from an event, this one from a request.
    disagreement.expectEqual(QStringLiteral("whether the overview is open"), state.isOverviewOpen(),
                             snapshot.value(QStringLiteral("is_open")).toBool());
    return disagreement;
}

using PayloadComparison = std::function<qstest::Disagreement(const QJsonValue&)>;

// Asks for one snapshot and compares it against the model. A request that is never answered, or refused,
// is not a disagreement about state — there is nothing to compare — so it comes back as a reading that was
// not taken and stops the loop rather than being retried as if it were a race.
qstest::Reading takeReading(NiriIPC& client, QStringView requestName, QStringView variantName,
                            const PayloadComparison& compare) {
    qstest::Reading reading;
    const std::optional<Reply> reply = requestSnapshot(client, requestName);
    if (!reply.has_value()) {
        reading.noReading = QStringLiteral("the compositor never answered a %1 request").arg(requestName);
        return reading;
    }
    if (!reply->isOk()) {
        reading.noReading = QStringLiteral("the compositor refused the %1 request: %2")
                                .arg(requestName, reply->describe());
        return reading;
    }

    const QJsonValue payload = reply->variant(variantName);
    reading.stamp = stampOf(payload);
    reading.mismatch = compare(payload).text();
    return reading;
}

}  // namespace

class NiriLiveStreamTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void receivesTheInitialStateWithoutAskingForIt();
    void theModelAgreesWithTheCompositorWorkspaceSnapshot();
    void theModelAgreesWithTheCompositorWindowSnapshot();
    void theModelAgreesWithTheCompositorOutputSnapshot();
    void theModelAgreesWithTheCompositorKeyboardLayouts();
    void theModelAgreesWithTheCompositorOverviewState();
    void theEventsItNowModelsAreNoLongerReportedAsGaps();
    void everyEventThatArrivedIsOneThisBuildKnows();

private:
    // One line per output, in the words the model holds: the readings, not a rendering of them.
    QString outputsSummary() const;

    // Reads `requestName` until the model and the payload `variantName` carries agree, failing with
    // `qstest::describeFailure` when they never do. Every comparison slot below is this call plus the
    // fields it compares.
    void expectSnapshotAgrees(QStringView what, QStringView requestName, QStringView variantName,
                              const PayloadComparison& compare);

    NiriIPC client_;
    // Outputs are the one part of the model niri sends no event for, so this service asks for them.
    NiriOutputs outputs_{client_};
    NiriEventStream stream_;
    NiriState state_;
    QString transportFailure_;
    QString streamFailure_;
    QString detectionFailure_;
    bool streaming_ = false;
    bool burstArrived_ = false;
    qint64 burstMilliseconds_ = 0;
};

void NiriLiveStreamTest::initTestCase() {
    if (niriSocketPath().isEmpty()) {
        QSKIP("$NIRI_SOCKET is not set: this test needs a running niri session");
    }

    // Observe before connecting, so the burst that follows the subscription is not missed.
    state_.observe(stream_);

    connect(&stream_, &NiriEventStream::transportError, this,
            [this](const QString& reason) { transportFailure_ = reason; });
    connect(&stream_, &NiriEventStream::streamFailed, this,
            [this](const QString& reason) { streamFailure_ = reason; });
    connect(&stream_, &NiriEventStream::streaming, this, [this] { streaming_ = true; });

    // The request connection is the independent path this test compares the model against.
    QSignalSpy requestConnected(&client_, &NiriIPC::connected);
    connect(&client_, &NiriIPC::transportError, this,
            [this](const QString& reason) { transportFailure_ = reason; });
    client_.connectToCompositor();
    QTRY_VERIFY_WITH_TIMEOUT(requestConnected.count() == 1 || !transportFailure_.isEmpty(), 5000);
    QVERIFY2(requestConnected.count() == 1,
             qPrintable(QStringLiteral("no request connection to %1: %2")
                            .arg(niriSocketPath(), transportFailure_)));

    // The outputs are asked for rather than streamed, because niri has no output event to stream.
    state_.observeOutputs(outputs_);
    outputs_.observe(stream_);

    QElapsedTimer timer;
    timer.start();
    stream_.connectToCompositor();
    // The first line after a subscription is the acknowledgement; the state that follows it arrives
    // unasked for, which is the whole point of the event stream.
    QTRY_VERIFY_WITH_TIMEOUT(streaming_ || !streamFailure_.isEmpty() || !transportFailure_.isEmpty(),
                             5000);
    QVERIFY2(streaming_, qPrintable(QStringLiteral("the stream never started: %1 %2")
                                        .arg(streamFailure_, transportFailure_)));
    QVERIFY2(QTest::qWaitFor([this] { return !state_.workspaces().isEmpty(); }, 10000),
             "the compositor sent no workspace state after the subscription");
    QVERIFY2(QTest::qWaitFor([this] { return !state_.outputs().isEmpty(); }, 10000),
             "the compositor's outputs never arrived");
    burstMilliseconds_ = timer.elapsed();
    burstArrived_ = true;

    qInfo("niri event stream on %s: workspace state %lld ms after subscribing",
          qPrintable(niriSocketPath()), static_cast<long long>(burstMilliseconds_));
    qInfo("model now holds %lld workspace(s), %lld window(s); focused workspace %llu, "
          "focused window %llu",
          static_cast<long long>(state_.workspaces().size()),
          static_cast<long long>(state_.windows().size()),
          static_cast<unsigned long long>(state_.focusedWorkspace().id()),
          static_cast<unsigned long long>(state_.focusedWindow().id()));
    qInfo("outputs: %s", qPrintable(outputsSummary()));
    qInfo("keyboard layouts: %s", qPrintable(state_.keyboardLayouts().describe()));
    qInfo("overview %s, last config load %s", state_.isOverviewOpen() ? "open" : "closed",
          state_.configLoadFailed() ? "failed" : "succeeded");
    qInfo("event names this build received but does not model: %s",
          stream_.unmodelledEvents().isEmpty()
              ? "none"
              : qPrintable(stream_.unmodelledEvents().join(QStringLiteral(", "))));
}

void NiriLiveStreamTest::receivesTheInitialStateWithoutAskingForIt() {
    QVERIFY(burstArrived_);
    QVERIFY(stream_.isConnected());
    QVERIFY(stream_.isStreaming());
    // A real session always has at least one workspace, and it arrived as an event rather than as an
    // answer to a request this test made.
    QVERIFY(!state_.workspaces().isEmpty());
    QVERIFY(state_.focusedWorkspace().isValid());
    QVERIFY(stream_.unknownEvents().isEmpty());
}

void NiriLiveStreamTest::expectSnapshotAgrees(QStringView what, QStringView requestName,
                                              QStringView variantName,
                                              const PayloadComparison& compare) {
    const qstest::ReconcileOutcome outcome = qstest::reconcile(
        [this, requestName, variantName, compare] {
            return takeReading(client_, requestName, variantName, compare);
        });

    // A reading that had to be taken again is the desktop changing between the two paths rather than a
    // defect, and saying so is what turns a race that was met into something a reader can see instead of
    // something that silently passed. Silent when the first reading agreed, which is the usual case.
    const QString reconciled = qstest::describeReconciliation(what.toString(), outcome);
    if (!reconciled.isEmpty()) {
        qInfo("%s", qPrintable(reconciled));
    }
    QVERIFY2(outcome.agreed, qPrintable(qstest::describeFailure(what.toString(), outcome)));
}

void NiriLiveStreamTest::theModelAgreesWithTheCompositorWorkspaceSnapshot() {
    expectSnapshotAgrees(QStringLiteral("the workspace list"), QStringLiteral("Workspaces"),
                         QStringLiteral("Workspaces"), [this](const QJsonValue& payload) {
                             return compareWorkspaces(state_, payload.toArray());
                         });
}

void NiriLiveStreamTest::theModelAgreesWithTheCompositorWindowSnapshot() {
    expectSnapshotAgrees(QStringLiteral("the window list"), QStringLiteral("Windows"),
                         QStringLiteral("Windows"), [this](const QJsonValue& payload) {
                             return compareWindows(state_, payload.toArray());
                         });
}

QString NiriLiveStreamTest::outputsSummary() const {
    QStringList lines;
    for (const NiriOutput& output : state_.outputs()) {
        lines.append(output.describe());
    }
    return lines.join(QStringLiteral(" | "));
}

void NiriLiveStreamTest::theModelAgreesWithTheCompositorOutputSnapshot() {
    expectSnapshotAgrees(QStringLiteral("the output list"), QStringLiteral("Outputs"),
                         QStringLiteral("Outputs"), [this](const QJsonValue& payload) {
                             return compareOutputs(state_, payload.toObject());
                         });
}

void NiriLiveStreamTest::theModelAgreesWithTheCompositorKeyboardLayouts() {
    expectSnapshotAgrees(QStringLiteral("the keyboard layouts"), QStringLiteral("KeyboardLayouts"),
                         QStringLiteral("KeyboardLayouts"), [this](const QJsonValue& payload) {
                             return compareKeyboardLayouts(state_, payload.toObject());
                         });
}

void NiriLiveStreamTest::theModelAgreesWithTheCompositorOverviewState() {
    expectSnapshotAgrees(QStringLiteral("whether the overview is open"),
                         QStringLiteral("OverviewState"), QStringLiteral("OverviewState"),
                         [this](const QJsonValue& payload) {
                             return compareOverview(state_, payload.toObject());
                         });
}

void NiriLiveStreamTest::theEventsItNowModelsAreNoLongerReportedAsGaps() {
    // The names below all arrive in the burst a subscription is answered with, and this build decodes
    // all of them. If the decoding were removed they would land in unmodelledEvents() instead.
    const QStringList gaps = stream_.unmodelledEvents();
    const QStringList modelled{QStringLiteral("WorkspacesChanged"), QStringLiteral("WindowsChanged"),
                               QStringLiteral("KeyboardLayoutsChanged"),
                               QStringLiteral("OverviewOpenedOrClosed"),
                               QStringLiteral("ConfigLoaded")};
    for (const QString& name : modelled) {
        QVERIFY2(!gaps.contains(name),
                 qPrintable(QStringLiteral("%1 arrived but was reported as not modelled")
                                .arg(name)));
    }

    // And the state those events feed is actually populated, not merely error-free.
    QVERIFY(!state_.workspaces().isEmpty());
    QVERIFY(state_.keyboardLayouts().isValid());
    QVERIFY(!state_.outputs().isEmpty());
}

void NiriLiveStreamTest::everyEventThatArrivedIsOneThisBuildKnows() {
    // Anything the compositor sent that this build does not decode is either a modelled gap (a
    // niri-ipc v26.04 event this build does not act on yet) or protocol drift. Both are reported by
    // the stream, and protocol drift is the one that must fail here: this build is written against
    // niri-ipc v26.04, which is the version floor.
    const QStringList unknown = stream_.unknownEvents();
    QVERIFY2(unknown.isEmpty(),
             qPrintable(QStringLiteral("niri sent events this build does not know: %1")
                            .arg(unknown.join(QStringLiteral(", ")))));

    // Every unmodelled name is an event of the protocol version this build targets, and the stream
    // recorded it rather than dropping it.
    for (const QString& name : stream_.unmodelledEvents()) {
        QVERIFY2(quantum::niri::isKnownEventName(name), qPrintable(name));
    }
}

QTEST_GUILESS_MAIN(NiriLiveStreamTest)
#include "niri_live_stream_test.moc"
