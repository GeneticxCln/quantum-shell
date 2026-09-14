// The QML-facing service, driven without a compositor and without a display.
//
// Two things are being tested that a Q_PROPERTY list alone does not show: that the values QML reads
// are the model's real state (driven here through the event stream and the fake compositor, not by
// calling the service), and that a binding written in QML actually follows the notify signals. The
// second is the one that fails silently in production if a NOTIFY signal is missing, so it is checked
// by evaluating a real binding in a real QML engine rather than by watching signals on the C++ side.
#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"
#include "niri/NiriOutputs.h"
#include "niri/NiriProtocol.h"
#include "niri/NiriQmlModule.h"
#include "niri/NiriService.h"
#include "niri/NiriServiceKeys.h"
#include "niri/NiriState.h"

#include "FakeNiriServer.h"
#include "NiriProtocolTestData.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>

#include <array>
#include <memory>
#include <string_view>

using quantum::niri::NiriEventStream;
using quantum::niri::NiriIPC;
using quantum::niri::NiriOutputs;
using quantum::niri::NiriService;
using quantum::niri::NiriState;

namespace qml = quantum::niri::qml;

namespace {

// The QML-visible key sets, written out here independently of `NiriServiceKeys.h` and compared with it at
// compile time. The duplication is the point, not an accident: adding, removing or renaming a key in
// `src/niri/` changes what QML sees, so it must not be possible to do it without coming here — the
// service simply will not build until this file agrees. These lists are what the key-set comparisons in
// `everyKeyTheServiceCanEmitIsTheOneAWidgetReads` are written against, and `NiriServiceKeys.h` is not:
// the test states the interface it requires rather than asking the service what it emits.
//
// Two halves, then, and neither is sufficient alone. The compile-time half makes a change to the key set
// impossible to make silently. The runtime half compares the key sets the service really produced
// against these lists, which is what catches a key written into a map as a string literal that never
// appears in `NiriServiceKeys.h` — a change the compile-time half cannot see.
constexpr std::array<const char*, 7> coveredWorkspaceKeys{qml::WorkspaceId,      qml::WorkspaceIdx,
                                                          qml::WorkspaceName,    qml::WorkspaceOutput,
                                                          qml::WorkspaceIsActive, qml::WorkspaceIsFocused,
                                                          qml::WorkspaceIsUrgent};
constexpr std::array<const char*, 1> coveredWorkspaceOptionalKeys{qml::WorkspaceActiveWindowId};

constexpr std::array<const char*, 6> coveredWindowKeys{qml::WindowId,        qml::WindowTitle,
                                                       qml::WindowAppId,     qml::WindowIsFocused,
                                                       qml::WindowIsFloating, qml::WindowIsUrgent};
constexpr std::array<const char*, 1> coveredWindowOptionalKeys{qml::WindowWorkspaceId};

constexpr std::array<const char*, 5> coveredOutputKeys{qml::OutputName,    qml::OutputMake,
                                                       qml::OutputModel,   qml::OutputIsEnabled,
                                                       qml::OutputIsVrrEnabled};
constexpr std::array<const char*, 6> coveredOutputGeometryKeys{qml::OutputX,       qml::OutputY,
                                                               qml::OutputWidth,   qml::OutputHeight,
                                                               qml::OutputScale,   qml::OutputTransform};
constexpr std::array<const char*, 3> coveredOutputModeKeys{qml::OutputModeWidth, qml::OutputModeHeight,
                                                          qml::OutputRefreshRate};

constexpr std::array<const char*, 3> coveredKeyboardLayoutKeys{qml::KeyboardLayoutNames,
                                                              qml::KeyboardLayoutCurrentIndex,
                                                              qml::KeyboardLayoutCurrentName};

template <std::size_t Declared, std::size_t Covered>
consteval bool sameKeys(const std::array<const char*, Declared>& declared,
                        const std::array<const char*, Covered>& covered) {
    if (Declared != Covered) {
        return false;
    }
    for (std::size_t index = 0; index < Declared; ++index) {
        if (std::string_view(declared[index]) != std::string_view(covered[index])) {
            return false;
        }
    }
    return true;
}

static_assert(sameKeys(qml::WorkspaceKeys, coveredWorkspaceKeys),
              "the workspace key set changed: update the list above, and say what the new key's value must be");
static_assert(sameKeys(qml::WorkspaceOptionalKeys, coveredWorkspaceOptionalKeys),
              "the optional workspace key set changed: update the list above and the absence assertion");
static_assert(sameKeys(qml::WindowKeys, coveredWindowKeys),
              "the window key set changed: update the list above, and say what the new key's value must be");
static_assert(sameKeys(qml::WindowOptionalKeys, coveredWindowOptionalKeys),
              "the optional window key set changed: update the list above and the absence assertion");
static_assert(sameKeys(qml::OutputKeys, coveredOutputKeys),
              "the output key set changed: update the list above, and say what the new key's value must be");
static_assert(sameKeys(qml::OutputGeometryKeys, coveredOutputGeometryKeys),
              "the output geometry key set changed: update the list above and the disabled-output assertion");
static_assert(sameKeys(qml::OutputModeKeys, coveredOutputModeKeys),
              "the output mode key set changed: update the list above and the disabled-output assertion");
static_assert(sameKeys(qml::KeyboardLayoutKeys, coveredKeyboardLayoutKeys),
              "the keyboard layout key set changed: update the list above, and assert the new key's value");

// The QML module's own identity, mirrored the same way and for the same reason. These names are the ones
// a QML file is written against — the import line and the type it looks the singleton up by — so a
// rename breaks every file that already says `import QuantumShell 1.0`, at load time, in a component
// this repository does not have yet. There is no emitted value to compare at runtime, so the mirror and
// these assertions are the whole guard: the registration will not build until a rename is acknowledged
// here, and the binding test below then imports those mirrored names for real.
constexpr auto coveredModuleUri = "QuantumShell";
constexpr int coveredModuleMajorVersion = 1;
constexpr int coveredModuleMinorVersion = 0;
constexpr auto coveredServiceTypeName = "NiriService";

static_assert(std::string_view(qml::ModuleUri) == std::string_view(coveredModuleUri),
              "the QML module URI changed: update the mirror above, and every QML file that imports it");
static_assert(qml::ModuleMajorVersion == coveredModuleMajorVersion, "the QML module major version changed");
static_assert(qml::ModuleMinorVersion == coveredModuleMinorVersion,
              "the QML module minor version changed: a major bump breaks existing imports, a minor one "
              "widens them, and both are a decision worth stating");
static_assert(std::string_view(qml::ServiceTypeName) == std::string_view(coveredServiceTypeName),
              "the QML service type name changed: update the mirror above, and every binding that reads it");

template <std::size_t Count>
QStringList keysOf(const std::array<const char*, Count>& keys) {
    QStringList list;
    list.reserve(static_cast<qsizetype>(Count));
    for (const char* key : keys) {
        list << QString::fromLatin1(key);
    }
    return list;
}

// The keys a map is expected to carry, from one or more of the declared groups, sorted so the comparison
// is about the set and not about the order the service happened to insert them in.
template <std::size_t... Counts>
QStringList expectedKeys(const std::array<const char*, Counts>&... groups) {
    QStringList list;
    ((list << keysOf(groups)), ...);
    list.sort();
    return list;
}

QStringList sortedKeys(const QVariantMap& map) {
    QStringList list = map.keys();
    list.sort();
    return list;
}

// One output's map out of the list, found by name. An empty map means no output with that name, which is
// what the waits use: a name no earlier test used, so the wait waits for this test's reply.
QVariantMap outputNamed(const QVariantList& outputs, const QString& name) {
    for (const QVariant& entry : outputs) {
        const QVariantMap map = entry.toMap();
        if (map.value(qml::OutputName).toString() == name) {
            return map;
        }
    }
    return {};
}

// What a failed whole-map comparison needs to say: which key is missing, which one is unexpected, and
// which one holds the wrong value. `QCOMPARE` on a `QVariantMap` prints the type and nothing useful.
QString mapDiff(const QVariantMap& actual, const QVariantMap& expected) {
    QStringList lines;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        if (!actual.contains(it.key())) {
            lines << QStringLiteral("missing %1").arg(it.key());
        } else if (actual.value(it.key()) != it.value()) {
            lines << QStringLiteral("%1 is %2, expected %3")
                         .arg(it.key(), actual.value(it.key()).toString(), it.value().toString());
        }
    }
    for (auto it = actual.constBegin(); it != actual.constEnd(); ++it) {
        if (!expected.contains(it.key())) {
            lines << QStringLiteral("unexpected %1 (%2)")
                         .arg(it.key(), it.value().toString());
        }
    }
    return lines.join(QStringLiteral("; "));
}

}  // namespace

class NiriServiceTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void nothingIsReportedUntilTheCompositorSaysSomething();
    void outputsAreAskedForAtOnceBecauseNiriDoesNotStreamThem();
    void theWorkspaceStripFollowsTheEventStream();
    void theFocusedWindowFollowsFocusEvents();
    void outputsAndKeyboardLayoutFollowTheCompositor();
    void everyValueQmlReadsComesFromTheFieldItNames();
    void everyKeyTheServiceCanEmitIsTheOneAWidgetReads();
    void theQmlPropertiesAreTheOnesABindingCanFollow();
    void anEventThatChangesNothingDoesNotWakeABinding();
    void aQmlBindingFollowsTheServicesNotifySignals();

private:
    // One event line to a given connection, counted from zero in acceptance order: the shared request
    // connection is opened first, so the shared event stream is the second. A slot that opens a connection
    // of its own addresses that one instead of the shared stream.
    void pushEventTo(int connection, const QString& name, const QJsonObject& fields = {});
    void pushEvent(const QString& name, const QJsonObject& fields = {});
    void pushWorkspaces(const QList<QJsonObject>& workspaces);

    FakeNiriServer server_;
    NiriIPC request_;
    NiriEventStream stream_;
    NiriState state_;
    std::unique_ptr<NiriOutputs> outputs_;
    NiriService service_{state_, stream_};
};

void NiriServiceTest::initTestCase() {
    QVERIFY2(server_.listen(), qPrintable(server_.serverError()));
    server_.setReply(QStringLiteral("EventStream"), QByteArrayLiteral("{\"Ok\":\"Handled\"}"));

    // One real output, with a fractional scale and a millihertz refresh rate, so the mapping is
    // checked against values an integer assumption would visibly break.
    server_.setReply(QStringLiteral("Outputs"),
                     qstest::outputsReplyLine({qstest::outputObject(
                         QStringLiteral("DP-3"),
                         {qstest::modeObject(3840, 2160, 119880)}, qstest::number(0),
                         qstest::logicalObject(0, 0, 3072, 1728, 1.25))}));

    state_.observe(stream_);
    outputs_ = std::make_unique<NiriOutputs>(request_);
    state_.observeOutputs(*outputs_);
    outputs_->observe(stream_);

    QSignalSpy streaming(&stream_, &NiriEventStream::streaming);
    request_.connectToCompositor(server_.path());
    stream_.connectToCompositor(server_.path());
    QTRY_VERIFY_WITH_TIMEOUT(streaming.count() == 1, 5000);
    QCOMPARE(request_.isConnected(), true);
}

void NiriServiceTest::pushEventTo(int connection, const QString& name, const QJsonObject& fields) {
    server_.writeRawTo(connection, qstest::eventLine(name, fields));
}

void NiriServiceTest::pushEvent(const QString& name, const QJsonObject& fields) {
    pushEventTo(1, name, fields);
}

void NiriServiceTest::pushWorkspaces(const QList<QJsonObject>& workspaces) {
    pushEvent(QStringLiteral("WorkspacesChanged"),
              QJsonObject{{QStringLiteral("workspaces"), qstest::array(workspaces)}});
}

void NiriServiceTest::nothingIsReportedUntilTheCompositorSaysSomething() {
    // A service built over a model that has been told nothing, constructed here rather than relying on this
    // slot running first: "nothing has arrived yet" is a property of a fresh service, not of a position in
    // the suite. Asserting it on the shared service would only hold while this slot was the first declared —
    // which is what the order-independence check caught.
    NiriState fresh;
    fresh.observe(stream_);
    const NiriService service(fresh, stream_);

    // Each property is empty or false, and none of them is a plausible stand-in: an empty map has no keys,
    // so a widget cannot mistake it for a reading. Outputs are the one exception, and the next slot is
    // about them.
    QCOMPARE(service.workspaces(), QVariantList{});
    QCOMPARE(service.focusedWindow(), QVariantMap{});
    QCOMPARE(service.keyboardLayout(), QVariantMap{});
    QCOMPARE(service.overviewOpen(), false);
    // The subscription is acknowledged, so this one is true and the emptiness above is the compositor's own
    // answer rather than a broken connection.
    QCOMPARE(service.connected(), true);

    // The shared service took the other path into the same property: it was constructed before the
    // compositor was connected, so `connected` became true when the subscription was acknowledged rather
    // than being seeded. That is true whatever order the slots run in, and it is the only thing this slot
    // still asserts about the shared fixtures.
    QCOMPARE(service_.connected(), true);
}

void NiriServiceTest::outputsAreAskedForAtOnceBecauseNiriDoesNotStreamThem() {
    // Outputs are the exception, and honestly so: niri has no output event, so `NiriOutputs` asks for them
    // as soon as it observes the stream rather than waiting for one. What QML sees is therefore the
    // compositor's answer from the start.
    //
    // The whole chain is built here — its own reply, its own outputs refresh, its own state and service —
    // because the claim is about a service created while the compositor is connected. A service that
    // inherited whatever the shared fixtures happened to hold would be asserting about those instead.
    server_.setReply(QStringLiteral("Outputs"),
                     qstest::outputsReplyLine({qstest::outputObject(
                         QStringLiteral("DP-3"),
                         {qstest::modeObject(3840, 2160, 119880)}, qstest::number(0),
                         qstest::logicalObject(0, 0, 3072, 1728, 1.25))}));

    NiriState fresh;
    fresh.observe(stream_);
    NiriOutputs outputs(request_);
    fresh.observeOutputs(outputs);
    outputs.observe(stream_);
    const NiriService service(fresh, stream_);

    QTRY_COMPARE(service.outputs().size(), 1);
    const QVariantMap output = service.outputs().first().toMap();
    QCOMPARE(output.value("name").toString(), QStringLiteral("DP-3"));
    QCOMPARE(output.value("isEnabled").toBool(), true);
    QCOMPARE(output.value("width").toInt(), 3072);
    QCOMPARE(output.value("height").toInt(), 1728);
    // Fractional, kept as niri reported it: a widget that assumed an integer scale would be wrong here.
    QCOMPARE(output.value("scale").toDouble(), 1.25);
    QCOMPARE(output.value("transform").toString(), QStringLiteral("Normal"));
    // Millihertz, so 119.88 Hz is not rounded into 120.
    QCOMPARE(output.value("refreshRate").toUInt(), 119880u);
}

void NiriServiceTest::theWorkspaceStripFollowsTheEventStream() {
    QSignalSpy changed(&service_, &NiriService::workspacesChanged);

    pushWorkspaces({
        qstest::workspaceObject(7, 2, QStringLiteral("DP-3"), true, true),
        qstest::workspaceObject(6, 1, QStringLiteral("DP-3")),
        qstest::workspaceObject(11, 3, QStringLiteral("DP-3"), false, false, QStringLiteral("browser")),
    });
    // The id only this slot's event carries, not `size() == 3`: a count of workspaces can be reached by
    // another slot's leftovers before this event has been read, and then the spy count below would be
    // checked against a change that had not happened yet.
    QTRY_VERIFY_WITH_TIMEOUT(
        !service_.workspaces().isEmpty()
            && service_.workspaces().at(0).toMap().value(qml::WorkspaceId).toString()
                   == QStringLiteral("6"),
        5000);
    QCOMPARE(changed.count(), 1);

    // What QML reads, in the model's display order: by output, then by index on that output.
    const QVariantList strip = service_.workspaces();
    QCOMPARE(strip.at(0).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("6"));
    QCOMPARE(strip.at(0).toMap().value(QStringLiteral("idx")).toInt(), 1);
    QCOMPARE(strip.at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("7"));
    QCOMPARE(strip.at(1).toMap().value(QStringLiteral("isFocused")).toBool(), true);
    QCOMPARE(strip.at(1).toMap().value(QStringLiteral("isActive")).toBool(), true);
    QCOMPARE(strip.at(1).toMap().value(QStringLiteral("output")).toString(), QStringLiteral("DP-3"));
    QCOMPARE(strip.at(2).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("browser"));

    // An id is passed as text, because a random 64-bit id does not survive QML's double.
    QVERIFY(strip.at(0).toMap().value(QStringLiteral("id")).typeId() == QMetaType::QString);

    // The same set again is not news: the strip QML holds is the same strip, so no binding is woken.
    pushWorkspaces({
        qstest::workspaceObject(7, 2, QStringLiteral("DP-3"), true, true),
        qstest::workspaceObject(6, 1, QStringLiteral("DP-3")),
        qstest::workspaceObject(11, 3, QStringLiteral("DP-3"), false, false, QStringLiteral("browser")),
    });
    QTest::qWait(60);
    QCOMPARE(changed.count(), 1);

    // A real change does wake it, and focus moves without a second workspace being focused.
    pushEvent(QStringLiteral("WorkspaceActivated"), QJsonObject{{QStringLiteral("id"), qstest::number(6)},
                                                               {QStringLiteral("focused"), true}});
    QTRY_COMPARE(changed.count(), 2);
    const QVariantList after = service_.workspaces();
    QCOMPARE(after.at(0).toMap().value(QStringLiteral("isFocused")).toBool(), true);
    QCOMPARE(after.at(1).toMap().value(QStringLiteral("isFocused")).toBool(), false);
}

void NiriServiceTest::theFocusedWindowFollowsFocusEvents() {
    // Nothing focused first, established by a wait on the value rather than on a count. The push below is
    // a change only if something was focused before it, and a slot that ran earlier can leave exactly this
    // window focused — in which case nothing moves, no signal fires, and a count assertion fails against a
    // service that behaved correctly. One shuffled order produced precisely that.
    pushEvent(QStringLiteral("WindowFocusChanged"), QJsonObject{{QStringLiteral("id"), QJsonValue()}});
    QTRY_VERIFY(service_.focusedWindow().isEmpty());

    QSignalSpy changed(&service_, &NiriService::focusedWindowChanged);

    pushEvent(QStringLiteral("WindowsChanged"),
              QJsonObject{{QStringLiteral("windows"),
                           qstest::array({qstest::windowObject(13, QStringLiteral("google-chrome"), 7, true),
                                          qstest::windowObject(14, QStringLiteral("editor"), 7)})}});
    QTRY_COMPARE(changed.count(), 1);

    const QVariantMap window = service_.focusedWindow();
    QCOMPARE(window.value(QStringLiteral("id")).toString(), QStringLiteral("13"));
    QCOMPARE(window.value(QStringLiteral("appId")).toString(), QStringLiteral("google-chrome"));
    QCOMPARE(window.value(QStringLiteral("title")).toString(),
             QStringLiteral("title of google-chrome"));
    QCOMPARE(window.value(QStringLiteral("isFocused")).toBool(), true);
    QCOMPARE(window.value(QStringLiteral("workspaceId")).toString(), QStringLiteral("7"));

    // Focus going nowhere is a real state, and it is reported as an empty map rather than as the last
    // window, which would leave a bar showing a title for a window that is not focused. Counting this one
    // is safe because the slot started from nothing focused, so it is the second real change.
    pushEvent(QStringLiteral("WindowFocusChanged"), QJsonObject{{QStringLiteral("id"), QJsonValue()}});
    QTRY_VERIFY(service_.focusedWindow().isEmpty());
    QCOMPARE(changed.count(), 2);
}

void NiriServiceTest::outputsAndKeyboardLayoutFollowTheCompositor() {
    QSignalSpy outputChanges(&service_, &NiriService::outputsChanged);
    QSignalSpy layoutChanges(&service_, &NiriService::keyboardLayoutChanged);

    // Outputs are not streamed by niri, so this is the whole chain: a workspace event names an output the
    // model has not seen before, which makes NiriOutputs ask for the outputs, which the test double
    // answers. The connector names are this slot's own, so the list the reply installs differs from
    // whatever ran before it, and the wait below is for this reply rather than for "a list of two" —
    // which an earlier slot can leave behind, letting the wait pass before this slot's refresh lands.
    server_.setReply(
        QStringLiteral("Outputs"),
        qstest::outputsReplyLine(
            {qstest::outputObject(QStringLiteral("DP-7"), {qstest::modeObject(3840, 2160, 119880)},
                                  qstest::number(0), qstest::logicalObject(0, 0, 3072, 1728, 1.25)),
             qstest::outputObject(QStringLiteral("HDMI-D-4"), {qstest::modeObject(1920, 1080, 60000)},
                                  qstest::number(0), qstest::logicalObject(3072, 0, 1920, 1080, 1.0))}));
    pushWorkspaces({qstest::workspaceObject(7, 1, QStringLiteral("DP-7"), true, true),
                    qstest::workspaceObject(8, 2, QStringLiteral("HDMI-D-4"))});
    QTRY_VERIFY_WITH_TIMEOUT(!outputNamed(service_.outputs(), QStringLiteral("DP-7")).isEmpty(), 5000);
    QCOMPARE(outputChanges.count(), 1);

    QCOMPARE(service_.outputs().size(), 2);
    const QVariantMap output = outputNamed(service_.outputs(), QStringLiteral("DP-7"));
    QCOMPARE(output.value(qml::OutputName).toString(), QStringLiteral("DP-7"));
    // Ordered by connector name, so the second output sorts after the first.
    QCOMPARE(service_.outputs().at(1).toMap().value(qml::OutputName).toString(),
             QStringLiteral("HDMI-D-4"));

    pushEvent(QStringLiteral("KeyboardLayoutsChanged"),
              QJsonObject{{QStringLiteral("keyboard_layouts"),
                           qstest::keyboardLayoutsObject({QStringLiteral("English (US)"),
                                                          QStringLiteral("Norwegian")},
                                                         0)}});
    QTRY_COMPARE(layoutChanges.count(), 1);
    const QVariantMap layout = service_.keyboardLayout();
    QCOMPARE(layout.value(QStringLiteral("names")).toStringList(),
             QStringList({QStringLiteral("English (US)"), QStringLiteral("Norwegian")}));
    QCOMPARE(layout.value(QStringLiteral("currentIndex")).toInt(), 0);
    QCOMPARE(layout.value(QStringLiteral("currentName")).toString(), QStringLiteral("English (US)"));

    // A switch moves only the index, and the indicator follows it.
    pushEvent(QStringLiteral("KeyboardLayoutSwitched"), QJsonObject{{QStringLiteral("idx"), 1}});
    QTRY_COMPARE(layoutChanges.count(), 2);
    QCOMPARE(service_.keyboardLayout().value(QStringLiteral("currentName")).toString(),
             QStringLiteral("Norwegian"));

    // The overview is a flag QML can bind to, and only a real change wakes it.
    QSignalSpy overview(&service_, &NiriService::overviewOpenChanged);
    pushEvent(QStringLiteral("OverviewOpenedOrClosed"), QJsonObject{{QStringLiteral("is_open"), true}});
    QTRY_COMPARE(overview.count(), 1);
    QCOMPARE(service_.overviewOpen(), true);
    pushEvent(QStringLiteral("OverviewOpenedOrClosed"), QJsonObject{{QStringLiteral("is_open"), true}});
    QTest::qWait(60);
    QCOMPARE(overview.count(), 1);

    // Closed again before the slot ends: a flag left on is the same kind of leftover as a list left full,
    // and a later slot asserting that opening the overview wakes a binding would find nothing to report.
    pushEvent(QStringLiteral("OverviewOpenedOrClosed"), QJsonObject{{QStringLiteral("is_open"), false}});
    QTRY_COMPARE(overview.count(), 2);
    QCOMPARE(service_.overviewOpen(), false);
}

// Every value a widget reads, compared as a whole map rather than one key at a time.
//
// The maps are the model's fields renamed, and a key filled from the *wrong* source field still reads
// plausibly — so every field in the state below is given a value no other field on the same object has,
// and what is compared is the entire map. That makes it a check on the key set as much as on the values:
// a key the service stops emitting is missing, a key it invents is unexpected, and a key filled from the
// wrong field holds the wrong value, with `mapDiff` saying which. The other half of the same check — that
// the key sets declared in `NiriServiceKeys.h` are the ones this suite covers — is the compile-time
// comparison at the top of this file and `everyKeyTheServiceCanEmitIsTheOneAWidgetReads` below.
void NiriServiceTest::everyValueQmlReadsComesFromTheFieldItNames() {
    // Ids and names no other test in this suite uses, so each wait below waits for this test's event
    // rather than being satisfied by state the model was already holding.
    pushWorkspaces({qstest::workspaceObject(21, 1, QStringLiteral("DP-3"), true, true,
                                            QStringLiteral("browser"), true, 23),
                    qstest::workspaceObject(22, 2, QStringLiteral("DP-3"), false, false,
                                            QStringLiteral("mail"))});
    QTRY_VERIFY_WITH_TIMEOUT(
        !service_.workspaces().isEmpty()
            && service_.workspaces().at(0).toMap().value(qml::WorkspaceId).toString()
                   == QStringLiteral("21"),
        5000);

    QVariantMap expectedWorkspace;
    expectedWorkspace.insert(qml::WorkspaceId, QStringLiteral("21"));
    expectedWorkspace.insert(qml::WorkspaceIdx, 1);
    expectedWorkspace.insert(qml::WorkspaceName, QStringLiteral("browser"));
    expectedWorkspace.insert(qml::WorkspaceOutput, QStringLiteral("DP-3"));
    expectedWorkspace.insert(qml::WorkspaceIsActive, true);
    expectedWorkspace.insert(qml::WorkspaceIsFocused, true);
    expectedWorkspace.insert(qml::WorkspaceIsUrgent, true);
    expectedWorkspace.insert(qml::WorkspaceActiveWindowId, QStringLiteral("23"));
    const QVariantMap urgent = service_.workspaces().at(0).toMap();
    QVERIFY2(urgent == expectedWorkspace, qPrintable(mapDiff(urgent, expectedWorkspace)));

    // The same shape for a workspace the compositor reported no active window for: the comparison is the
    // whole map again, so the *absent* `activeWindowId` is part of what is being asserted.
    QVariantMap expectedPlain;
    expectedPlain.insert(qml::WorkspaceId, QStringLiteral("22"));
    expectedPlain.insert(qml::WorkspaceIdx, 2);
    expectedPlain.insert(qml::WorkspaceName, QStringLiteral("mail"));
    expectedPlain.insert(qml::WorkspaceOutput, QStringLiteral("DP-3"));
    expectedPlain.insert(qml::WorkspaceIsActive, false);
    expectedPlain.insert(qml::WorkspaceIsFocused, false);
    expectedPlain.insert(qml::WorkspaceIsUrgent, false);
    const QVariantMap plain = service_.workspaces().at(1).toMap();
    QVERIFY2(plain == expectedPlain, qPrintable(mapDiff(plain, expectedPlain)));

    // The focused window, with the two fields no earlier test reached set true, so a key filled from the
    // other one — or from `is_focused` — reads differently.
    pushEvent(QStringLiteral("WindowsChanged"),
              QJsonObject{{QStringLiteral("windows"),
                           qstest::array({qstest::windowObject(23, QStringLiteral("google-chrome"), 21,
                                                               true, true, true)})}});
    QTRY_COMPARE(service_.focusedWindow().value(qml::WindowId).toString(), QStringLiteral("23"));

    QVariantMap expectedWindow;
    expectedWindow.insert(qml::WindowId, QStringLiteral("23"));
    expectedWindow.insert(qml::WindowTitle, QStringLiteral("title of google-chrome"));
    expectedWindow.insert(qml::WindowAppId, QStringLiteral("google-chrome"));
    expectedWindow.insert(qml::WindowIsFocused, true);
    expectedWindow.insert(qml::WindowIsFloating, true);
    expectedWindow.insert(qml::WindowIsUrgent, true);
    expectedWindow.insert(qml::WindowWorkspaceId, QStringLiteral("21"));
    const QVariantMap window = service_.focusedWindow();
    QVERIFY2(window == expectedWindow, qPrintable(mapDiff(window, expectedWindow)));

    // Outputs, which niri does not stream: the reply is asked for again because a config load is exactly
    // when scale, mode and position may have moved. The numbers and strings are all different from each
    // other so a swap between two keys cannot go unnoticed, and one output is disabled so the keys niri
    // reports nothing for are covered too.
    server_.setReply(
        QStringLiteral("Outputs"),
        qstest::outputsReplyLine(
            {qstest::outputObject(QStringLiteral("DP-4"), {qstest::modeObject(3840, 2160, 119880)},
                                  qstest::number(0), qstest::logicalObject(0, 0, 3072, 1728, 1.25),
                                  QStringLiteral("MODEL-D"), false, false, QStringLiteral("MAKE-D")),
             qstest::outputObject(QStringLiteral("eDP-2"), {qstest::modeObject(2560, 1440, 165000)},
                                  qstest::number(0), qstest::logicalObject(3840, 2160, 2048, 1152, 1.5),
                                  QStringLiteral("MODEL-E"), true, true, QStringLiteral("MAKE-E")),
             qstest::outputObject(QStringLiteral("HDMI-B-2"), {}, QJsonValue(), QJsonValue(),
                                  QStringLiteral("MODEL-9"))}));
    pushEvent(QStringLiteral("ConfigLoaded"),
              QJsonObject{{QStringLiteral("failed"), QJsonValue(false)}});
    QTRY_VERIFY_WITH_TIMEOUT(!outputNamed(service_.outputs(), QStringLiteral("eDP-2")).isEmpty(), 5000);

    QVariantMap expectedOutput;
    expectedOutput.insert(qml::OutputName, QStringLiteral("eDP-2"));
    expectedOutput.insert(qml::OutputMake, QStringLiteral("MAKE-E"));
    expectedOutput.insert(qml::OutputModel, QStringLiteral("MODEL-E"));
    expectedOutput.insert(qml::OutputIsEnabled, true);
    expectedOutput.insert(qml::OutputIsVrrEnabled, true);
    expectedOutput.insert(qml::OutputX, 3840);
    expectedOutput.insert(qml::OutputY, 2160);
    // The types are part of what QML receives, so they are pinned by the expectation: `width` is the
    // model's own `quint32` and the mode's `quint16`, not an int this class widened on the way out.
    expectedOutput.insert(qml::OutputWidth, static_cast<quint32>(2048));
    expectedOutput.insert(qml::OutputHeight, static_cast<quint32>(1152));
    expectedOutput.insert(qml::OutputScale, 1.5);
    expectedOutput.insert(qml::OutputTransform, QStringLiteral("Normal"));
    expectedOutput.insert(qml::OutputModeWidth, static_cast<quint16>(2560));
    expectedOutput.insert(qml::OutputModeHeight, static_cast<quint16>(1440));
    expectedOutput.insert(qml::OutputRefreshRate, static_cast<quint32>(165000));
    const QVariantMap enabled = outputNamed(service_.outputs(), QStringLiteral("eDP-2"));
    QVERIFY2(enabled == expectedOutput, qPrintable(mapDiff(enabled, expectedOutput)));

    // The output beside it reports vrr off and a different scale, so the `true` and `1.5` above are
    // readings rather than constants this class writes for every output.
    QVariantMap expectedSecond;
    expectedSecond.insert(qml::OutputName, QStringLiteral("DP-4"));
    expectedSecond.insert(qml::OutputMake, QStringLiteral("MAKE-D"));
    expectedSecond.insert(qml::OutputModel, QStringLiteral("MODEL-D"));
    expectedSecond.insert(qml::OutputIsEnabled, true);
    expectedSecond.insert(qml::OutputIsVrrEnabled, false);
    expectedSecond.insert(qml::OutputX, 0);
    expectedSecond.insert(qml::OutputY, 0);
    expectedSecond.insert(qml::OutputWidth, static_cast<quint32>(3072));
    expectedSecond.insert(qml::OutputHeight, static_cast<quint32>(1728));
    expectedSecond.insert(qml::OutputScale, 1.25);
    expectedSecond.insert(qml::OutputTransform, QStringLiteral("Normal"));
    expectedSecond.insert(qml::OutputModeWidth, static_cast<quint16>(3840));
    expectedSecond.insert(qml::OutputModeHeight, static_cast<quint16>(2160));
    expectedSecond.insert(qml::OutputRefreshRate, static_cast<quint32>(119880));
    const QVariantMap other = outputNamed(service_.outputs(), QStringLiteral("DP-4"));
    QVERIFY2(other == expectedSecond, qPrintable(mapDiff(other, expectedSecond)));

    // A disabled output has no logical output and no current mode, so the nine keys that describe where
    // it is and what it is running are absent rather than zero: a widget must be able to tell "not
    // mapped" from "mapped at 0x0". The whole-map comparison is what states that.
    QVariantMap expectedDisabled;
    expectedDisabled.insert(qml::OutputName, QStringLiteral("HDMI-B-2"));
    expectedDisabled.insert(qml::OutputMake, QStringLiteral("MAKE"));
    expectedDisabled.insert(qml::OutputModel, QStringLiteral("MODEL-9"));
    expectedDisabled.insert(qml::OutputIsEnabled, false);
    expectedDisabled.insert(qml::OutputIsVrrEnabled, false);
    const QVariantMap disabled = outputNamed(service_.outputs(), QStringLiteral("HDMI-B-2"));
    QVERIFY2(disabled == expectedDisabled, qPrintable(mapDiff(disabled, expectedDisabled)));
}

// The key sets the service actually produced, against the sets declared for it. The compile-time half of
// this check is at the top of this file, and it is the half that makes adding a key impossible to do
// silently. This is the half only the running service can answer: that each map carries exactly the
// declared keys — no more, and none missing — which is also what catches a key written into a map as a
// string literal that never appears in `NiriServiceKeys.h`.
void NiriServiceTest::everyKeyTheServiceCanEmitIsTheOneAWidgetReads() {
    // A state of this test's own again, because a key set is compared as a set: it must be this test's
    // state that is being described, not whatever the test before it happened to leave behind.
    pushWorkspaces({qstest::workspaceObject(31, 1, QStringLiteral("DP-3"), true, true,
                                            QStringLiteral("browser"), false, 33),
                    qstest::workspaceObject(32, 2, QStringLiteral("DP-3"), false, false,
                                            QStringLiteral("mail"))});
    QTRY_VERIFY_WITH_TIMEOUT(
        !service_.workspaces().isEmpty()
            && service_.workspaces().at(0).toMap().value(qml::WorkspaceId).toString()
                   == QStringLiteral("31"),
        5000);

    // A workspace the compositor named an active window for carries the optional key; one it did not
    // does not. That difference is exactly what the two declared groups describe, and the lists here are
    // this suite's own rather than the service's.
    QCOMPARE(sortedKeys(service_.workspaces().at(0).toMap()),
             expectedKeys(coveredWorkspaceKeys, coveredWorkspaceOptionalKeys));
    QCOMPARE(sortedKeys(service_.workspaces().at(1).toMap()), expectedKeys(coveredWorkspaceKeys));

    // A window on a workspace, then one niri reports no workspace for. The second is the case a fixture
    // that always writes a `workspace_id` cannot build, and the key set is the whole claim there.
    pushEvent(QStringLiteral("WindowsChanged"),
              QJsonObject{{QStringLiteral("windows"),
                           qstest::array({qstest::windowObject(33, QStringLiteral("app.thirty-three"),
                                                               31, true)})}});
    QTRY_COMPARE(service_.focusedWindow().value(qml::WindowId).toString(), QStringLiteral("33"));
    QCOMPARE(sortedKeys(service_.focusedWindow()),
             expectedKeys(coveredWindowKeys, coveredWindowOptionalKeys));

    pushEvent(QStringLiteral("WindowsChanged"),
              QJsonObject{{QStringLiteral("windows"),
                           qstest::array({qstest::windowObject(34, QStringLiteral("app.thirty-four"),
                                                               std::nullopt, true)})}});
    QTRY_COMPARE(service_.focusedWindow().value(qml::WindowId).toString(), QStringLiteral("34"));
    QCOMPARE(sortedKeys(service_.focusedWindow()), expectedKeys(coveredWindowKeys));

    // One output niri has mapped and one it has not, in a single reply. The unmapped one is where the
    // geometry and mode groups are absent, so it is the disabled output that makes those two groups'
    // declared absence a tested claim rather than a comment.
    server_.setReply(
        QStringLiteral("Outputs"),
        qstest::outputsReplyLine(
            {qstest::outputObject(QStringLiteral("DP-5"), {qstest::modeObject(1920, 1080, 60000)},
                                  qstest::number(0), qstest::logicalObject(0, 0, 1920, 1080, 1.0)),
             qstest::outputObject(QStringLiteral("HDMI-C-3"), {}, QJsonValue(), QJsonValue())}));
    pushEvent(QStringLiteral("ConfigLoaded"),
              QJsonObject{{QStringLiteral("failed"), QJsonValue(false)}});
    QTRY_VERIFY_WITH_TIMEOUT(!outputNamed(service_.outputs(), QStringLiteral("HDMI-C-3")).isEmpty(), 5000);

    QCOMPARE(sortedKeys(outputNamed(service_.outputs(), QStringLiteral("DP-5"))),
             expectedKeys(coveredOutputKeys, coveredOutputGeometryKeys, coveredOutputModeKeys));
    QCOMPARE(sortedKeys(outputNamed(service_.outputs(), QStringLiteral("HDMI-C-3"))),
             expectedKeys(coveredOutputKeys));
}

// The QML-visible properties, checked against the meta-object rather than read from the header: a
// property added to `NiriService` without a line in the list below fails this test, which is how a
// property stops being added without a decision about the binding it exists for. `niri_service_test`
// runs in `check`, so the decision is part of the gate rather than something an audit has to notice.
void NiriServiceTest::theQmlPropertiesAreTheOnesABindingCanFollow() {
    const QStringList covered{QStringLiteral("connected"),      QStringLiteral("focusedWindow"),
                              QStringLiteral("keyboardLayout"), QStringLiteral("outputs"),
                              QStringLiteral("overviewOpen"),   QStringLiteral("workspaces")};

    const QMetaObject& meta = NiriService::staticMetaObject;
    QStringList actual;
    // From `QObject`'s own property count, so `objectName` is not counted as part of this surface.
    for (int index = QObject::staticMetaObject.propertyCount(); index < meta.propertyCount(); ++index) {
        const QMetaProperty property = meta.property(index);
        actual << QString::fromLatin1(property.name());

        // QML reads these and never writes them: the shell owns the state, and a writable property would
        // let a binding hand the model a value the compositor never reported.
        QVERIFY2(!property.isWritable(), qPrintable(QStringLiteral("NiriService.%1 is writable")
                                                        .arg(QString::fromLatin1(property.name()))));
        // Without a NOTIFY signal QML reads a property once and never sees it change again. That is
        // invisible from C++, which is why it is asserted for every property here rather than only for
        // the ones the binding test below happens to name.
        QVERIFY2(property.hasNotifySignal(),
                 qPrintable(QStringLiteral("NiriService.%1 has no NOTIFY signal")
                                .arg(QString::fromLatin1(property.name()))));
    }

    QStringList expected = covered;
    actual.sort();
    expected.sort();
    QCOMPARE(actual, expected);
}

void NiriServiceTest::anEventThatChangesNothingDoesNotWakeABinding() {
    pushEvent(QStringLiteral("WindowsChanged"),
              QJsonObject{{QStringLiteral("windows"),
                           qstest::array({qstest::windowObject(13, QStringLiteral("google-chrome"), 7, true),
                                          qstest::windowObject(14, QStringLiteral("editor"), 7)})}});
    // The workspace this event names, not `!isEmpty()`: a test earlier in the suite leaves a focused
    // window behind, so an emptiness check is satisfied before this event has been read — and then the
    // spies below would be created in time to record it. That is exactly what happened when this suite
    // was audited.
    QTRY_COMPARE(service_.focusedWindow().value(QStringLiteral("workspaceId")).toString(),
                 QStringLiteral("7"));

    // The spies start recording after the state above is in place, so a count is about the event this
    // test pushes and nothing else.
    QSignalSpy focusChanges(&service_, &NiriService::focusedWindowChanged);
    QSignalSpy windowChanges(&state_, &NiriState::windowsChanged);

    // A background window's title changes. The model reports new windows, because one of them did
    // change, but the focused window QML reads is the same window with the same title — so the binding
    // is not woken for it. This is the difference between relaying the model's signals and comparing
    // the value, and it is why the service compares.
    pushEvent(QStringLiteral("WindowsChanged"),
              QJsonObject{{QStringLiteral("windows"),
                           qstest::array({qstest::windowObject(13, QStringLiteral("google-chrome"), 7, true),
                                          qstest::windowObject(15, QStringLiteral("editor"), 7)})}});
    QTRY_COMPARE(windowChanges.count(), 1);
    QTest::qWait(60);
    QCOMPARE(focusChanges.count(), 0);
    QCOMPARE(service_.focusedWindow().value(QStringLiteral("id")).toString(), QStringLiteral("13"));
}

void NiriServiceTest::aQmlBindingFollowsTheServicesNotifySignals() {
    // This slot opens its own connection and builds its own state and service instead of using the shared
    // ones, for two reasons that are one: the binding is what is being examined, so the state behind it has
    // to be this slot's; and the last thing it does is take the compositor away, which must not reach any
    // other slot. Nothing shared is touched, and the shared `stream_` is left exactly as it was found.
    NiriEventStream stream;
    NiriState state;
    state.observe(stream);
    NiriService service(state, stream);

    QSignalSpy streaming(&stream, &NiriEventStream::streaming);
    stream.connectToCompositor(server_.path());
    QTRY_VERIFY_WITH_TIMEOUT(streaming.count() == 1, 5000);
    // The connection this slot just opened, so the events below go to it and not to the fixtures'. The
    // fixtures' two connections were accepted first, so this is the last one; the check says so rather
    // than letting the slot address a connection it does not own and quietly test the wrong thing.
    const int connection = server_.connectionCount() - 1;
    QVERIFY2(connection >= 2,
             qPrintable(QStringLiteral("the dedicated stream is connection %1, behind the fixtures' two")
                            .arg(connection)));

    // A known state, so what the binding is read against is set by this slot rather than inherited from
    // whichever slot ran before it. Three layouts and not two, because a state identical to one another
    // slot leaves behind could satisfy the waits below without this slot's event having arrived at all.
    pushEventTo(connection, QStringLiteral("WorkspacesChanged"),
                QJsonObject{{QStringLiteral("workspaces"),
                             qstest::array({qstest::workspaceObject(6, 1, QStringLiteral("DP-3"), true,
                                                                    true)})}});
    pushEventTo(connection, QStringLiteral("KeyboardLayoutsChanged"),
                QJsonObject{{QStringLiteral("keyboard_layouts"),
                             qstest::keyboardLayoutsObject({QStringLiteral("English (US)"),
                                                            QStringLiteral("Norwegian"),
                                                            QStringLiteral("German")},
                                                           1)}});
    QTRY_COMPARE(service.workspaces().size(), 1);
    QTRY_COMPARE(service.keyboardLayout().value(qml::KeyboardLayoutCurrentName).toString(),
                 QStringLiteral("Norwegian"));

    NiriService::registerQmlSingleton(service);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    // The shape the bar's first bindings take: a workspace strip's model, an indicator's text, and a
    // widget's honest note about whether the compositor is talking at all. The import line and the type
    // name are substituted from the mirror above rather than written out again, so the names are pinned
    // in one place and this slot stays about the bindings following the notify signals. `%1` is the type
    // name and it appears once per binding; `%2`-`%4` are the URI and its version.
    const QString source =
        QStringLiteral("import QtQml\n"
                       "import %2 %3.%4\n"
                       "QtObject {\n"
                       "    property var strip: %1.workspaces\n"
                       "    property int workspaceCount: %1.workspaces.length\n"
                       "    property string focusedWorkspaceId: %1.workspaces.length > 0\n"
                       "                                         ? %1.workspaces[0].id : \"\"\n"
                       "    property string layoutName: %1.keyboardLayout.currentName || \"\"\n"
                       "    property int layoutCount: %1.keyboardLayout.names ? "
                       "%1.keyboardLayout.names.length : 0\n"
                       "    property bool connected: %1.connected\n"
                       "}\n")
            .arg(QString::fromLatin1(coveredServiceTypeName), QString::fromLatin1(coveredModuleUri))
            .arg(coveredModuleMajorVersion)
            .arg(coveredModuleMinorVersion);
    component.setData(source.toUtf8(), QUrl(QStringLiteral("qrc:/test/Binding.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    std::unique_ptr<QObject> bound(component.create());
    QVERIFY2(bound != nullptr, qPrintable(component.errorString()));

    QCOMPARE(bound->property("workspaceCount").toInt(), 1);
    // Three names, which only this test's event carries: the binding is reading the state this test
    // pushed, not the two-layout state the test before it left behind.
    QCOMPARE(bound->property("layoutCount").toInt(), 3);
    QCOMPARE(bound->property("layoutName").toString(), QStringLiteral("Norwegian"));
    QCOMPARE(bound->property("connected").toBool(), true);

    // Now the compositor's state changes and nothing in the QML or the C++ is touched: the bindings
    // re-evaluate because the notify signals fired. A missing NOTIFY signal is invisible on the C++
    // side and shows up exactly here.
    pushEventTo(connection, QStringLiteral("WorkspacesChanged"),
                QJsonObject{{QStringLiteral("workspaces"),
                             qstest::array({qstest::workspaceObject(6, 1, QStringLiteral("DP-3"), true,
                                                                    true),
                                            qstest::workspaceObject(7, 2, QStringLiteral("DP-3"))})}});
    QTRY_COMPARE(bound->property("workspaceCount").toInt(), 2);
    QCOMPARE(bound->property("focusedWorkspaceId").toString(), QStringLiteral("6"));
    QCOMPARE(bound->property("strip").toList().size(), 2);

    pushEventTo(connection, QStringLiteral("KeyboardLayoutSwitched"),
                QJsonObject{{QStringLiteral("idx"), 0}});
    QTRY_COMPARE(bound->property("layoutName").toString(), QStringLiteral("English (US)"));

    // And the compositor going away is visible to the binding, not only to the model. What goes is this
    // slot's own connection, through the client-side teardown the shell uses when it shuts down, so the
    // fixtures' sockets are unaffected — this used to close the whole fake server's connections, which
    // left every slot declared after this one without a compositor to talk to.
    stream.disconnectFromCompositor();
    QTRY_COMPARE(bound->property("connected").toBool(), false);
    QTRY_COMPARE(bound->property("workspaceCount").toInt(), 0);
}

QTEST_GUILESS_MAIN(NiriServiceTest)
#include "niri_service_test.moc"
