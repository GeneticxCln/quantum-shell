// Unit tests for the workspace and window state model.
//
// The model's inputs are public, so every transition is driven here directly, without a socket and
// without a compositor. The live test checks the same model against a real niri.
//
// The values come from NiriProtocolTestData.h: wire-shaped and synthetic, because a fixture cut from
// a real session would carry one machine's titles and output names into the repository.
#include "niri/NiriState.h"

#include "NiriProtocolTestData.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QTest>

using quantum::niri::NiriKeyboardLayouts;
using quantum::niri::NiriOutput;
using quantum::niri::NiriState;
using quantum::niri::NiriWindow;
using quantum::niri::NiriWorkspace;
using qstest::keyboardLayoutsObject;
using qstest::logicalObject;
using qstest::modeObject;
using qstest::outputObject;
using qstest::window;
using qstest::windowObject;
using qstest::workspace;

namespace {

// Counts the model's change signals. Declared after the state in each test, so it outlives it.
class SignalCounter {
public:
    explicit SignalCounter(NiriState& state) {
        QObject::connect(&state, &NiriState::workspacesChanged, &state, [this] { ++workspaces; });
        QObject::connect(&state, &NiriState::windowsChanged, &state, [this] { ++windows; });
        QObject::connect(&state, &NiriState::focusedWindowChanged, &state, [this] { ++focus; });
        QObject::connect(&state, &NiriState::outputsChanged, &state, [this] { ++outputs; });
        QObject::connect(&state, &NiriState::keyboardLayoutsChanged, &state,
                         [this] { ++layouts; });
        QObject::connect(&state, &NiriState::overviewChanged, &state, [this] { ++overview; });
        QObject::connect(&state, &NiriState::configLoadFailedChanged, &state,
                         [this] { ++configFailed; });
    }

    int workspaces = 0;
    int windows = 0;
    int focus = 0;
    int outputs = 0;
    int layouts = 0;
    int overview = 0;
    int configFailed = 0;
};

NiriOutput sampleOutput(const QString& name) {
    return NiriOutput::fromJson(outputObject(name, {modeObject(1920, 1080, 60000)}, QJsonValue(0),
                                            QJsonValue(logicalObject(0, 0, 1920, 1080, 1.0))));
}

NiriKeyboardLayouts sampleLayouts(const QStringList& names, int index) {
    return NiriKeyboardLayouts::fromJson(keyboardLayoutsObject(names, index));
}

}  // namespace

class NiriStateTest : public QObject {
    Q_OBJECT

private slots:
    void holdsTheWholeWorkspaceSetItIsGiven();
    void replacesRatherThanMergesTheWorkspaceSet();
    void ordersWorkspacesPerOutputByIndex();
    void holdsTheWholeWindowSetItIsGiven();
    void upsertsAnOpenedOrChangedWindow();
    void openingAFocusedWindowUnfocusesTheOthers();
    void focusedWindowUpdatesNotifyExactlyOnce();
    void closingTheFocusedWindowLeavesNothingFocused();
    void closingAnUnknownWindowChangesNothing();
    void focusMovesAndClears();
    void focusForAnUnknownWindowStillUnfocusesTheOthers();
    void urgencyChangesAreAppliedAndDeduplicated();
    void activatingAWorkspaceDeactivatesItsOutputOnly();
    void tracksTheActiveWindowOfAWorkspace();
    void dropsValuesWithoutAnId();
    void holdsTheOutputsItIsGiven();
    void followsTheKeyboardLayoutsAndASwitch();
    void tracksTheOverviewState();
    void tracksAConfigLoadFailure();
    void clearForgetsEverythingOnce();
    void clearForgetsTheNewPartsOnlyWhenTheyHoldSomething();
    void emitsOnlyWhenSomethingActuallyChanged();
};

void NiriStateTest::holdsTheWholeWorkspaceSetItIsGiven() {
    NiriState state;
    state.applyWorkspacesChanged({
        workspace(11, 1, QStringLiteral("OUT-A")),
        workspace(12, 2, QStringLiteral("OUT-A"), true, true),
        workspace(13, 1, QStringLiteral("OUT-B"), false, true),
    });

    const QList<NiriWorkspace> all = state.workspaces();
    QCOMPARE(all.size(), 3);
    QVERIFY(state.workspace(11).isValid());
    QVERIFY(!state.workspace(99).isValid());

    const NiriWorkspace focused = state.focusedWorkspace();
    QVERIFY(focused.isValid());
    QCOMPARE(focused.id(), quint64{12});
    QVERIFY(focused.isFocused());
    QVERIFY(focused.isActive());
    QVERIFY(!state.workspace(11).isFocused());

    QCOMPARE(state.workspacesOn(QStringLiteral("OUT-A")).size(), 2);
    QCOMPARE(state.workspacesOn(QStringLiteral("OUT-B")).size(), 1);
    QCOMPARE(state.workspacesOn(QStringLiteral("OUT-Z")).size(), 0);
}

void NiriStateTest::replacesRatherThanMergesTheWorkspaceSet() {
    NiriState state;
    state.applyWorkspacesChanged({workspace(1, 1, QStringLiteral("OUT-A")),
                                  workspace(2, 2, QStringLiteral("OUT-A"))});
    QCOMPARE(state.workspaces().size(), 2);

    // niri documents this event as a complete replacement: a workspace missing from it is gone.
    state.applyWorkspacesChanged({workspace(2, 1, QStringLiteral("OUT-A")),
                                  workspace(3, 2, QStringLiteral("OUT-A"))});

    QCOMPARE(state.workspaces().size(), 2);
    QVERIFY(!state.workspace(1).isValid());
    QVERIFY(state.workspace(2).isValid());
    QVERIFY(state.workspace(3).isValid());
    QCOMPARE(state.workspace(2).idx(), 1);
}

void NiriStateTest::ordersWorkspacesPerOutputByIndex() {
    NiriState state;
    state.applyWorkspacesChanged({
        workspace(1, 3, QStringLiteral("OUT-B")),
        workspace(2, 2, QStringLiteral("OUT-A")),
        workspace(3, 1, QStringLiteral("OUT-A")),
    });

    const QList<NiriWorkspace> ordered = state.workspaces();
    QCOMPARE(ordered.size(), 3);
    QCOMPARE(ordered.at(0).id(), quint64{3});  // OUT-A, index 1
    QCOMPARE(ordered.at(1).id(), quint64{2});  // OUT-A, index 2
    QCOMPARE(ordered.at(2).id(), quint64{1});  // OUT-B

    const QList<NiriWorkspace> a = state.workspacesOn(QStringLiteral("OUT-A"));
    QCOMPARE(a.size(), 2);
    QCOMPARE(a.at(0).id(), quint64{3});
    QCOMPARE(a.at(1).id(), quint64{2});
}

void NiriStateTest::holdsTheWholeWindowSetItIsGiven() {
    NiriState state;
    state.applyWorkspacesChanged({workspace(1, 1, QStringLiteral("OUT-A"))});
    state.applyWindowsChanged({window(7, QStringLiteral("app.one"), 1),
                               window(8, QStringLiteral("app.two"), 1, true)});

    QCOMPARE(state.windows().size(), 2);
    QCOMPARE(state.windowsOn(1).size(), 2);
    QCOMPARE(state.windowsOn(2).size(), 0);

    const NiriWindow focused = state.focusedWindow();
    QVERIFY(focused.isValid());
    QCOMPARE(focused.id(), quint64{8});
    QCOMPARE(focused.appId(), QStringLiteral("app.two"));
    QCOMPARE(state.window(7).title(), QStringLiteral("title of app.one"));

    // A replacement that drops one window drops it here too.
    state.applyWindowsChanged({window(7, QStringLiteral("app.one"), 1)});
    QCOMPARE(state.windows().size(), 1);
    QVERIFY(!state.window(8).isValid());
}

void NiriStateTest::upsertsAnOpenedOrChangedWindow() {
    NiriState state;
    state.applyWindowsChanged({window(4, QStringLiteral("app.one"), 1)});

    state.applyWindowOpenedOrChanged(window(5, QStringLiteral("app.two"), 1));
    QCOMPARE(state.windows().size(), 2);

    // The same id again is an update, not a second window.
    QJsonObject changed = windowObject(5, QStringLiteral("app.two"), 2);
    changed.insert(QStringLiteral("title"), QJsonValue(QStringLiteral("a new title")));
    state.applyWindowOpenedOrChanged(NiriWindow::fromJson(changed));

    QCOMPARE(state.windows().size(), 2);
    QCOMPARE(state.window(5).title(), QStringLiteral("a new title"));
    QCOMPARE(state.window(5).workspaceId().value_or(0), quint64{2});
    QCOMPARE(state.windowsOn(2).size(), 1);
}

void NiriStateTest::openingAFocusedWindowUnfocusesTheOthers() {
    NiriState state;
    state.applyWindowsChanged({window(1, QStringLiteral("app.one"), 1, true),
                               window(2, QStringLiteral("app.two"), 1)});
    QCOMPARE(state.focusedWindow().id(), quint64{1});
    SignalCounter counter(state);

    state.applyWindowOpenedOrChanged(window(3, QStringLiteral("app.three"), 1, true));

    QCOMPARE(state.focusedWindow().id(), quint64{3});
    QVERIFY(!state.window(1).isFocused());
    QVERIFY(!state.window(2).isFocused());
    QCOMPARE(counter.windows, 1);
    QCOMPARE(counter.focus, 1);
}

void NiriStateTest::focusedWindowUpdatesNotifyExactlyOnce() {
    NiriState state;
    SignalCounter counter(state);
    const NiriWindow focused = window(4, QStringLiteral("app.one"), 1, true);
    state.applyWindowOpenedOrChanged(focused);
    QCOMPARE(counter.windows, 1);
    QCOMPARE(counter.focus, 1);

    // A duplicate is not a property change.
    state.applyWindowOpenedOrChanged(focused);
    QCOMPARE(counter.windows, 1);
    QCOMPARE(counter.focus, 1);

    QJsonObject changed = windowObject(4, QStringLiteral("app.one"), 1, true);
    changed.insert(QStringLiteral("title"), QStringLiteral("changed focused title"));
    state.applyWindowOpenedOrChanged(NiriWindow::fromJson(changed));
    QCOMPARE(state.focusedWindow().title(), QStringLiteral("changed focused title"));
    QCOMPARE(counter.windows, 2);
    QCOMPARE(counter.focus, 2);

    // An unrelated window does not change the focused-window value.
    state.applyWindowOpenedOrChanged(window(5, QStringLiteral("app.two"), 1));
    QCOMPARE(counter.windows, 3);
    QCOMPARE(counter.focus, 2);

    changed.insert(QStringLiteral("is_focused"), false);
    state.applyWindowOpenedOrChanged(NiriWindow::fromJson(changed));
    QVERIFY(!state.focusedWindow().isValid());
    QCOMPARE(counter.windows, 4);
    QCOMPARE(counter.focus, 3);
}

void NiriStateTest::closingTheFocusedWindowLeavesNothingFocused() {
    NiriState state;
    state.applyWindowsChanged({window(1, QStringLiteral("app.one"), 1),
                               window(2, QStringLiteral("app.two"), 1, true)});
    SignalCounter counter(state);

    state.applyWindowClosed(2);

    QVERIFY(!state.focusedWindow().isValid());
    QVERIFY(!state.window(1).isFocused());
    QCOMPARE(state.windows().size(), 1);
    QCOMPARE(counter.windows, 1);
    QCOMPARE(counter.focus, 1);
    state.applyWindowClosed(2);
    QCOMPARE(counter.windows, 1);
    QCOMPARE(counter.focus, 1);
}

void NiriStateTest::closingAnUnknownWindowChangesNothing() {
    NiriState state;
    state.applyWindowsChanged({window(1, QStringLiteral("app.one"), 1, true)});
    SignalCounter counter(state);

    state.applyWindowClosed(77);

    QCOMPARE(state.windows().size(), 1);
    QCOMPARE(state.focusedWindow().id(), quint64{1});
    QCOMPARE(counter.windows, 0);
    QCOMPARE(counter.focus, 0);
}

void NiriStateTest::focusMovesAndClears() {
    NiriState state;
    state.applyWindowsChanged({window(1, QStringLiteral("app.one"), 1),
                               window(2, QStringLiteral("app.two"), 1)});

    state.applyFocusedWindowChanged(2, true);
    QCOMPARE(state.focusedWindow().id(), quint64{2});
    QVERIFY(!state.window(1).isFocused());

    // niri reports "no window focused" as a null id, which is a real state: a layer-shell surface
    // can hold focus instead.
    state.applyFocusedWindowChanged(0, false);
    QVERIFY(!state.focusedWindow().isValid());
    QVERIFY(!state.window(2).isFocused());
}

void NiriStateTest::focusForAnUnknownWindowStillUnfocusesTheOthers() {
    NiriState state;
    state.applyWindowsChanged({window(1, QStringLiteral("app.one"), 1, true)});

    // The compositor said window 99 is the focused one. This model has no window 99, but it still
    // knows window 1 is not focused any more; keeping the old focus would be the lie.
    state.applyFocusedWindowChanged(99, true);

    QVERIFY(!state.focusedWindow().isValid());
    QVERIFY(!state.window(1).isFocused());
}

void NiriStateTest::urgencyChangesAreAppliedAndDeduplicated() {
    NiriState state;
    state.applyWindowsChanged({window(1, QStringLiteral("app.one"), 1),
                               window(2, QStringLiteral("app.two"), 1)});
    state.applyWorkspacesChanged({workspace(1, 1, QStringLiteral("OUT-A"))});
    SignalCounter counter(state);

    state.applyWindowUrgencyChanged(2, true);
    QVERIFY(state.window(2).isUrgent());
    QVERIFY(!state.window(1).isUrgent());
    QCOMPARE(counter.windows, 1);

    // The same value again is not a change.
    state.applyWindowUrgencyChanged(2, true);
    QCOMPARE(counter.windows, 1);

    state.applyWorkspaceUrgencyChanged(1, true);
    QVERIFY(state.workspace(1).isUrgent());
    QCOMPARE(counter.workspaces, 1);
    state.applyWorkspaceUrgencyChanged(1, true);
    QCOMPARE(counter.workspaces, 1);

    state.applyWindowUrgencyChanged(2, false);
    QVERIFY(!state.window(2).isUrgent());
    QCOMPARE(counter.windows, 2);
}

void NiriStateTest::activatingAWorkspaceDeactivatesItsOutputOnly() {
    NiriState state;
    state.applyWorkspacesChanged({
        workspace(1, 1, QStringLiteral("OUT-A"), true, true),
        workspace(2, 2, QStringLiteral("OUT-A")),
        workspace(3, 1, QStringLiteral("OUT-B"), false, true),
    });

    state.applyWorkspaceActivated(2, true);

    QVERIFY(!state.workspace(1).isActive());
    QVERIFY(!state.workspace(1).isFocused());
    QVERIFY(state.workspace(2).isActive());
    QVERIFY(state.workspace(2).isFocused());
    QCOMPARE(state.focusedWorkspace().id(), quint64{2});

    // The other output keeps its own active workspace: activation is per output.
    QVERIFY(state.workspace(3).isActive());
}

void NiriStateTest::tracksTheActiveWindowOfAWorkspace() {
    NiriState state;
    state.applyWorkspacesChanged({workspace(1, 1, QStringLiteral("OUT-A"))});
    SignalCounter counter(state);

    state.applyWorkspaceActiveWindowChanged(1, 7, true);
    QVERIFY(state.workspace(1).activeWindowId().has_value());
    QCOMPARE(*state.workspace(1).activeWindowId(), quint64{7});
    QCOMPARE(counter.workspaces, 1);

    // niri reports "no active window" as null.
    state.applyWorkspaceActiveWindowChanged(1, 0, false);
    QVERIFY(!state.workspace(1).activeWindowId().has_value());
    QCOMPARE(counter.workspaces, 2);

    // An event for a workspace this model does not have is ignored, not invented.
    state.applyWorkspaceActiveWindowChanged(42, 9, true);
    QCOMPARE(counter.workspaces, 2);
}

void NiriStateTest::dropsValuesWithoutAnId() {
    NiriState state;
    QJsonObject anonymous = windowObject(5, QStringLiteral("app.one"), 1);
    anonymous.remove(QStringLiteral("id"));
    const NiriWindow broken = NiriWindow::fromJson(anonymous);
    QVERIFY(!broken.isValid());

    state.applyWindowsChanged({broken, window(6, QStringLiteral("app.two"), 1)});
    QCOMPARE(state.windows().size(), 1);

    state.applyWindowOpenedOrChanged(broken);
    QCOMPARE(state.windows().size(), 1);

    state.applyWorkspacesChanged({NiriWorkspace::fromJson(QJsonObject{}),
                                  workspace(2, 1, QStringLiteral("OUT-A"))});
    QCOMPARE(state.workspaces().size(), 1);
    QVERIFY(state.workspace(2).isValid());
}

void NiriStateTest::holdsTheOutputsItIsGiven() {
    NiriState state;
    SignalCounter counter(state);

    state.applyOutputsChanged({sampleOutput(QStringLiteral("DP-3")),
                               sampleOutput(QStringLiteral("HDMI-A-1"))});
    QCOMPARE(state.outputs().size(), 2);
    // Ordered by connector name, so the order is stable for a given compositor state.
    QCOMPARE(state.outputs().at(0).name(), QStringLiteral("DP-3"));
    QCOMPARE(state.outputs().at(1).name(), QStringLiteral("HDMI-A-1"));
    QVERIFY(state.output(QStringLiteral("DP-3")).isValid());
    QVERIFY(!state.output(QStringLiteral("DP-9")).isValid());
    QCOMPARE(counter.outputs, 1);

    // A snapshot replaces the set: an output missing from it is gone.
    state.applyOutputsChanged({sampleOutput(QStringLiteral("HDMI-A-1"))});
    QCOMPARE(state.outputs().size(), 1);
    QVERIFY(!state.output(QStringLiteral("DP-3")).isValid());
    QCOMPARE(counter.outputs, 2);

    // The same snapshot again is not a change.
    state.applyOutputsChanged({sampleOutput(QStringLiteral("HDMI-A-1"))});
    QCOMPARE(counter.outputs, 2);

    // An output without a name is not an output, and does not displace the one that has a name.
    state.applyOutputsChanged({NiriOutput{}, sampleOutput(QStringLiteral("DP-3"))});
    QCOMPARE(state.outputs().size(), 1);
    QCOMPARE(state.outputs().at(0).name(), QStringLiteral("DP-3"));
}

void NiriStateTest::followsTheKeyboardLayoutsAndASwitch() {
    NiriState state;
    SignalCounter counter(state);

    // Nothing is known until the compositor reports it.
    QVERIFY(!state.keyboardLayouts().isValid());

    state.applyKeyboardLayoutsChanged(sampleLayouts({QStringLiteral("us"), QStringLiteral("de")}, 0));
    QVERIFY(state.keyboardLayouts().isValid());
    QCOMPARE(state.keyboardLayouts().currentName(), QStringLiteral("us"));
    QCOMPARE(counter.layouts, 1);

    state.applyKeyboardLayoutSwitched(1);
    QCOMPARE(state.keyboardLayouts().currentName(), QStringLiteral("de"));
    QCOMPARE(counter.layouts, 2);

    // The same index again is not a change.
    state.applyKeyboardLayoutSwitched(1);
    QCOMPARE(counter.layouts, 2);

    // A switch is a switch even when the index names nothing known: the index is a reading, and the
    // lack of a name is reported by currentName() rather than by inventing a layout.
    state.applyKeyboardLayoutSwitched(9);
    QCOMPARE(counter.layouts, 3);
    QVERIFY(state.keyboardLayouts().currentName().isEmpty());

    // Before any names are known there is nothing for a switch to point into, so it changes nothing.
    NiriState fresh;
    SignalCounter freshCounter(fresh);
    fresh.applyKeyboardLayoutSwitched(1);
    QVERIFY(!fresh.keyboardLayouts().isValid());
    QCOMPARE(freshCounter.layouts, 0);

    // Layouts that cannot be read are refused rather than stored as an empty layout list.
    state.applyKeyboardLayoutsChanged(NiriKeyboardLayouts{});
    QVERIFY(state.keyboardLayouts().isValid());
    QCOMPARE(counter.layouts, 3);
}

void NiriStateTest::tracksTheOverviewState() {
    NiriState state;
    SignalCounter counter(state);

    QVERIFY(!state.isOverviewOpen());
    state.applyOverviewOpenedOrClosed(true);
    QVERIFY(state.isOverviewOpen());
    QCOMPARE(counter.overview, 1);

    state.applyOverviewOpenedOrClosed(true);
    QCOMPARE(counter.overview, 1);

    state.applyOverviewOpenedOrClosed(false);
    QVERIFY(!state.isOverviewOpen());
    QCOMPARE(counter.overview, 2);
}

void NiriStateTest::tracksAConfigLoadFailure() {
    NiriState state;
    SignalCounter counter(state);

    QVERIFY(!state.configLoadFailed());
    state.applyConfigLoaded(true);
    QVERIFY(state.configLoadFailed());
    QCOMPARE(counter.configFailed, 1);

    state.applyConfigLoaded(true);
    QCOMPARE(counter.configFailed, 1);

    // The flag is the last reported load, not a history: a later success clears it.
    state.applyConfigLoaded(false);
    QVERIFY(!state.configLoadFailed());
    QCOMPARE(counter.configFailed, 2);
}

void NiriStateTest::clearForgetsTheNewPartsOnlyWhenTheyHoldSomething() {
    NiriState state;
    state.applyOutputsChanged({sampleOutput(QStringLiteral("DP-3"))});
    state.applyKeyboardLayoutsChanged(sampleLayouts({QStringLiteral("us")}, 0));
    state.applyOverviewOpenedOrClosed(true);
    state.applyConfigLoaded(true);
    SignalCounter counter(state);

    state.clear();

    QVERIFY(state.outputs().isEmpty());
    QVERIFY(!state.keyboardLayouts().isValid());
    QVERIFY(!state.isOverviewOpen());
    QVERIFY(!state.configLoadFailed());
    QCOMPARE(counter.outputs, 1);
    QCOMPARE(counter.layouts, 1);
    QCOMPARE(counter.overview, 1);
    QCOMPARE(counter.configFailed, 1);
    // A part that held nothing is not reported, so the shell's workspace bindings are not woken by a
    // clear that did not touch them.
    QCOMPARE(counter.workspaces, 0);
    QCOMPARE(counter.windows, 0);

    // Nothing left to forget: no second round of signals.
    state.clear();
    QCOMPARE(counter.outputs, 1);
    QCOMPARE(counter.layouts, 1);
    QCOMPARE(counter.overview, 1);
    QCOMPARE(counter.configFailed, 1);
}

void NiriStateTest::clearForgetsEverythingOnce() {
    NiriState state;
    state.applyWorkspacesChanged({workspace(1, 1, QStringLiteral("OUT-A"))});
    state.applyWindowsChanged({window(1, QStringLiteral("app.one"), 1, true)});
    SignalCounter counter(state);

    state.clear();

    QVERIFY(state.workspaces().isEmpty());
    QVERIFY(state.windows().isEmpty());
    QVERIFY(!state.focusedWindow().isValid());
    QCOMPARE(counter.workspaces, 1);
    QCOMPARE(counter.windows, 1);

    // Nothing left to forget: no second round of signals.
    state.clear();
    QCOMPARE(counter.workspaces, 1);
    QCOMPARE(counter.windows, 1);
}

void NiriStateTest::emitsOnlyWhenSomethingActuallyChanged() {
    NiriState state;
    const QList<NiriWorkspace> workspaces{workspace(1, 1, QStringLiteral("OUT-A"), true, true),
                                          workspace(2, 2, QStringLiteral("OUT-A"))};
    const QList<NiriWindow> windows{window(1, QStringLiteral("app.one"), 1, true)};

    SignalCounter counter(state);
    state.applyWorkspacesChanged(workspaces);
    state.applyWindowsChanged(windows);
    QCOMPARE(counter.workspaces, 1);
    QCOMPARE(counter.windows, 1);
    // No window was focused before this set and window 1 is in it, so focus did change here.
    QCOMPARE(counter.focus, 1);

    // The same state arriving again changes nothing, so nothing is emitted: a consumer bound to
    // these signals is not woken for an event that carried no news.
    state.applyWorkspacesChanged(workspaces);
    state.applyWindowsChanged(windows);
    QCOMPARE(counter.workspaces, 1);
    QCOMPARE(counter.windows, 1);
    QCOMPARE(counter.focus, 1);

    // A real change still gets through.
    state.applyWorkspacesChanged({workspace(1, 1, QStringLiteral("OUT-A"), true, true, QStringLiteral("main")),
                                  workspace(2, 2, QStringLiteral("OUT-A"))});
    QCOMPARE(counter.workspaces, 2);
}

QTEST_GUILESS_MAIN(NiriStateTest)
#include "niri_state_test.moc"
