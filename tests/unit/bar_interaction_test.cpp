// The bar's interaction and arrangement, driven through the components the shell ships rather than through
// copies of them.
//
// What a click and a wheel *mean* is a property of the bar's QML, and nothing in C++ can be asked about it:
// a capsule that draws the right workspace and does nothing when it is clicked passes every assertion a C++
// test can make. So `qml/Bar.qml` — the whole bar, as the application packs it — is loaded into a real
// engine here over the fake compositor, and the gestures are delivered as real window events,
// `QTest::mouseClick` and `QTest::wheelEvent`, which go through the platform layer the way a person's
// pointer does. The assertions are about what reached niri's end of the socket, so they are about the
// request the person's click produced and not about which QML function was called.
//
// Where a widget *ends up* is a property of the same files: the bar's groups and the group component they
// are made of. So the group is loaded here as well, on its own, and given widgets of three different natural
// heights — because the bar itself holds only widgets of one size, and "a widget is placed by the group it
// is declared in" is a claim about widgets of any size.
//
// The configuration is loaded here as well, because the bar's system status is written against it: which
// readouts the centre group draws and the form the memory one takes are `[bar.system]`'s values, and the
// only way to check that the widget honours a file is to change one and read the text back off the shipped
// QML. So a real `Config` is registered for this engine and driven the way the watcher drives it — by
// applying validated values.
//
// The notification readout is the one widget here whose daemon is the shell itself, so this binary is also the
// desktop's notification daemon for the two slots that ask it to be: a `dbus-daemon` of this process's own
// (`tests/support/NotificationBus.h`), the shipped service taking the notifications name on it, a second
// connection delivering a real `Notify`, and — at the end of each of those slots — the name handed to that
// connection, so the shell is *not* the daemon while the rest of the file reads the bar's arrangement. Both
// slots skip, naming the reason, when this machine has no `dbus-daemon`: without one the readout is dark
// whatever a slot does, so the claim is untestable rather than false, which is what the other live halves of
// this suite say for an unusable external program too.
//
// No display and no session: the platform plugin is the offscreen one, and the fake compositor owns the
// socket. The names a QML file is written against — the import and the singletons `NiriService`, `NiriActions`,
// `SysMonService`, `PipeWireService`, `NetworkService`, `BatteryService` and `Config` — are mirrored below and
// compared at compile time, on the same rule the service test mirrors the first two for.
#include "config/Config.h"
#include "config/ConfigSchema.h"
#include "niri/NiriActions.h"
#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"
#include "niri/NiriQmlModule.h"
#include "niri/NiriService.h"
#include "niri/NiriState.h"
#include "app/Logging.h"
#include "dbus/NotificationService.h"
#include "audio/PipeWireService.h"
#include "dbus/BatteryService.h"
#include "dbus/NetworkService.h"
#include "dbus/MediaService.h"
#include "system/SysMonService.h"

#include "FakeNiriServer.h"
#include "NiriProtocolTestData.h"
#include "NotificationBus.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTime>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <cmath>
#include <memory>
#include <optional>
#include <string_view>

using quantum::config::Config;
using quantum::config::ConfigValues;
using quantum::niri::NiriActions;
using quantum::niri::NiriEventStream;
using quantum::niri::NiriIPC;
using quantum::niri::NiriService;
using quantum::audio::PipeWireService;
using quantum::dbus::BatteryService;
using quantum::dbus::NetworkService;
using quantum::niri::NiriState;
using quantum::system::SysMonService;

namespace qml = quantum::niri::qml;

namespace {

// The names this test's QML and the shipped component are both written against, mirrored here rather than
// read out of the header. A rename in `src/niri/` does not build until this file agrees, which is where
// the question "who else says `NiriActions`" gets asked. The two below the module's own names are the
// singletons: the component this test loads imports the module and calls both by name, so a rename that
// got past these assertions would still fail — in the engine, at load time — and be reported as an
// unloadable component rather than as a broken rule.
constexpr auto coveredModuleUri = "QuantumShell";
constexpr int coveredModuleMajorVersion = 1;
constexpr int coveredModuleMinorVersion = 0;
constexpr auto coveredServiceTypeName = "NiriService";
constexpr auto coveredActionTypeName = "NiriActions";
constexpr auto coveredSysMonTypeName = "SysMonService";
constexpr auto coveredAudioTypeName = "PipeWireService";
constexpr auto coveredNetworkTypeName = "NetworkService";
constexpr auto coveredBatteryTypeName = "BatteryService";
constexpr auto coveredConfigTypeName = "Config";

static_assert(std::string_view(qml::ModuleUri) == std::string_view(coveredModuleUri),
              "the QML module URI changed: update the mirror above, and every QML file that imports it");
static_assert(qml::ModuleMajorVersion == coveredModuleMajorVersion, "the QML module major version changed");
static_assert(qml::ModuleMinorVersion == coveredModuleMinorVersion, "the QML module minor version changed");
static_assert(std::string_view(qml::ServiceTypeName) == std::string_view(coveredServiceTypeName),
              "the QML service type name changed: update the mirror above, and every binding that reads it");
static_assert(std::string_view(qml::ActionTypeName) == std::string_view(coveredActionTypeName),
              "the QML action type name changed: update the mirror above, and every gesture that calls it");
static_assert(std::string_view(SysMonService::QmlTypeName) == std::string_view(coveredSysMonTypeName),
              "the QML system-statistics name changed: update the mirror above, and every binding that reads it");
static_assert(std::string_view(PipeWireService::QmlTypeName) == std::string_view(coveredAudioTypeName),
              "the QML audio name changed: update the mirror above, and every binding that reads it");
static_assert(std::string_view(NetworkService::QmlTypeName) == std::string_view(coveredNetworkTypeName),
              "the QML network name changed: update the mirror above, and every binding that reads it");
static_assert(std::string_view(BatteryService::QmlTypeName) == std::string_view(coveredBatteryTypeName),
              "the QML battery name changed: update the mirror above, and every binding that reads it");

// The network readout's tokens — the state, the kind of device and the daemon's verdict — are compared in
// `theNetworkReadoutDrawsWhatTheDaemonReportsAndTheConfigurationNames` rather than here, and the reason is the
// one written above the volume token's resolver: a token builder returns a `QString`, and a `QString` cannot
// appear in a constant expression. What the comparison buys is the same thing the mirrors above buy for the
// names: the widget switches on these strings, so a token renamed in the module would leave it drawing its
// default branch — a readout that looks right and says the wrong thing.

// The schema's three `[bar.network]` key paths, which are the names `qsctl config get` takes and the names the
// `Config` tree answers to. The widget reads the flags through `Config.bar.network`, so a rename on one side of
// that boundary is a readout switched off by nothing.
static_assert(std::string_view(quantum::config::KeyBarNetworkShowStatus) == "bar.network.show_status");
static_assert(std::string_view(quantum::config::KeyBarNetworkShowName) == "bar.network.show_name");
static_assert(std::string_view(quantum::config::KeyBarNetworkShowStrength) == "bar.network.show_strength");
// And the fourth table's three, for the same reason: `qml/Battery.qml` reads them through `Config.bar.battery`,
// so a rename on one side of that boundary is a readout switched off by nothing.
static_assert(std::string_view(quantum::config::KeyBarBatteryShowStatus) == "bar.battery.show_status");
static_assert(std::string_view(quantum::config::KeyBarBatteryShowPercentage) == "bar.battery.show_percentage");
static_assert(std::string_view(quantum::config::KeyBarBatteryShowTime) == "bar.battery.show_time");
static_assert(std::string_view(Config::QmlTypeName) == std::string_view(coveredConfigTypeName),
              "the QML configuration name changed: update the mirror above, and every binding that reads it");

// The one token in this shell that three libraries have to agree about: `[bar.audio].volume_scale` is
// validated by the schema, switched on by `qml/Volume.qml` and resolved by the audio service into the unit a
// wheel notch is measured in. This binary is the only one that links the configuration and the audio module,
// which is why the schema's copy and the module's are compared here — the schema refuses a token it does not
// know, and the module refuses one it cannot resolve, so a rename on either side has to stop the build rather
// than reach a person as a readout drawn in one unit while a notch moves in the other.
static_assert(quantum::audio::wheelStepUnitToken(quantum::audio::WheelStepUnit::Percent)
                  == std::string_view(quantum::config::VolumeScalePercent),
              "the percentage token moved on one side of the schema/audio boundary");
static_assert(quantum::audio::wheelStepUnitToken(quantum::audio::WheelStepUnit::Decibel)
                  == std::string_view(quantum::config::VolumeScaleDecibel),
              "the decibel token moved on one side of the schema/audio boundary");
// The resolving half is a run-time check rather than a third assertion, because the resolver builds a
// `QString` and a `QString` cannot appear in a constant expression: it is in
// `theVolumeReadoutDrawsTheUnitTheConfigurationNames`, walking the schema's list through the module's own
// resolver, which is the direction the static assertions above cannot reach.

// Whether two measured widths are the same one. A pixel is the unit a layout works in and the widths these
// assertions compare come from font metrics, which are fractional (multiples of 1/64 px here, from the fixed
// point Freetype measures advances in) — so what is claimed is that two widths are the same width, not that
// two doubles are bit-identical. A hundredth of a pixel is two orders of magnitude below anything a person
// can see and below any difference the layout can act on, and it is still far tighter than the sub-pixel
// change that a minute tick in the clock's text produces.
bool sameWidth(qreal left, qreal right)
{
    return std::abs(left - right) < 0.01;
}

// `Number.prototype.toFixed`, which is what the widget's QML rounds its numbers with — half away from zero.
// The forms are all positive, which is what lets one branch serve.
//
// This used to say that `QString::number(value, 'f', digits)` rounds half to even and therefore differs from
// the widget on a reading that lands exactly on a rounding boundary. **That is false, and it was measured
// rather than argued**: over every tie that is exactly representable at these two precisions — `x.25` and
// `x.75` up to 40, and `N + 1/2` up to 4000.5, which is 4,081 values and 8,162 answers — Qt 6.11.2 and
// JavaScript's `toFixed` agree on all of them, and 40 of those values are ones where half-to-even and half
// away from zero would have disagreed, so the sweep can tell the two rules apart at all. Qt rounds half away
// from zero here, which is the widget's rule. The helper stays because it *states* that rule in the widget's
// own terms rather than inheriting Qt's — a Qt that changed its mind would leave this test asserting the
// wrong arithmetic — and because the rule is then written once for every readout that draws a number.
QString toFixed(double value, int digits)
{
    const double scale = std::pow(10.0, digits);
    const double scaled = value * scale;
    return QString::number(std::floor(scaled + 0.5) / scale, 'f', digits);
}

// The two `/proc` files the reading comes from, in the kernel's own shape.
//
// The numbers are this test's choice, and every literal expectation in the system-status slots is worked out
// from them: 16 GiB of memory with 4.5 GiB available is 11.5 GiB used, and the aggregate CPU counters below
// advance by 1,000 jiffies of which 700 are idle, which is 30% busy. That is what makes those assertions
// about the widget rather than about the machine this test happens to be running on.
//
// The aggregate counters are cumulative in the file and *advanced* on every write, which is what
// `writeFixtureReading` exists for: a real `/proc` only ever goes up, so any two readings of it are an
// interval. A slot that sampled twice against a line that had not moved would be provoking the service's own
// refusal — two readings that are not an interval, which it records on its own category, and which
// `sysmon-test` asserts on purpose and this file has no business inventing.
//
// Two details are deliberate rather than decorative. `MemFree` is filled in with a number that does not agree
// with the other two, because that is the field this shell must *not* be reporting: `used` is
// `MemTotal - MemAvailable`, so a widget reading `MemFree` would draw 2 GiB where the assertion says 11.5. And
// the per-core lines below the aggregate one are what a real file has, so a parser that read the first `cpu`
// line it saw would be reading this fixture's `cpu0`.
QByteArray memInfoOf(qulonglong availableKb)
{
    return QByteArrayLiteral("MemTotal:       ") + QByteArray::number(qulonglong(16777216)) + " kB\n"
        + QByteArrayLiteral("MemFree:         2000000 kB\n")
        + QByteArrayLiteral("MemAvailable:   ") + QByteArray::number(availableKb) + " kB\n"
        + QByteArrayLiteral("Buffers:           10000 kB\n")
        + QByteArrayLiteral("Cached:          3000000 kB\n");
}

// One reading of `/proc/stat`'s aggregate line, with `guest` and `guest_nice` left at zero: the kernel counts
// those inside `user` and `nice` already, so a sum of all ten fields would count them twice.
QByteArray statOf(qulonglong user, qulonglong system, qulonglong idle)
{
    return QByteArrayLiteral("cpu  ") + QByteArray::number(user) + " 0 " + QByteArray::number(system) + " "
        + QByteArray::number(idle) + " 0 0 0 0 0 0\n"
        + QByteArrayLiteral("cpu0 ") + QByteArray::number(user / 2) + " 0 " + QByteArray::number(system / 2)
        + " " + QByteArray::number(idle / 2) + " 0 0 0 0 0 0\n"
        + QByteArrayLiteral("cpu1 ") + QByteArray::number(user - user / 2) + " 0 "
        + QByteArray::number(system - system / 2) + " " + QByteArray::number(idle - idle / 2)
        + " 0 0 0 0 0 0\n"
        + QByteArrayLiteral("intr 1234567\nctxt 7654321\nbtime 1700000000\n");
}

// The rules the two readouts draw by, named once.
//
// Only the live slot uses these. The system-status slots compare the widget with a literal, because the
// reading they compare against is the test's own and the answer for it can therefore be written down — which
// is a stronger claim, since it pins the whole chain from the fixture's numbers through the parser and the
// widget to the string, rather than comparing the widget with a re-derivation of the same rule. What cannot
// work that way is `theSystemStatusReadsTheMachineItRunsOn`: nothing there knows the numbers in advance, so
// what it asserts is that the widget draws the reading the service holds — both read in one expression, with
// nothing between them that turns the event loop, because on a live machine the numbers move.
QString drawsCpuPercent(const SysMonService& service)
{
    return service.cpuAvailable() ? QString::number(qRound(service.cpuPercent())) + QStringLiteral("%")
                                  : QStringLiteral("—");
}

QString drawsUsedOfTotal(const SysMonService& service)
{
    const double used = service.memoryUsedKb() / 1048576.0;
    const double total = service.memoryTotalKb() / 1048576.0;
    return toFixed(used, 1) + QStringLiteral("/") + toFixed(total, 0) + QStringLiteral("G");
}

// The workspaces the compositor reports to this test, as ids: 6 is the focused one, so a click on the
// second capsule (workspace 7) is a click on a workspace the compositor has not focused, and a click on
// the first is the case the bar deliberately does nothing for.
constexpr quint64 firstWorkspaceId = 6;
constexpr quint64 secondWorkspaceId = 7;
constexpr quint64 thirdWorkspaceId = 8;

// The bar's own size. The width is the output's in the shell and the height is `Config.bar.height`, and both
// matter here rather than being decoration: the right group is anchored to the bar's trailing edge, so a bar
// of no width would put it off the window and leave a click nowhere to land.
constexpr int barWidth = 400;
constexpr int barHeight = 32;

// The records the shell writes, captured while a gesture is delivered. This is how a gesture that reaches the
// audio service is observed at all: the service has no daemon behind it in this test, so what it can be asked
// is what the gesture *reached* — each of the three asks the daemon for something, and with nothing attached
// the service says so on its own category. The gestures' *effects* are `audio-live-test`'s, against a daemon
// it starts; what is pinned here is that the widget's click and wheel get there.
struct Record {
    QString category;
    QtMsgType type = QtInfoMsg;
    QString message;
};

QList<Record> records;

void captureRecords(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    records.append(Record{QString::fromLatin1(context.category), type, message});
}

bool mentionsRecord(const QString& text)
{
    for (const Record& record : records) {
        if (record.message.contains(text))
            return true;
    }
    return false;
}

}  // namespace

class BarInteractionTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void clickingACapsuleFocusesTheWorkspaceItNames();
    void clickingTheFocusedCapsuleAsksTheCompositorForNothing();
    void theWheelMovesThroughNirisOwnWorkspaces();

    void theBarPlacesItsWidgetsInNamedGroups();
    void aGroupGivesEveryWidgetOfItTheSameHeightAndOrder();

    void theSystemStatusSitsInTheCentreGroup();
    void theSystemStatusDrawsTheMemoryFormTheConfigurationNames();
    void aReadoutTheConfigurationHidesIsNotDrawn();
    void theSystemStatusShowsAnEmptyStateBeforeThereIsAReading();
    void theSystemStatusReadsTheMachineItRunsOn();

    void theVolumeReadoutSitsInTheTrailingGroupAndShowsNoValue();
    void theVolumeReadoutDrawsTheUnitTheConfigurationNames();
    void theVolumeReadoutFollowsItsConfiguration();

    void theNetworkReadoutSitsInTheTrailingGroupAndShowsNoValue();
    void theNetworkReadoutDrawsWhatTheDaemonReportsAndTheConfigurationNames();
    void theNetworkReadoutFollowsItsConfiguration();

    void theNotificationReadoutDrawsWhatTheSenderWrote();
    void theNotificationReadoutFollowsItsConfiguration();
    void theBatteryReadoutSitsInTheTrailingGroupAndShowsItsEmptyState();
    void theBatteryReadoutDrawsWhatTheDaemonReportsAndTheConfigurationNames();

    void aClickAndAWheelOnTheVolumeReadoutReachTheService();

private:
    // The delegate the strip built for `index`, in the order the model reports: the item a pointer would
    // land on, found by walking what the real component produced rather than by counting pixels.
    QQuickItem* capsuleAt(int index);
    // Any item of the loaded bar by name — a group, or a widget inside one. Found by name rather than by
    // walking the bar's children, because which child something happens to be is not part of what is claimed
    // about it. It is also how a widget's own text is read without knowing its type: a `Text` is a private
    // Qt Quick class, and its `text` is a property on it like any other.
    QQuickItem* itemNamed(const QString& name);
    QString textOf(const QString& name);
    // The point at the middle of `capsule`, in the window's own coordinates — what `QTest` wants.
    QPointF centreOf(QQuickItem* capsule) const;

    int requestsSent() const;
    QString lastRequest() const;

    // Whether a group's width is exactly what its drawn widgets come to, gaps included — the one place "the
    // room it took is back" is visible, since a widget's own width says nothing about the gap it sat in. A
    // caller waits for it rather than reading it once: a positioner's width follows its children on the next
    // frame, which is the lag the volume and network slots were both caught by.
    bool groupHoldsExactlyItsDrawnChildren(QQuickItem* group) const;

    // The three steps of driving the shell's notification daemon, and they are the shell's own sequence
    // rather than a value pushed into the service: the daemon registers, a sender puts a notification on the
    // wire, and the shell stands down again. Each is used by both notification slots, so the reading and the
    // configuration are asserted against the same driver.
    //
    // `deliverNotification` returns the id the daemon answered with, or zero when it did not answer.
    void theShellBecomesTheNotificationDaemon();
    quint32 deliverNotification(const QString& application, const QString& summary, const QString& body);
    void theShellStopsBeingTheNotificationDaemon();

    // One reading of the fixture, both files at once: `availableKb` of the fixture's 16 GiB, and the
    // aggregate counters advanced by `jiffies` with `idleJiffies` of them idle. The deltas are the caller's
    // because the percentage a slot asserts is `100 * (jiffies - idleJiffies) / jiffies`, and the cumulative
    // line the file carries is this test's to keep — see the definition.
    bool writeFixtureReading(qulonglong availableKb, quint64 jiffies = 1000, quint64 idleJiffies = 700);
    // Writes one of the two files the reading comes from. The file is removed first rather than opened over:
    // the slot that points these names at the machine leaves symlinks here, and writing through one of those
    // would be an attempt to write the machine's own `/proc/meminfo` — it is the link that has to go.
    bool writeProcFile(const QString& name, const QByteArray& text);
    // Points those two names at the machine's own files, which is the shell's read path with a different
    // prefix: the service opens `<root>/stat` and `<root>/meminfo`, and a root whose two names are links is
    // the machine's reading through the same code.
    bool pointTheReadingsAtTheRealProc();
    // A field of the machine's `/proc/meminfo`, read here rather than through the service: two paths to the
    // same file, so a service that invented a number could not agree with this on its own.
    std::optional<qulonglong> memInfoFieldFromTheMachine(const QString& field);

    FakeNiriServer server_;
    NiriIPC requests_;
    NiriEventStream stream_;
    NiriState state_;
    std::unique_ptr<NiriService> service_;
    std::unique_ptr<NiriActions> actions_;
    // The `/proc` the service reads. Declared before the service so that it outlives it: the service opens
    // files in this directory, and the directory removes itself when it is destroyed. "Scratch" rather than
    // the word Qt's own type is named after, because the gate's `prototype-marker` rule reads that word
    // anywhere in the tree as a marker for work that was never finished — a rule that is crude on purpose, so
    // one word of prose is a cheaper correction than narrowing it.
    QTemporaryDir procRoot_;
    std::unique_ptr<SysMonService> sysMon_;
    // The fixture's busy time, cumulative as the file's aggregate line is and never reset: every sample in
    // this file therefore sees a line that has moved since the last one.
    qulonglong fixtureBusyJiffies_ = 0;
    qulonglong fixtureIdleJiffies_ = 0;
    // The audio service is registered but never started: this test loads the bar against the state a shell is
    // in before a daemon has answered — the empty state — which is also the state that makes a click and a
    // wheel *observable*, because with nothing attached each gesture says so on the service's own category.
    std::unique_ptr<PipeWireService> audio_;
    // The network service, registered the same way and for the same reason: the bar is loaded against the state
    // of a shell whose daemon has not answered, which is a dash. It is started against nothing in particular — a
    // bus connection that was never made — so that a slot cannot depend on the machine this runs on having a
    // NetworkManager, and no slot here can reach the desktop's own bus.
    std::unique_ptr<NetworkService> network_;
    // The battery service, registered the same way and for the same reason, and never started for the same
    // reason too: the bar is loaded against the state of a shell whose daemon has not answered, which for this
    // readout is a dash. A slot that needs the *other* answer — the daemon saying there is no battery — asks the
    // widget's own rule for it, because producing it from a service needs an upowerd.
    std::unique_ptr<BatteryService> battery_;
    std::unique_ptr<quantum::dbus::MediaService> media_;
    // The notification daemon, the one service here that is *driven* rather than left in its empty state, and
    // the two pieces of the desktop it needs: a bus of this process's own, and a second connection to it that
    // plays both parts a sender plays — it delivers the `Notify` call, and between slots it holds the
    // notifications name so the shell is not the daemon while the bar's other slots read the arrangement.
    qstest::NotificationBus notificationBus_;
    // The bus the shell's notification daemon registers on, and a second connection to it for the sender.
    // `QDBusConnection` has no default constructor — every one of them names a bus — so what is held before
    // `initTestCase` has made them is the value Qt gives for a name no connection was made under: not
    // connected, and every call on it failing rather than reaching somewhere unintended. That is what the
    // service is constructed with when this machine has no `dbus-daemon`, so a busless run leaves the readout
    // dark instead of reaching the desktop's own bus.
    QDBusConnection notificationConnection_{QString()};
    QDBusConnection notificationSender_{QString()};
    bool senderHoldsTheNotificationsName_ = false;
    std::unique_ptr<quantum::dbus::NotificationService> notifications_;
    Config config_;
    std::unique_ptr<QQmlEngine> engine_;
    std::unique_ptr<QObject> bar_;
    std::unique_ptr<QQuickWindow> window_;
};

void BarInteractionTest::initTestCase() {
    QVERIFY2(server_.listen(), qPrintable(server_.serverError()));
    server_.setReply(QStringLiteral("EventStream"), QByteArrayLiteral("{\"Ok\":\"Handled\"}"));
    server_.setReply(QStringLiteral("Action"), QByteArrayLiteral("{\"Ok\":\"Handled\"}"));

    // The state the strip draws, fed the way the shell feeds it: an event from the compositor, not a value
    // pushed into the service.
    state_.observe(stream_);

    service_ = std::make_unique<NiriService>(state_, stream_);
    NiriService::registerQmlSingleton(*service_);
    actions_ = std::make_unique<NiriActions>(requests_);
    NiriActions::registerQmlSingleton(*actions_);
    // The system status, reading a `/proc` of this test's own — the two files below, in a scratch directory,
    // rather than the machine this test happens to run on. That is what lets the assertions be literals: the
    // numbers the widget draws are numbers this test wrote, so the answer for them is arithmetic rather than
    // observation, and nothing about the widget can be moved by what the machine is doing at the time. One
    // slot points the same two names at the machine's own files again — `theSystemStatusReadsTheMachineItRunsOn`
    // — because a fixture is only a proof of the parsing and the binding if something reads a real one.
    //
    // What the reading *is* still belongs to `sysmon-test`, which drives every shape of input through these
    // parsers; what is asserted here is the bar: the flags, the four memory forms, the empty state, and the
    // binding that puts a service's number on screen.
    QVERIFY2(procRoot_.isValid(), qPrintable(procRoot_.errorString()));
    QVERIFY2(writeFixtureReading(4718592), "the fixture could not be written");
    // Left inactive, which is also the state a shell is in for the first moment of its life, so the bar loads
    // against the empty state a person sees before the first reading — and each slot below asks for the state
    // it needs. Nothing in this test ever arms the timer: every reading is taken by hand, so no assertion here
    // can be moved by a sample landing between two lines of it.
    sysMon_ = std::make_unique<SysMonService>(procRoot_.path());
    SysMonService::registerQmlSingleton(*sysMon_);
    audio_ = std::make_unique<PipeWireService>();
    PipeWireService::registerQmlSingleton(*audio_);
    network_ = std::make_unique<NetworkService>();
    NetworkService::registerQmlSingleton(*network_);
    battery_ = std::make_unique<BatteryService>();
    BatteryService::registerQmlSingleton(*battery_);
    media_ = std::make_unique<quantum::dbus::MediaService>();
    quantum::dbus::MediaService::registerQmlSingleton(*media_);
    // The notification daemon, which is the one service here this binary *drives* rather than leaving in its
    // empty state: both of the readout's states are real, and the slot that asks for a reading is the slot that
    // takes the desktop's notifications name. So this binary needs a bus of its own — `NotificationBus`, the
    // same harness `notification-test` uses — and when this machine has no `dbus-daemon` to start one the
    // service is constructed on a connection that was never made and never started: the readout is then dark
    // exactly as the rest of this file expects, and the two slots that need a daemon skip and say why.
    //
    // It is deliberately not the desktop's session bus. A service watching the notifications name there would
    // ask for it the moment the desktop's own notifier restarted — that is what the service's registration
    // path does on any `serviceRegistered` it hears — and a test binary has no business becoming the desktop's
    // notification daemon.
    if (notificationBus_.start(QStringLiteral(FIXTURES))) {
        notificationConnection_ = QDBusConnection::connectToBus(
            notificationBus_.address(), QStringLiteral("quantum-shell-bar-interaction-test"));
        notificationSender_ = QDBusConnection::connectToBus(
            notificationBus_.address(), QStringLiteral("quantum-shell-bar-interaction-test-sender"));
    }
    notifications_ = std::make_unique<quantum::dbus::NotificationService>(notificationConnection_);
    quantum::dbus::NotificationService::registerQmlSingleton(*notifications_);
    // The defaults, which is the state a shell starts in: both readouts drawn, memory as used of total. The
    // slots below that change it put it back, so no slot reads another's edit.
    Config::registerQmlSingleton(config_);

    QSignalSpy streaming(&stream_, &NiriEventStream::streaming);
    requests_.connectToCompositor(server_.path());
    stream_.connectToCompositor(server_.path());
    QTRY_VERIFY_WITH_TIMEOUT(streaming.count() == 1, 5000);
    QVERIFY(requests_.isConnected());

    server_.writeRawTo(1, qstest::eventLine(
                              QStringLiteral("WorkspacesChanged"),
                              QJsonObject{{QStringLiteral("workspaces"),
                                           qstest::array({qstest::workspaceObject(
                                                              firstWorkspaceId, 1,
                                                              QStringLiteral("DP-3"), true, true),
                                                          qstest::workspaceObject(
                                                              secondWorkspaceId, 2,
                                                              QStringLiteral("DP-3")),
                                                          qstest::workspaceObject(
                                                              thirdWorkspaceId, 3,
                                                              QStringLiteral("DP-3"))})}}));
    QTRY_COMPARE_WITH_TIMEOUT(service_->workspaces().size(), 3, 5000);

    // The component itself. No copy of the bar exists in this file: `QS_BAR_QML` is the `Bar.qml` entry of
    // the list the application's resources are built from, read here from the source tree because that
    // resource set is the application's, and the loading of it is the first assertion — a component that
    // does not resolve fails here rather than silently testing nothing. Loading the bar rather than one
    // widget inside it is also what puts the gestures under the arrangement: the click below reaches the
    // strip through the group it is declared in, the way the shell's own window arranges it.
    engine_ = std::make_unique<QQmlEngine>();
    QQmlComponent component(engine_.get(), QUrl::fromLocalFile(QStringLiteral(QS_BAR_QML)));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    bar_.reset(component.create());
    QVERIFY2(bar_ != nullptr, qPrintable(component.errorString()));

    // The bar fills the window its surface creates (`Main.qml` anchors it to), and the window fills the
    // output, so the size comes from the window here the way it does there rather than from any widget.
    auto* root = qobject_cast<QQuickItem*>(bar_.get());
    QVERIFY(root != nullptr);
    root->setWidth(barWidth);
    root->setHeight(barHeight);

    window_ = std::make_unique<QQuickWindow>();
    window_->resize(barWidth, barHeight);
    root->setParentItem(window_->contentItem());
    window_->show();
    QVERIFY2(QTest::qWaitForWindowExposed(window_.get()), "the window never became exposed");
}

QQuickItem* BarInteractionTest::capsuleAt(int index) {
    auto* root = qobject_cast<QQuickItem*>(bar_.get());
    if (root == nullptr) {
        return nullptr;
    }
    // The id the component gives the row, which is how a pointer's target is found without depending on
    // the order children happen to be declared in.
    QQuickItem* strip = root->findChild<QQuickItem*>(QStringLiteral("strip"));
    if (strip == nullptr) {
        return nullptr;
    }
    const QList<QQuickItem*> capsules = strip->childItems();
    if (index < 0 || index >= capsules.size()) {
        return nullptr;
    }
    return capsules.at(index);
}

QQuickItem* BarInteractionTest::itemNamed(const QString& name) {
    return bar_ == nullptr ? nullptr : bar_->findChild<QQuickItem*>(name);
}

QString BarInteractionTest::textOf(const QString& name) {
    QQuickItem* item = itemNamed(name);
    return item == nullptr ? QString() : item->property("text").toString();
}

QPointF BarInteractionTest::centreOf(QQuickItem* capsule) const {
    return capsule->mapToScene(QPointF(capsule->width() / 2.0, capsule->height() / 2.0));
}

int BarInteractionTest::requestsSent() const {
    return server_.receivedRequests().size();
}

QString BarInteractionTest::lastRequest() const {
    if (server_.receivedRequests().isEmpty()) {
        return QString();
    }
    return server_.receivedRequests().last();
}

bool BarInteractionTest::writeProcFile(const QString& name, const QByteArray& text) {
    const QString path = procRoot_.path() + QLatin1Char('/') + name;
    // Removed rather than truncated in place: a slot that pointed these names at the machine leaves symlinks
    // here, and opening one of those for writing is an attempt to write `/proc/meminfo`. The link is what has
    // to go, and `QFile::remove` unlinks it rather than following it.
    if (!QFile::remove(path) && QFile::exists(path)) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(text) == text.size();
}

bool BarInteractionTest::writeFixtureReading(qulonglong availableKb, quint64 jiffies, quint64 idleJiffies) {
    // The file's line is cumulative since boot, so the caller gives a difference and this keeps the total. The
    // whole busy part is written as user time and the idle part as idle; which of the eight documented fields
    // a percentage is summed from is `sysmon-test`'s subject, and what the slots here need of the fixture is a
    // difference of `jiffies` with `idleJiffies` idle in it.
    Q_ASSERT(jiffies > idleJiffies);
    fixtureBusyJiffies_ += jiffies - idleJiffies;
    fixtureIdleJiffies_ += idleJiffies;
    return writeProcFile(QStringLiteral("meminfo"), memInfoOf(availableKb))
        && writeProcFile(QStringLiteral("stat"),
                         statOf(fixtureBusyJiffies_, 0, fixtureIdleJiffies_));
}

bool BarInteractionTest::pointTheReadingsAtTheRealProc() {
    const char* const names[] = {"meminfo", "stat"};
    for (const char* name : names) {
        const QString path = procRoot_.path() + QLatin1Char('/') + name;
        if (!QFile::remove(path) && QFile::exists(path)) {
            return false;
        }
        if (!QFile::link(QStringLiteral("/proc/") + QLatin1String(name), path)) {
            return false;
        }
        // procfs holds no hard links, so what is needed here is a symlink, and Qt's `QFile::link` makes one
        // on Unix — measured rather than taken from the documentation, which says the kind of link is the
        // platform's. A Qt that made a hard link instead would fail here rather than have this slot read a
        // stale file and pass.
        if (!QFileInfo(path).isSymLink()) {
            return false;
        }
    }
    return true;
}

std::optional<qulonglong> BarInteractionTest::memInfoFieldFromTheMachine(const QString& field) {
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray first = field.toLatin1() + ':';
    for (const QByteArray& line : file.readAll().split('\n')) {
        if (!line.startsWith(first)) {
            continue;
        }
        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() < 2) {
            return std::nullopt;
        }
        bool ok = false;
        const qulonglong value = parts.at(1).toULongLong(&ok);
        return ok ? std::optional<qulonglong>(value) : std::nullopt;
    }
    return std::nullopt;
}

void BarInteractionTest::clickingACapsuleFocusesTheWorkspaceItNames() {
    const int before = requestsSent();

    QQuickItem* capsule = capsuleAt(1);
    QVERIFY2(capsule != nullptr, "the strip built no second capsule, so there is nothing to click");

    // The second workspace, by the id the compositor gave it — which is the whole claim: the click reaches
    // the compositor as `FocusWorkspace` on *that* workspace's id and not on its index in the strip.
    QTest::mouseClick(window_.get(), Qt::LeftButton, Qt::NoModifier, centreOf(capsule).toPoint());

    QTRY_VERIFY_WITH_TIMEOUT(requestsSent() > before, 5000);
    QCOMPARE(lastRequest(),
             QStringLiteral(R"({"Action":{"FocusWorkspace":{"reference":{"Id":7}}}})"));
}

void BarInteractionTest::clickingTheFocusedCapsuleAsksTheCompositorForNothing() {
    const int before = requestsSent();

    QQuickItem* capsule = capsuleAt(0);
    QVERIFY2(capsule != nullptr, "the strip built no first capsule, so there is nothing to click");

    // niri resolves the reference and then switches to it, and with `workspace-auto-back-and-forth` — which
    // this project's own session sets — switching to the workspace already focused lands on the previously
    // focused one. So the click is dropped here rather than sent: a bar that moved a person off the
    // workspace they are on when they click the workspace they are on would be worse than one that does
    // nothing. The wait is what makes "nothing" mean anything: a request that was going to be sent would
    // have been answered by now.
    QTest::mouseClick(window_.get(), Qt::LeftButton, Qt::NoModifier, centreOf(capsule).toPoint());
    QTest::qWait(150);

    QCOMPARE(requestsSent(), before);
}

void BarInteractionTest::theWheelMovesThroughNirisOwnWorkspaces() {
    QQuickItem* capsule = capsuleAt(1);
    QVERIFY2(capsule != nullptr, "the strip built no capsule to turn the wheel over");
    const QPointF overStrip = centreOf(capsule);

    // Down is `focus-workspace-down` and up is `focus-workspace-up`: the same pair, in the same direction,
    // that niri's own default config binds to the wheel (`Mod+WheelScrollDown { focus-workspace-down; }`).
    // Which workspace is below is the compositor's answer, so what is asserted here is the request and not
    // a workspace id computed from the strip — the bar does not walk its own model.
    const int beforeDown = requestsSent();
    QTest::wheelEvent(window_.get(), overStrip, QPoint(0, -120));
    QTRY_VERIFY_WITH_TIMEOUT(requestsSent() > beforeDown, 5000);
    QCOMPARE(lastRequest(), QStringLiteral(R"({"Action":{"FocusWorkspaceDown":{}}})"));

    const int beforeUp = requestsSent();
    QTest::wheelEvent(window_.get(), overStrip, QPoint(0, 120));
    QTRY_VERIFY_WITH_TIMEOUT(requestsSent() > beforeUp, 5000);
    QCOMPARE(lastRequest(), QStringLiteral(R"({"Action":{"FocusWorkspaceUp":{}}})"));
}

void BarInteractionTest::theBarPlacesItsWidgetsInNamedGroups() {
    auto* bar = qobject_cast<QQuickItem*>(bar_.get());
    QVERIFY(bar != nullptr);

    QQuickItem* left = itemNamed(QStringLiteral("left"));
    QQuickItem* right = itemNamed(QStringLiteral("right"));
    QVERIFY2(left != nullptr, "the bar has no group named left");
    QVERIFY2(right != nullptr, "the bar has no group named right");

    // The bar decides where a group sits and how tall it is: one at the leading edge, one at the trailing
    // edge, both the bar's full height. Nothing a widget declares takes part in that.
    QCOMPARE(left->x(), 0.0);
    QCOMPARE(right->x() + right->width(), bar->width());
    QCOMPARE(left->height(), bar->height());
    QCOMPARE(right->height(), bar->height());

    // A group's children are its widgets, in the order they were declared in it, and the bar's two widgets
    // are in the groups they were declared in rather than wherever an anchor left them.
    const QList<QQuickItem*> leftWidgets = left->childItems();
    const QList<QQuickItem*> rightWidgets = right->childItems();
    QCOMPARE(leftWidgets.size(), 1);
    QCOMPARE(rightWidgets.size(), 6);
    QCOMPARE(leftWidgets.first()->objectName(), QStringLiteral("workspaces"));
    // The trailing group holds the network, the battery, the media player, the notification, the volume and then
    // the clock, in the order they were declared in `qml/Bar.qml`: the group decides that order, the machine's
    // own condition is read together at the outside of it — the connection it is on, then the power it has left
    // — then what the desktop is doing — what is playing, and what was just said — then the one control a person
    // changes by hand, and the time keeps the corner.
    QCOMPARE(rightWidgets.at(0)->objectName(), QStringLiteral("network"));
    QCOMPARE(rightWidgets.at(1)->objectName(), QStringLiteral("battery"));
    QCOMPARE(rightWidgets.at(2)->objectName(), QStringLiteral("media"));
    QCOMPARE(rightWidgets.at(3)->objectName(), QStringLiteral("notifications"));
    QCOMPARE(rightWidgets.at(4)->objectName(), QStringLiteral("volume"));
    QCOMPARE(rightWidgets.at(5)->objectName(), QStringLiteral("clock"));
    // The height convention as it lands on the real widgets: each is its group's height, whatever the widget
    // would have been on its own, and each sits at the group's leading edge — which together mean no part of
    // either position was written down by the widget.
    QCOMPARE(leftWidgets.first()->height(), left->height());
    QCOMPARE(leftWidgets.first()->x(), 0.0);
    QCOMPARE(leftWidgets.first()->y(), 0.0);
    for (QQuickItem* widget : rightWidgets) {
        QCOMPARE(widget->height(), right->height());
        QCOMPARE(widget->y(), 0.0);
    }
    // The widgets that are drawn are beside each other in the order they were declared rather than on top of
    // each other, and that order is the group's own doing: none of them carries a coordinate. Positions are
    // compared rather than widths, because a positioner can lay two widgets out at the same `x` and still
    // report a width for both.
    //
    // Waited for, and the shuffled slot-order check is why. What is claimed is the *settled* arrangement, and a
    // positioner that has just been re-flowed by another slot's configuration edit holds the old positions until
    // the next turn of the event loop — the same lag the width comparisons above already allow for. Read
    // straight away, this slot passed in declaration order, where a slot that hides and restores the *first*
    // widget of the group (which is what `theNetworkReadoutFollowsItsConfiguration` does) happens to run
    // earlier than it; the check found the one order where it does not and this read a frame that was still
    // moving. A group that never places them in order still fails, however long this waits.
    //
    // The two widgets those comparisons step over are `media` and `notifications`, which this binary holds dark:
    // no player is playing, and the shell is not the notification daemon — the two notification slots take that
    // name and hand it back at the end of each, and this is the state they leave. A positioner lays out nothing
    // for an invisible child, so the four that are drawn are the four that are compared, in declaration order:
    // network, battery, volume, clock.
    QTRY_VERIFY_WITH_TIMEOUT(rightWidgets.at(1)->x() > rightWidgets.at(0)->x(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(rightWidgets.at(4)->x() > rightWidgets.at(1)->x(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(rightWidgets.at(5)->x() > rightWidgets.at(4)->x(), 5000);

    // And a widget that is not drawn holds no room rather than a width with nothing in it — the other half of
    // what being hidden has to mean, which is what stops it being an empty gap in the bar. Asserted for both of
    // this group's dark widgets here, not only for the one whose own readout has a slot of its own, so that a
    // group that stopped skipping an invisible child fails in the slot about arrangement.
    QVERIFY2(!rightWidgets.at(2)->isVisible(), "the media readout is drawn with no player");
    QCOMPARE(rightWidgets.at(2)->width(), 0.0);
    QVERIFY2(!rightWidgets.at(3)->isVisible(), "the notification readout is drawn while the shell is not the daemon");
    QCOMPARE(rightWidgets.at(3)->width(), 0.0);

    // The clock is a Text that has been given more height than its glyphs, so being on the same line as the
    // capsules is a matter of its own alignment rather than of the bar anchoring it. That alignment is what
    // is read here, and it is the declaration rather than a measured pixel: nothing in this test renders.
    // Qt reports the same number the QML `Text.AlignVCenter` names, so the two forms can be compared.
    QCOMPARE(rightWidgets.at(5)->property("verticalAlignment").toInt(), int(Qt::AlignVCenter));
}

void BarInteractionTest::theVolumeReadoutSitsInTheTrailingGroupAndShowsNoValue() {
    auto* bar = qobject_cast<QQuickItem*>(bar_.get());
    QVERIFY(bar != nullptr);

    QQuickItem* widget = itemNamed(QStringLiteral("volume"));
    QQuickItem* group = itemNamed(QStringLiteral("right"));
    QVERIFY2(widget != nullptr, "the bar has no volume readout");
    QVERIFY2(group != nullptr, "the bar has no group named right");
    QCOMPARE(widget->parentItem(), group);

    // Drawn with no file at all, and with a real width: the readout is on the bar rather than a zero-width
    // nothing that would satisfy a visibility check alone.
    QVERIFY2(widget->isVisible(), "the volume readout is hidden by default");
    QVERIFY2(widget->width() > 0, "the volume readout has no width to draw in");

    // The empty state, and it is a real check rather than a permanent label: the service is registered and
    // has never attached to a daemon, so `available` is false and the widget draws a dash. Never a zero — a
    // percentage the shell has not been told is a percentage it does not draw — and never a full bar, which
    // is what a default of 100 would look like.
    QCOMPARE(textOf(QStringLiteral("volumeValue")), QString::fromUtf8("—"));
    QCOMPARE(widget->property("shown").toBool(), true);

    // The service and the widget agree about the state, which is what makes the dash the service's answer
    // rather than a string the widget happens to contain.
    QVERIFY2(!audio_->available(), "a service that has never attached reports a reading");
}

void BarInteractionTest::theVolumeReadoutDrawsTheUnitTheConfigurationNames() {
    QQuickItem* widget = itemNamed(QStringLiteral("volume"));
    QVERIFY(widget != nullptr);

    // The widget's own rendering rule, called with the numbers a daemon sends. Nothing in this test has a
    // daemon — the service is registered and never started, so its reading is the empty state — and the rule is
    // a function of its inputs for exactly that reason: a comparison with a literal is a stronger claim than
    // one against a re-derivation, and the alternative would be a rule nothing can exercise without a
    // PipeWire daemon on the machine running the suite. What a real daemon does to the *reading* is
    // `audio-live-test`'s, and the binding below is what says the rule is handed those readings.
    const auto drawsWith = [&widget](const QString& scale, bool available, bool muted, int percent,
                                     double decibels) {
        QVariant text;
        QMetaObject::invokeMethod(widget, "textFor", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, scale),
                                  Q_ARG(QVariant, available), Q_ARG(QVariant, muted), Q_ARG(QVariant, percent),
                                  Q_ARG(QVariant, decibels));
        return text.toString();
    };

    // Named rather than assumed: with the rule gone every comparison below would fail with an empty actual and
    // nothing saying why, and this is the line that says the name looked for.
    QVERIFY2(!drawsWith(QStringLiteral("percent"), true, false, 42, -22.6).isEmpty(),
             "the volume readout has no textFor() rule, so nothing below is being asked of it");

    // One reading, two units, and neither number is the other: the percentage is WirePlumber's cube root and
    // the decibels are the physical gain of the same factor — 0.074087 is the sink `wpctl get-volume` reports
    // as 0.42 and `pactl list sinks` reports as -22,61 dB. The decibel form draws one decimal place, which is
    // the shell's rounding of that same measurement rather than a different one.
    QCOMPARE(drawsWith(QStringLiteral("percent"), true, false, 42, -22.605), QStringLiteral("42%"));
    QCOMPARE(drawsWith(QStringLiteral("decibel"), true, false, 42, -22.605), QStringLiteral("-22.6 dB"));
    // Above unity, where the two units disagree about which direction is bigger: a linear factor of 10.0 is
    // 215% and +20 dB, and a readout drawn in dB has to be able to say so without a ceiling of its own.
    QCOMPARE(drawsWith(QStringLiteral("percent"), true, false, 215, 20.0), QStringLiteral("215%"));
    QCOMPARE(drawsWith(QStringLiteral("decibel"), true, false, 215, 20.0), QStringLiteral("20.0 dB"));

    // Silence, which is the one reading the two units do not both render as a number: a zero factor is 0% and
    // negative infinity in decibels, and only one of those has a spelling a person can read. The shell draws
    // the mathematical symbol rather than a floor, because a floor would be a volume the sink is not playing at.
    QCOMPARE(drawsWith(QStringLiteral("percent"), true, false, 0, -INFINITY), QStringLiteral("0%"));
    QCOMPARE(drawsWith(QStringLiteral("decibel"), true, false, 0, -INFINITY), QStringLiteral("-∞ dB"));

    // And the two states that are not a level at all, in both units, because neither unit may overrule them: a
    // muted sink is a mute rather than a number — its decibels are still the daemon's, but drawing them would
    // say the sink is playing at that level — and a sink with no reading is a dash rather than a value derived
    // from the unit, which is what a readout that switched on the token alone would draw.
    for (const char* token : {"percent", "decibel"}) {
        const QString scale = QString::fromLatin1(token);
        QCOMPARE(drawsWith(scale, true, true, 42, -22.605), QStringLiteral("MUTE"));
        QCOMPARE(drawsWith(scale, false, false, 0, 0.0), QString::fromUtf8("—"));
    }

    // And the unit reaches the widget the way a person sets it: each token written into a real configuration
    // file, parsed by the schema and applied, with the widget's own property read back. That is what a renamed
    // token cannot survive — the schema refuses a spelling it does not know, and the widget reports what it was
    // given — and it is the half a rule comparison cannot see, because a widget that ignored the token and drew
    // the default would pass every comparison above.
    for (const char* token : {"percent", "decibel"}) {
        const QByteArray text = QByteArrayLiteral("schema_version = 1\n[bar.audio]\nvolume_scale = \"")
            + QByteArray(token) + "\"\n";
        const quantum::config::ParseResult parsed = quantum::config::parseConfig(text);
        QVERIFY2(parsed.errors.isEmpty() && parsed.warnings.isEmpty(),
                 qPrintable(QStringLiteral("%1: %2 %3")
                                .arg(QString::fromLatin1(token),
                                     parsed.errors.join(QStringLiteral("; ")),
                                     parsed.warnings.join(QStringLiteral("; ")))));
        config_.apply(parsed.values);
        QCOMPARE(widget->property("scale").toString(), QString::fromLatin1(token));
        // And with no daemon behind it the readout is still a dash under either token, which is the claim that
        // the unit is a rendering of a reading rather than a source of one.
        QCOMPARE(textOf(QStringLiteral("volumeValue")), QString::fromUtf8("—"));
    }

    // And the other direction across the same boundary, which the compile-time mirror above cannot reach: the
    // schema's own token list, resolved by the audio module the composition root has to hand them to. A file
    // whose unit the module could not resolve would leave a running shell drawing decibels while a notch moved
    // by percentage points, and this is the loop that says the two lists are one.
    for (const char* token : quantum::config::VolumeScales) {
        const std::optional<quantum::audio::WheelStepUnit> unit =
            quantum::audio::wheelStepUnitFromToken(QString::fromLatin1(token));
        QVERIFY2(unit.has_value(), token);
        QVERIFY2(quantum::audio::wheelStepUnitToken(*unit) == std::string_view(token), token);
    }

    // Back to what the shell ships, so a slot running after this one reads the default unit.
    config_.apply(ConfigValues{});
    QCOMPARE(widget->property("scale").toString(), QStringLiteral("percent"));
}

void BarInteractionTest::theVolumeReadoutFollowsItsConfiguration() {
    QQuickItem* widget = itemNamed(QStringLiteral("volume"));
    QQuickItem* clock = itemNamed(QStringLiteral("clock"));
    QVERIFY(widget != nullptr);
    QVERIFY(clock != nullptr);

    // The group it sits in. A widget that is not drawn has to stop taking room as well, and "took no room"
    // is only visible on the group: the widget's own width is the widget's, and a bar can have a zero-width
    // child and still have a gap where it used to be.
    //
    // What is deliberately *not* taken here is the group's width, and that is the one thing this slot used to
    // get wrong. The group holds the clock as well, and the clock is a wall clock: its text — and with
    // proportional figures, its width — changes with the minute. `Inter` does not resolve to a tabular-figure
    // font on the machine this was measured on (it resolves through fontconfig, and the figures there are
    // proportional: the width of `HH:mm` runs from 28.28125 px at "11:11" to 37.0625 px at "06:06", and no
    // two consecutive minutes have the same width), so a group width remembered before a round trip and
    // compared after it is a claim that the time stood still. That claim is false whenever a minute boundary
    // falls inside the round trip — rare for a quiet pass, which is why this passed in declaration order for
    // so long, and taken by the pass the shuffled check made slow enough to cross one.
    //
    // So every group width below is compared with what the group holds *at that moment*: the widget's own
    // width — which is stable, since both its texts are — the group's spacing, and the clock's width read in
    // the same evaluation. The three are read in one expression, so the clock cannot tick between them.
    // What that gives up is stated where it is used: a positioner's width is its contents, so the comparison
    // is really asking whether the widget is counted among them, which is the fact the flag controls.
    QQuickItem* group = itemNamed(QStringLiteral("right"));
    QVERIFY2(group != nullptr, "the bar has no trailing group");
    const qreal widgetWidth = widget->width();
    const qreal spacing = group->property("spacing").toReal();
    QVERIFY2(widgetWidth > 0, "the volume readout occupies no room to reclaim");

    // And it is *waited for* rather than read once, which is the second thing this slot had to learn about a
    // group whose contents move. A positioner's own size is not a binding: the group's width follows its
    // children's widths when it next lays out, which is the next frame, and its `implicitWidth` lags with it.
    // Measured by moving the clock's text on by a minute and reading: the clock's width changed at once
    // (33.671875 against 33.515625 — one minute, 10/64 px) and the group stayed at 78.09375 through
    // `qWait(1)`, with both it and its implicit width at 78.25 once a frame had gone through. So a single
    // read is a claim that no frame is pending between the clock's text changing and this line — true on a
    // quiet pass, and false on a pass slow enough to cross a minute boundary, which is how
    // `slot-order-randomised` caught it at 79 against 79.2344. Waiting does not weaken the assertion: what it
    // says is that the group agrees with its contents once the layout has settled, which is the fact the flag
    // controls, and the clock is moved on inside this slot so the settling is exercised on every run rather
    // than waited for by luck.
    // The group now holds three widgets, and what is compared against is all three read in the same
    // evaluation: the volume readout's own width, the network readout's — which is beside it and does not move
    // while this slot runs — and the clock's.
    QQuickItem* network = itemNamed(QStringLiteral("network"));
    QVERIFY2(network != nullptr, "the bar has no network readout");
    QQuickItem* battery = itemNamed(QStringLiteral("battery"));
    QVERIFY2(battery != nullptr, "the bar has no battery readout");
    QTRY_VERIFY_WITH_TIMEOUT(
        sameWidth(group->width(), widgetWidth + spacing + network->width() + spacing + battery->width() + spacing
                                      + clock->width()),
        5000);

    ConfigValues values;
    values.bar.audio.showVolume = false;
    config_.apply(values);

    // A minute passes while the widget is away — which is the thing the clock beside it really does, and the
    // smallest honest way to have it happen inside a slot that takes a fifth of a second rather than waiting
    // up to a minute for the wall clock to do it. The text is set to the minute after the real one, and the
    // widget's own `refresh()` puts the real time back at the end of the slot.
    //
    // This is what makes the property above *tested* rather than argued. The same round trip with the group's
    // width remembered instead of read fails here every time — 10 runs out of 10 — with a delta of one
    // minute's change in the clock's text, which over a day is anywhere from 1/64 px to 145/64 px (2.27 px).
    // Waiting for the wall clock to do it instead would leave the guard to chance, which is how the failure
    // reached `slot-order-randomised` in the first place.
    QTest::qWait(20);
    clock->setProperty("text", QTime::currentTime().addSecs(60).toString(QStringLiteral("HH:mm")));

    // Three halves of "not drawn", each a different fact: the widget is not visible, its value is not visible
    // with it, the room it took is back in the bar, and its own width is zero — the last being the one a
    // visibility check alone would miss, because a hidden item keeps whatever width it declares. The clock
    // beside it stays, so the flag is not hiding the whole group.
    QVERIFY2(!widget->isVisible(), "the volume readout is still drawn with show_volume false");
    QVERIFY2(!itemNamed(QStringLiteral("volumeValue"))->isVisible(),
             "the widget is hidden but its value is still visible");
    QTRY_COMPARE_WITH_TIMEOUT(widget->width(), 0.0, 5000);
    // The room it took is gone and not merely shrunk: the group is holding the network readout and the clock
    // and nothing else — a positioner lays out no space for an invisible child and no gap for one either,
    // which is the behaviour `CapsuleGroup.qml` documents and the one a hidden readout depends on.
    // Clock-proof by construction: every width is read in the same evaluation.
    QTRY_VERIFY_WITH_TIMEOUT(
        sameWidth(group->width(), network->width() + spacing + battery->width() + spacing + clock->width()), 5000);
    QVERIFY2(clock->isVisible(), "hiding the volume readout hid the clock as well");

    // Put back, and the widget comes back with the width it had — so nothing about its absence was permanent.
    // Its own width is compared exactly, because it is the same computation over the same two texts; the
    // group's is compared against what it holds now, for the reason written at the top of this slot.
    config_.apply(ConfigValues{});
    QTRY_VERIFY_WITH_TIMEOUT(widget->isVisible(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(widget->width(), widgetWidth, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        sameWidth(group->width(), widgetWidth + spacing + network->width() + spacing + battery->width() + spacing
                                      + clock->width()),
        5000);
    QCOMPARE(textOf(QStringLiteral("volumeValue")), QString::fromUtf8("—"));

    // The clock is left as this slot found it: the time it really reads, set by the widget's own function
    // rather than by a value written here, so no slot after this one reads a clock set to please this one.
    QMetaObject::invokeMethod(clock, "refresh");
}

void BarInteractionTest::theNetworkReadoutSitsInTheTrailingGroupAndShowsNoValue() {
    auto* bar = qobject_cast<QQuickItem*>(bar_.get());
    QVERIFY(bar != nullptr);

    QQuickItem* widget = itemNamed(QStringLiteral("network"));
    QQuickItem* group = itemNamed(QStringLiteral("right"));
    QVERIFY2(widget != nullptr, "the bar has no network readout");
    QVERIFY2(group != nullptr, "the bar has no group named right");
    QCOMPARE(widget->parentItem(), group);

    // Drawn with no file at all, and with a real width: the readout is on the bar rather than a zero-width
    // nothing that would satisfy a visibility check alone.
    QVERIFY2(widget->isVisible(), "the network readout is hidden by default");
    QVERIFY2(widget->width() > 0, "the network readout has no width to draw in");

    // The empty state, and it is a real check rather than a permanent label. The service is registered and has
    // never been started — its bus connection is one that was never made — so `available` is false and the
    // widget draws a dash. Never `offline`: that is a *reading*, the daemon saying nothing is connected, and a
    // bar that drew it from the daemon's silence would be claiming a fact about the network that nobody told
    // it. This is the assertion that tells the two apart, and the service's own flag is what says which is
    // which — the widget is not asked to guess.
    QVERIFY2(!network_->available(), "a service that was never started reports a reading");
    QCOMPARE(textOf(QStringLiteral("networkValue")), QString::fromUtf8("—"));
    QCOMPARE(widget->property("shown").toBool(), true);
    // The three flags the widget reads are the configuration's, and with no file at all they are the schema's
    // defaults — every part of the readout drawn.
    QCOMPARE(widget->property("naming").toBool(), true);
    QCOMPARE(widget->property("strengthShown").toBool(), true);
}

void BarInteractionTest::theNetworkReadoutDrawsWhatTheDaemonReportsAndTheConfigurationNames() {
    QQuickItem* widget = itemNamed(QStringLiteral("network"));
    QVERIFY(widget != nullptr);

    // The tokens the widget switches on, compared with the module's own builders. Every branch below is written
    // against these strings, so a token renamed on one side leaves the widget drawing the wrong branch — which
    // is why the comparison is here, where the rendering rule is exercised, rather than nowhere at all.
    QCOMPARE(quantum::dbus::connectionStateToken(quantum::dbus::ConnectionState::Disconnected),
             QStringLiteral("disconnected"));
    QCOMPARE(quantum::dbus::connectionStateToken(quantum::dbus::ConnectionState::Connecting),
             QStringLiteral("connecting"));
    QCOMPARE(quantum::dbus::connectionStateToken(quantum::dbus::ConnectionState::Connected),
             QStringLiteral("connected"));
    QCOMPARE(quantum::dbus::deviceKindToken(quantum::dbus::DeviceKind::Wifi), QStringLiteral("wifi"));
    QCOMPARE(quantum::dbus::deviceKindToken(quantum::dbus::DeviceKind::Ethernet), QStringLiteral("ethernet"));
    QCOMPARE(quantum::dbus::deviceKindToken(quantum::dbus::DeviceKind::Other), QStringLiteral("other"));
    QCOMPARE(quantum::dbus::connectivityToken(quantum::dbus::Connectivity::Portal), QStringLiteral("portal"));
    QCOMPARE(quantum::dbus::connectivityToken(quantum::dbus::Connectivity::Limited), QStringLiteral("limited"));

    // The widget's own rendering rule, called with the tokens and numbers NetworkManager sends. Nothing in
    // this test has a daemon — the service's bus connection was never made, so its reading is the empty state —
    // and the rule is a function of its inputs for exactly that reason: a comparison with a literal is a
    // stronger claim than one against a re-derivation, and the alternative would be a rule nothing can
    // exercise without a NetworkManager on the machine running the suite. What a real daemon does to the
    // *reading* is `network-test`'s, against a fake daemon it drives, and the binding below is what says the
    // rule is handed those readings.
    const auto drawsWith = [&widget](const QString& state, const QString& kind, const QString& name,
                                     const QString& interfaceName, bool hasStrength, int strength,
                                     const QString& connectivity, bool showName, bool showStrength) {
        QVariant text;
        QMetaObject::invokeMethod(widget, "textFor", Q_RETURN_ARG(QVariant, text),
                                  Q_ARG(QVariant, true), Q_ARG(QVariant, state), Q_ARG(QVariant, kind),
                                  Q_ARG(QVariant, name), Q_ARG(QVariant, interfaceName),
                                  Q_ARG(QVariant, hasStrength), Q_ARG(QVariant, strength),
                                  Q_ARG(QVariant, connectivity), Q_ARG(QVariant, showName),
                                  Q_ARG(QVariant, showStrength));
        return text.toString();
    };

    // Named rather than assumed: with the rule gone every comparison below would fail with an empty actual and
    // nothing saying why, and this is the line that says the name looked for.
    QVERIFY2(!drawsWith(QStringLiteral("connected"), QStringLiteral("wifi"), QStringLiteral("home"),
                        QStringLiteral("wlan0"), true, 74, QStringLiteral("full"), true, true)
                  .isEmpty(),
             "the network readout has no textFor() rule, so nothing below is being asked of it");

    // A wireless connection, drawn as the daemon's own facts: the name it reports for the connection (the SSID,
    // on the session this was written against), and the signal quality beside it. The quality is a byte of
    // quality and not a strength in dBm, so there is one unit it can be drawn in and no key offering another.
    QCOMPARE(drawsWith(QStringLiteral("connected"), QStringLiteral("wifi"), QStringLiteral("home"),
                       QStringLiteral("wlan0"), true, 74, QStringLiteral("full"), true, true),
             QStringLiteral("home 74%"));
    // A signal of zero is a reading — a device at the edge of range — and it is drawn, where a device without
    // one is a different fact. This is why the readout has `hasStrength` as well as `strength`, and the two
    // cases below are the ones that tell them apart.
    QCOMPARE(drawsWith(QStringLiteral("connected"), QStringLiteral("wifi"), QStringLiteral("home"),
                       QStringLiteral("wlan0"), true, 0, QStringLiteral("full"), true, true),
             QStringLiteral("home 0%"));
    QCOMPARE(drawsWith(QStringLiteral("connected"), QStringLiteral("wifi"), QStringLiteral("home"),
                       QStringLiteral("wlan0"), false, 0, QStringLiteral("full"), true, true),
             QStringLiteral("home"));
    // A wired connection has no quality to draw, whatever the flags say: what carries it is a cable.
    QCOMPARE(drawsWith(QStringLiteral("connected"), QStringLiteral("ethernet"),
                       QStringLiteral("Wired connection 1"), QStringLiteral("enp39s0"), false, 0,
                       QStringLiteral("full"), true, true),
             QStringLiteral("Wired connection 1"));

    // The two flags that decide what of that is drawn. With the name off the quality stands alone; with the
    // quality off the name does; with both off the readout falls back to what is left of the connection rather
    // than to an empty bar — the interface the daemon named, and the kind of device if it named none.
    QCOMPARE(drawsWith(QStringLiteral("connected"), QStringLiteral("wifi"), QStringLiteral("home"),
                       QStringLiteral("wlan0"), true, 74, QStringLiteral("full"), false, true),
             QStringLiteral("74%"));
    QCOMPARE(drawsWith(QStringLiteral("connected"), QStringLiteral("wifi"), QStringLiteral("home"),
                       QStringLiteral("wlan0"), true, 74, QStringLiteral("full"), true, false),
             QStringLiteral("home"));
    QCOMPARE(drawsWith(QStringLiteral("connected"), QStringLiteral("wifi"), QStringLiteral("home"),
                       QStringLiteral("wlan0"), true, 74, QStringLiteral("full"), false, false),
             QStringLiteral("wlan0"));
    QCOMPARE(drawsWith(QStringLiteral("connected"), QStringLiteral("other"), QString(), QString(), false, 0,
                       QStringLiteral("full"), false, false),
             QStringLiteral("other"));

    // The daemon's own state, which is three branches and not two. `disconnected` is a reading — the daemon
    // saying nothing is connected, which a bar draws as `offline` — and `connecting` is a machine whose link is
    // up and whose routing is not, which a bar that called it connected would be lying about.
    QCOMPARE(drawsWith(QStringLiteral("disconnected"), QStringLiteral("wifi"), QString(), QString(), false, 0,
                       QStringLiteral("none"), true, true),
             QStringLiteral("offline"));
    QCOMPARE(drawsWith(QStringLiteral("connecting"), QStringLiteral("wifi"), QString(), QString(), false, 0,
                       QStringLiteral("none"), true, true),
             QStringLiteral("connecting"));

    // The daemon's verdict on a connection it does not think works: a captive portal and a connection that
    // cannot reach everything are both connected and both not working, and both are drawn as the word the
    // daemon uses — the colour the widget also applies repeats that fact rather than carrying it alone. `full`
    // and no verdict at all draw the same nothing, because a marker's job is to flag trouble.
    QCOMPARE(drawsWith(QStringLiteral("connected"), QStringLiteral("wifi"), QStringLiteral("home"),
                       QStringLiteral("wlan0"), true, 74, QStringLiteral("portal"), true, true),
             QStringLiteral("home 74% portal"));
    QCOMPARE(drawsWith(QStringLiteral("connected"), QStringLiteral("wifi"), QStringLiteral("home"),
                       QStringLiteral("wlan0"), true, 74, QStringLiteral("limited"), true, true),
             QStringLiteral("home 74% limited"));
    QCOMPARE(drawsWith(QStringLiteral("connected"), QStringLiteral("wifi"), QStringLiteral("home"),
                       QStringLiteral("wlan0"), true, 74, QString(), true, true),
             QStringLiteral("home 74%"));

    // The empty state is the flag's rather than the string's, in every combination: no reading is a dash even
    // when the values handed in look like one, which is what a widget switching on the state alone would draw.
    QVariant empty;
    QMetaObject::invokeMethod(widget, "textFor", Q_RETURN_ARG(QVariant, empty), Q_ARG(QVariant, false),
                              Q_ARG(QVariant, QStringLiteral("connected")), Q_ARG(QVariant, QStringLiteral("wifi")),
                              Q_ARG(QVariant, QStringLiteral("home")),
                              Q_ARG(QVariant, QStringLiteral("wlan0")), Q_ARG(QVariant, true),
                              Q_ARG(QVariant, 74), Q_ARG(QVariant, QStringLiteral("full")), Q_ARG(QVariant, true),
                              Q_ARG(QVariant, true));
    QCOMPARE(empty.toString(), QString::fromUtf8("—"));

    // And each flag reaches the widget the way a person sets it: written into a real configuration file, parsed
    // by the schema and applied, with the widget's own property read back. That is what a renamed key cannot
    // survive — the schema refuses a spelling it does not know — and it is the half a rule comparison cannot
    // see, because a widget that ignored the flags and drew everything would pass every comparison above.
    const auto applyNetwork = [this](const QByteArray& body) {
        const quantum::config::ParseResult parsed = quantum::config::parseConfig(
            QByteArrayLiteral("schema_version = 1\n") + body);
        QVERIFY2(parsed.errors.isEmpty() && parsed.warnings.isEmpty(),
                 qPrintable(parsed.errors.join(QStringLiteral("; "))
                            + parsed.warnings.join(QStringLiteral("; "))));
        config_.apply(parsed.values);
    };
    applyNetwork(QByteArrayLiteral("[bar.network]\nshow_name = false\nshow_strength = false\n"));
    QCOMPARE(widget->property("naming").toBool(), false);
    QCOMPARE(widget->property("strengthShown").toBool(), false);
    applyNetwork(QByteArrayLiteral("[bar.network]\nshow_name = true\nshow_strength = false\n"));
    QCOMPARE(widget->property("naming").toBool(), true);
    QCOMPARE(widget->property("strengthShown").toBool(), false);

    // Back to what the shell ships, so a slot running after this one reads the defaults — and the readout is a
    // dash again under them, because what a flag decides is what is drawn and never whether a reading exists.
    config_.apply(ConfigValues{});
    QCOMPARE(widget->property("shown").toBool(), true);
    QCOMPARE(widget->property("naming").toBool(), true);
    QCOMPARE(widget->property("strengthShown").toBool(), true);
    QCOMPARE(textOf(QStringLiteral("networkValue")), QString::fromUtf8("—"));
}

void BarInteractionTest::theNetworkReadoutFollowsItsConfiguration() {
    QQuickItem* widget = itemNamed(QStringLiteral("network"));
    QQuickItem* clock = itemNamed(QStringLiteral("clock"));
    QVERIFY(widget != nullptr);
    QVERIFY(clock != nullptr);

    // The same claim the volume readout's own slot makes, for the same reason and with the same two halves of
    // "not drawn": the widget is hidden, its own width is zero, and the room it took is back in the bar — the
    // last being the one a visibility check alone would miss, because a hidden item keeps whatever width it
    // declares.
    //
    // The group's own width is compared with what it holds *at that moment*, read in one expression, and the
    // reason is worked out in full above `theVolumeReadoutFollowsItsConfiguration`: the clock that ends this
    // group is a wall clock whose text — and, with proportional figures, whose width — changes with the minute,
    // so a group width remembered before a round trip and compared after it is a claim that the time stood
    // still. Waiting does not weaken that: what is asserted is that the group agrees with its contents once the
    // layout has settled, which is the fact the flag controls. This widget is the *first* of the group, so its
    // absence and its return move every widget after it, and the group is therefore the only place the room it
    // took is visible at all.
    QQuickItem* group = itemNamed(QStringLiteral("right"));
    QVERIFY2(group != nullptr, "the bar has no trailing group");
    QQuickItem* battery = itemNamed(QStringLiteral("battery"));
    QVERIFY2(battery != nullptr, "the bar has no battery readout");
    QQuickItem* volume = itemNamed(QStringLiteral("volume"));
    QVERIFY2(volume != nullptr, "the bar has no volume readout");
    const qreal widgetWidth = widget->width();
    const qreal spacing = group->property("spacing").toReal();
    QVERIFY2(widgetWidth > 0, "the network readout occupies no room to reclaim");
    QVERIFY2(spacing > 0, "the group reported no spacing, so the sum below could not tell a gap from a widget");

    // What the group's four drawn widgets come to, gaps included: this readout, the battery, the volume and the
    // clock. Its other two — the media and notification readouts — are dark in this binary, so a positioner lays
    // out no space and no gap for either, which is why neither is in the sum: the assertion is what says they
    // stayed out of it, since a group that reserved room for an invisible child would report a width this does
    // not match.
    QTRY_VERIFY_WITH_TIMEOUT(
        sameWidth(group->width(), widgetWidth + spacing + battery->width() + spacing + volume->width() + spacing
                                      + clock->width()),
        5000);

    ConfigValues values;
    values.bar.network.showStatus = false;
    config_.apply(values);

    QVERIFY2(!widget->isVisible(), "the network readout is still drawn with show_status false");
    QVERIFY2(!itemNamed(QStringLiteral("networkValue"))->isVisible(),
             "the widget is hidden but its value is still visible");
    QTRY_COMPARE_WITH_TIMEOUT(widget->width(), 0.0, 5000);
    // The room it took is gone rather than merely shrunk: the group is holding the battery, the volume and the
    // clock and nothing else — not the widget at zero width, and not the gap it sat in. Clock-proof by
    // construction, since every width is read in one evaluation.
    QTRY_VERIFY_WITH_TIMEOUT(
        sameWidth(group->width(), battery->width() + spacing + volume->width() + spacing + clock->width()), 5000);
    QVERIFY2(clock->isVisible(), "hiding the network readout hid the clock as well");

    // Put back: the widget comes back with the width it had and the group with the room it had, so nothing
    // about its absence was permanent. The group's whole width is waited for and not just the widget's: this
    // widget is the first of the group, so its return moves the three after it, and a slot that read their
    // positions while that was still pending would be reading a layout mid-change rather than the bar's.
    config_.apply(ConfigValues{});
    QTRY_VERIFY_WITH_TIMEOUT(widget->isVisible(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(widget->width(), widgetWidth, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        sameWidth(group->width(), widgetWidth + spacing + battery->width() + spacing + volume->width() + spacing
                                      + clock->width()),
        5000);
    QVERIFY2(clock->isVisible(), "hiding the network readout hid the clock as well");
    QCOMPARE(textOf(QStringLiteral("networkValue")), QString::fromUtf8("—"));
}

// Whether a group's width is exactly what its drawn widgets come to, gaps included.
//
// A positioner lays out no space for an invisible child and no gap for one either — `CapsuleGroup.qml` states
// that as its own behaviour — so this is the whole of what a group's width is, and it is the only place the
// room a widget took is visible at all: a widget's own width says nothing about the gap it sat in. Every width
// is read in one evaluation, so the clock that ends the trailing group cannot tick between the group's width
// and its contents' — the failure the volume and network slots in this file were both caught by. A caller
// waits for it rather than reading it once, because a positioner's width follows its children on the next
// frame.
bool BarInteractionTest::groupHoldsExactlyItsDrawnChildren(QQuickItem* group) const {
    Q_ASSERT(group != nullptr);
    const qreal spacing = group->property("spacing").toReal();
    qreal drawn = 0.0;
    int count = 0;
    for (QQuickItem* child : group->childItems()) {
        if (!child->isVisible())
            continue;
        drawn += child->width();
        ++count;
    }
    return sameWidth(group->width(), drawn + spacing * (count > 0 ? count - 1 : 0));
}

// The shell takes the desktop's notifications name on this test's bus: the composition root's own sequence —
// the service registers, and the bar's binding to it follows — rather than a value pushed into the service.
//
// The two steps before the registration are what make the state a slot finds independent of any slot that ran
// earlier in the same process, and neither is decoration:
//
//   * the sender letting go of the name, if it is holding it. `theShellStopsBeingTheNotificationDaemon` hands
//     the name to the sender so that the shell cannot quietly become the daemon again between slots, and a
//     slot that wants the daemon asks for it back.
//   * `stop()` before `start()`, so the bring-up is the same two calls whatever the previous state was. It is
//     also what the service's own contract needs for a *handover*: a bare `start()` while the sender holds the
//     name would leave the shell queued behind it, and this slot wants the shell to be the daemon now rather
//     than whenever the sender lets go. (Qt refuses a second export of an object at the same path — measured:
//     the second `registerObject` returns false while the first still holds — which the service answers by
//     treating an export that is already its own as done, so a bare `start()` is no longer the dead end it was;
//     releasing first is still what makes this slot's state independent of the one before it.)
void BarInteractionTest::theShellBecomesTheNotificationDaemon() {
    if (senderHoldsTheNotificationsName_) {
        notificationSender_.unregisterService(QString::fromLatin1(quantum::dbus::NotificationsServiceName));
        senderHoldsTheNotificationsName_ = false;
    }
    notifications_->stop();
    notifications_->start();
    QTRY_VERIFY_WITH_TIMEOUT(notifications_->notificationAvailable(), 5000);
}

// A `Notify` call the way a sender makes it: a second connection to the same bus, the spec's arguments, and an
// event loop turning while the reply is in flight.
//
// That shape is forced rather than chosen. The daemon this calls is in this process and on this thread, so a
// blocking `call()` would hold the very loop that has to dispatch the message it sent, and the wait would end
// in a timeout reported as a refusal by a daemon that was never asked. What the round trip buys is that the
// reading arrived over the wire — marshalled and dispatched by Qt's bus layer and applied to the properties the
// widget binds — rather than being a value this test wrote into the service.
quint32 BarInteractionTest::deliverNotification(const QString& application, const QString& summary,
                                                const QString& body) {
    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(quantum::dbus::NotificationsServiceName),
        QString::fromLatin1(quantum::dbus::NotificationsObjectPath),
        QString::fromLatin1(quantum::dbus::NotificationsInterface), QStringLiteral("Notify"));
    call << application << quint32(0) << QString() << summary << body << QStringList() << QVariantMap()
         << qint32(-1);

    QDBusPendingCallWatcher watcher(notificationSender_.asyncCall(call));
    QElapsedTimer elapsed;
    elapsed.start();
    while (!watcher.isFinished() && elapsed.elapsed() < 5000)
        QCoreApplication::processEvents();
    if (!watcher.isFinished())
        return 0;

    const QDBusMessage reply = watcher.reply();
    if (reply.type() != QDBusMessage::ReplyMessage)
        return 0;
    return reply.arguments().constFirst().toUInt();
}

// The shell stops being the daemon, and stays stopped — which is the state the readout's rule is about, and the
// state the rest of this file reads the bar's arrangement in.
//
// `stop()` is the service's own standing down: it releases the name and the object and withdraws the reading.
// Then the *sender* takes the name, and that is the half that makes the state stable rather than merely
// current. A shell that hears a report of its own registration asks for the name again — the report can arrive
// after the release — and a request for a name another connection already holds queues instead of being
// granted, so while this holds it the shell stays dark and no slot after this one can find the readout drawn
// with a dash nobody asked for. It is a real desktop situation rather than a trick: another daemon running
// alongside the shell is what the service's own header describes, and it is what the widget's comment means by
// the readout being drawn only while the shell is the daemon.
void BarInteractionTest::theShellStopsBeingTheNotificationDaemon() {
    notifications_->stop();

    const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> taken =
        notificationSender_.interface()->registerService(
            QString::fromLatin1(quantum::dbus::NotificationsServiceName),
            QDBusConnectionInterface::DontQueueService);
    senderHoldsTheNotificationsName_ =
        taken.isValid() && taken.value() == QDBusConnectionInterface::ServiceRegistered;
    QVERIFY2(senderHoldsTheNotificationsName_,
             qPrintable(QStringLiteral("this test's sender could not take the notifications name, so the state "
                                       "the readout is read in afterwards would be whatever the shell did next: "
                                       "%1")
                            .arg(taken.isValid()
                                     ? QStringLiteral("the bus answered %1").arg(int(taken.value()))
                                     : taken.error().message())));

    QVERIFY2(!notifications_->notificationAvailable(),
             "the shell is still the desktop's notification daemon after standing down");
}

// The readout end to end: a sender puts a notification on the wire, the shell answers as the desktop's
// notification daemon, and the text the sender wrote is what the bar draws.
//
// Every other readout in this file is asserted against a service left in its empty state, because a daemon
// behind it would need root, a session or a compositor. This one is different in the one way that matters:
// the daemon is the shell, so this binary can be it — on a bus of its own, which is what `NotificationBus`
// exists for. What the round trip buys over comparing the widget with a rule is the whole chain at once: the
// name on the bus, the marshalled arguments, the service's properties, the QML binding and the text a person
// sees, with nothing in that chain a test wrote by hand.
void BarInteractionTest::theNotificationReadoutDrawsWhatTheSenderWrote() {
    // No bus, no daemon, and a readout that is dark whatever a slot does: the claim is then not testable rather
    // than false, which is what a skip says. Every other slot in this file runs without a bus at all.
    if (!notificationBus_.isRunning())
        QSKIP(qPrintable(notificationBus_.error()));

    QQuickItem* widget = itemNamed(QStringLiteral("notifications"));
    QVERIFY2(widget != nullptr, "the bar has no notification readout");

    // The state before the shell is the daemon: a bar whose notification daemon is another process draws
    // nothing at all. This is half of the widget's rule — `showNotifications && notificationAvailable` — and it
    // is asserted here, where both halves can be produced, rather than in the arrangement slot where it cannot
    // be told apart from a widget that never draws.
    QVERIFY2(!widget->isVisible(), "the notification readout is drawn before the shell is the daemon");
    QCOMPARE(widget->width(), 0.0);

    theShellBecomesTheNotificationDaemon();

    // Being the daemon and having something to say are two different states, which is what the readout's own
    // comment is about: the widget is drawn as soon as the name is the shell's, and what it draws before any
    // notification arrives is a dash. The service's own reading is asserted first, so the dash below is the
    // state the bring-up made (it released the reading a previous slot may have left) rather than a summary
    // that happens to be missing.
    QTRY_VERIFY_WITH_TIMEOUT(widget->isVisible(), 5000);
    QVERIFY2(widget->width() > 0, "the notification readout is drawn with no room to draw in");
    QVERIFY2(notifications_->notificationSummary().isEmpty(),
             "the bring-up left a reading behind, so the dash below would not be the empty state");
    QCOMPARE(textOf(QStringLiteral("notificationSummary")), QString::fromUtf8("—"));

    // The sender's own words, over the bus. Three distinct strings, and distinct from every other literal in
    // this file, so a widget drawing the wrong one of them cannot pass by coincidence.
    const QString application = QStringLiteral("Quantum Shell Bar Test");
    const QString summary = QStringLiteral("The summary the sender wrote");
    const QString body = QStringLiteral("The body the sender wrote");
    QVERIFY2(deliverNotification(application, summary, body) != 0,
             "the shell's notification daemon did not answer a Notify call");

    // What the service published: the spec's three text arguments verbatim. `notification-test` owns the wire
    // contract; this is the same reading asserted here because the claim below is about *those* values.
    QCOMPARE(notifications_->notificationApplication(), application);
    QCOMPARE(notifications_->notificationSummary(), summary);
    QCOMPARE(notifications_->notificationBody(), body);

    // And what the bar draws from it. Waited for, because the text follows the notify signal through the
    // binding on the next turn of the event loop rather than inside the call that moved it. The application
    // name is drawn because it is the sender's attribution of its own message; the body is deliberately not
    // drawn at all — the service publishes it and the bar has room for the subject line.
    QTRY_COMPARE_WITH_TIMEOUT(textOf(QStringLiteral("notificationApplication")), application, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(textOf(QStringLiteral("notificationSummary")), summary, 5000);

    theShellStopsBeingTheNotificationDaemon();

    // The reading goes with the daemon: a summary drawn after the shell stopped holding the name would be a
    // claim about a life of the daemon that is not this one.
    QTRY_VERIFY_WITH_TIMEOUT(!widget->isVisible(), 5000);
    QCOMPARE(widget->width(), 0.0);
    QVERIFY2(notifications_->notificationSummary().isEmpty(),
             "the shell stood down but kept the reading it was last sent");
}

// The readout's configuration, with a reading in hand.
//
// What makes this slot evidence is the notification in it. It used to be the empty state three times over: the
// service was registered and never started, so the widget was hidden whatever the file said and every
// assertion was `width() == 0` — which passes with `show_notifications` deleted, and passed with it ignored.
// So the readout is driven *drawn* first, and then the file says not to draw it.
void BarInteractionTest::theNotificationReadoutFollowsItsConfiguration() {
    if (!notificationBus_.isRunning())
        QSKIP(qPrintable(notificationBus_.error()));

    QQuickItem* widget = itemNamed(QStringLiteral("notifications"));
    QQuickItem* group = itemNamed(QStringLiteral("right"));
    QVERIFY2(widget != nullptr, "the bar has no notification readout");
    QVERIFY2(group != nullptr, "the bar has no trailing group");
    QCOMPARE(widget->parentItem(), group);

    theShellBecomesTheNotificationDaemon();
    QTRY_VERIFY_WITH_TIMEOUT(widget->isVisible(), 5000);

    // A reading for the flag to be about, in this slot's own words: nothing here depends on what any other
    // slot delivered, which is what keeps the slot order-independent.
    const QString application = QStringLiteral("Quantum Shell Configuration Test");
    const QString summary = QStringLiteral("The readout this slot turns off");
    QVERIFY2(deliverNotification(application, summary, QStringLiteral("A body the bar does not draw")) != 0,
             "the shell's notification daemon did not answer a Notify call");
    QTRY_COMPARE_WITH_TIMEOUT(textOf(QStringLiteral("notificationSummary")), summary, 5000);

    // Drawn, with the group accounting for it. This is the state the flag is checked against, and the state a
    // widget that ignored the flag would still be in after the edit below.
    QVERIFY2(widget->width() > 0, "the notification readout is drawn with no room to draw in");
    QCOMPARE(textOf(QStringLiteral("notificationApplication")), application);
    QTRY_VERIFY_WITH_TIMEOUT(groupHoldsExactlyItsDrawnChildren(group), 5000);

    // The flag off. Two halves of "not drawn": the widget, and the room it took — the second being the one a
    // visibility check alone would miss, because a hidden item keeps whatever width it declares and the gap it
    // sat in is the group's to give back.
    ConfigValues values;
    values.bar.notifications.showNotifications = false;
    config_.apply(values);

    QVERIFY2(!widget->isVisible(), "the notification readout is still drawn with show_notifications false");
    QTRY_COMPARE_WITH_TIMEOUT(widget->width(), 0.0, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(groupHoldsExactlyItsDrawnChildren(group), 5000);

    // And the flag is about the readout rather than about the daemon: the shell is still the name's owner and
    // still holds what the sender sent, so a readout that came back would have something to draw. A widget that
    // stood the daemon down in order to hide itself fails here.
    QVERIFY2(notifications_->notificationAvailable(),
             "show_notifications false stood the shell down as the notification daemon");
    QCOMPARE(notifications_->notificationSummary(), summary);

    // Back to what the shell ships: the readout returns with the sender's text rather than with an empty state,
    // and the group accounts for it again. Nothing about its absence was permanent.
    config_.apply(ConfigValues{});
    QTRY_VERIFY_WITH_TIMEOUT(widget->isVisible(), 5000);
    QVERIFY2(widget->width() > 0, "the notification readout came back with no room to draw in");
    QTRY_COMPARE_WITH_TIMEOUT(textOf(QStringLiteral("notificationSummary")), summary, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(groupHoldsExactlyItsDrawnChildren(group), 5000);

    // The other half of the widget's rule, with the flag in both states. `showNotifications` is the
    // configuration's half and `notificationAvailable` is the daemon's, and they are separate questions: this
    // is the one the bus answers rather than the file. The readout goes, and it stays gone with the flag off as
    // well — which is the state every other slot in this file reads the bar's arrangement in.
    theShellStopsBeingTheNotificationDaemon();

    QTRY_VERIFY_WITH_TIMEOUT(!widget->isVisible(), 5000);
    QCOMPARE(widget->width(), 0.0);
    values.bar.notifications.showNotifications = false;
    config_.apply(values);
    QVERIFY2(!widget->isVisible(), "the notification readout is drawn with no daemon and the flag off");
    QCOMPARE(widget->width(), 0.0);
    QVERIFY2(!notifications_->notificationAvailable(), "the shell is the notification daemon again");

    // The defaults, so a slot running after this one reads the configuration the shell ships rather than this
    // slot's last edit. The daemon itself is already left in the state the rest of the file expects: the shell
    // is not the name's owner.
    config_.apply(ConfigValues{});
}

void BarInteractionTest::theBatteryReadoutSitsInTheTrailingGroupAndShowsItsEmptyState() {
    auto* bar = qobject_cast<QQuickItem*>(bar_.get());
    QVERIFY(bar != nullptr);

    QQuickItem* widget = itemNamed(QStringLiteral("battery"));
    QQuickItem* group = itemNamed(QStringLiteral("right"));
    QVERIFY2(widget != nullptr, "the bar has no battery readout");
    QVERIFY2(group != nullptr, "the bar has no group named right");
    QCOMPARE(widget->parentItem(), group);

    // Drawn with no file at all, and with a real width: the readout is on the bar rather than a zero-width
    // nothing that would satisfy a visibility check alone.
    QVERIFY2(widget->isVisible(), "the battery readout is hidden by default");
    QVERIFY2(widget->width() > 0, "the battery readout has no width to draw in");
    QCOMPARE(widget->property("drawn").toBool(), true);

    // The empty state, and it is a real check rather than a permanent label. The service is registered and has
    // never been started — its bus connection is one that was never made — so `available` is false and the
    // widget draws a dash. Never a percentage: that is a *reading*, and a bar that drew one from the daemon's
    // silence would be claiming a charge nobody reported.
    //
    // This is also the assertion that pins the *shape* of the `drawn` binding, in the one direction that is
    // observable without a daemon on this machine: a widget bound to `present` alone would be hidden here,
    // where the honest rule is "a dash while I have not heard, nothing when the daemon says there is no
    // battery". The other direction is a comparison against `shouldDraw(true, false)` in the slot below,
    // because producing it from a service needs an upowerd.
    QVERIFY2(!battery_->available(), "a service that was never started reports a reading");
    QVERIFY2(!battery_->present(), "a service that was never started reports a battery");
    QCOMPARE(textOf(QStringLiteral("batteryValue")), QString::fromUtf8("—"));

    // The three flags the widget reads are the configuration's, and with no file at all they are the schema's
    // defaults: the readout drawn, and both of its readings drawn inside it.
    QCOMPARE(widget->property("shown").toBool(), true);
    QCOMPARE(widget->property("levelShown").toBool(), true);
    QCOMPARE(widget->property("timeShown").toBool(), true);
}

void BarInteractionTest::theBatteryReadoutDrawsWhatTheDaemonReportsAndTheConfigurationNames() {
    QQuickItem* widget = itemNamed(QStringLiteral("battery"));
    QVERIFY(widget != nullptr);

    // The tokens the widget switches on, compared with the module's own builders. Every branch below is written
    // against these strings, so a token renamed on one side leaves the widget drawing the wrong branch — which
    // is why the comparison is here, where the rendering rule is exercised, rather than nowhere at all.
    QCOMPARE(quantum::dbus::batteryStateToken(quantum::dbus::BatteryState::Unknown),
             QStringLiteral("unknown"));
    QCOMPARE(quantum::dbus::batteryStateToken(quantum::dbus::BatteryState::Charging),
             QStringLiteral("charging"));
    QCOMPARE(quantum::dbus::batteryStateToken(quantum::dbus::BatteryState::Discharging),
             QStringLiteral("discharging"));
    QCOMPARE(quantum::dbus::batteryStateToken(quantum::dbus::BatteryState::Empty), QStringLiteral("empty"));
    QCOMPARE(quantum::dbus::batteryStateToken(quantum::dbus::BatteryState::Full),
             QStringLiteral("fully-charged"));
    QCOMPARE(quantum::dbus::batteryStateToken(quantum::dbus::BatteryState::PendingCharge),
             QStringLiteral("pending-charge"));
    QCOMPARE(quantum::dbus::batteryStateToken(quantum::dbus::BatteryState::PendingDischarge),
             QStringLiteral("pending-discharge"));
    QCOMPARE(quantum::dbus::batteryWarningToken(quantum::dbus::BatteryWarning::Unknown),
             QStringLiteral("unknown"));
    QCOMPARE(quantum::dbus::batteryWarningToken(quantum::dbus::BatteryWarning::None), QStringLiteral("none"));
    QCOMPARE(quantum::dbus::batteryWarningToken(quantum::dbus::BatteryWarning::Discharging),
             QStringLiteral("discharging"));
    QCOMPARE(quantum::dbus::batteryWarningToken(quantum::dbus::BatteryWarning::Low), QStringLiteral("low"));
    QCOMPARE(quantum::dbus::batteryWarningToken(quantum::dbus::BatteryWarning::Critical),
             QStringLiteral("critical"));
    QCOMPARE(quantum::dbus::batteryWarningToken(quantum::dbus::BatteryWarning::Action),
             QStringLiteral("action"));

    // The widget's own rule for whether there is anything to draw, called with the four combinations of the
    // daemon's two answers. Nothing in this test has a daemon — the service's bus connection was never made —
    // and the rule is a function of its inputs for exactly that reason: the case that needs a desktop is the
    // one this comparison supplies, and a rule that drew a dash on a machine with no battery would be the
    // readout lying about a fact the daemon stated.
    const auto drawsAt = [&widget](bool available, bool present) {
        QVariant answer;
        QMetaObject::invokeMethod(widget, "shouldDraw", Q_RETURN_ARG(QVariant, answer), Q_ARG(QVariant, available),
                                  Q_ARG(QVariant, present));
        return answer.toBool();
    };
    QVERIFY2(!drawsAt(true, false), "a machine the daemon says has no battery still draws a readout");
    QVERIFY2(drawsAt(true, true), "a battery the daemon reports is not drawn");
    QVERIFY2(drawsAt(false, false), "a daemon that has not answered draws nothing instead of a dash");
    QVERIFY2(drawsAt(false, true), "the state before the first reading draws nothing");

    // The widget's rendering rule, called with the tokens and numbers UPower sends. A comparison with a
    // literal is a stronger claim than one against a re-derivation, and the alternative would be a rule nothing
    // can exercise without a battery on the machine running the suite. What a real daemon does to the *reading*
    // is `battery-test`'s, against a fake daemon it drives, and the binding below is what says the rule is
    // handed those readings.
    const auto drawsWith = [&widget](bool available, bool present, bool hasPercentage, int percentage,
                                     const QString& state, const QString& warning, bool hasTime,
                                     const QString& timeText, bool showPercentage, bool showTime) {
        QVariant text;
        QMetaObject::invokeMethod(widget, "textFor", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, available),
                                  Q_ARG(QVariant, present), Q_ARG(QVariant, hasPercentage),
                                  Q_ARG(QVariant, percentage), Q_ARG(QVariant, state), Q_ARG(QVariant, warning),
                                  Q_ARG(QVariant, hasTime), Q_ARG(QVariant, timeText),
                                  Q_ARG(QVariant, showPercentage), Q_ARG(QVariant, showTime));
        return text.toString();
    };

    // Named rather than assumed: with the rule gone every comparison below would fail with an empty actual and
    // nothing saying why, and this is the line that says the name looked for.
    QVERIFY2(!drawsWith(true, true, true, 42, QStringLiteral("charging"), QStringLiteral("none"), true,
                        QStringLiteral("2:15"), true, true)
                  .isEmpty(),
             "the battery readout has no textFor() rule, so nothing below is being asked of it");

    // A battery being charged, drawn as the daemon's own facts: the level, the time to full and the word for
    // where the charge is going. `fully-charged` is the daemon's spelling of its own enum, which is why the
    // word is not shortened here — the token is what the daemon says, not a vocabulary this file invents.
    QCOMPARE(drawsWith(true, true, true, 42, QStringLiteral("charging"), QStringLiteral("none"), true,
                       QStringLiteral("2:15"), true, true),
             QStringLiteral("42% 2:15 charging"));
    QCOMPARE(drawsWith(true, true, true, 100, QStringLiteral("fully-charged"), QStringLiteral("none"), false,
                       QString(), true, true),
             QStringLiteral("100% fully-charged"));
    QCOMPARE(drawsWith(true, true, true, 3, QStringLiteral("empty"), QStringLiteral("none"), false, QString(),
                       true, true),
             QStringLiteral("3% empty"));
    QCOMPARE(drawsWith(true, true, true, 60, QStringLiteral("pending-charge"), QStringLiteral("none"), true,
                       QStringLiteral("0:40"), true, true),
             QStringLiteral("60% 0:40 pending-charge"));
    QCOMPARE(drawsWith(true, true, true, 60, QStringLiteral("pending-discharge"), QStringLiteral("none"), true,
                       QStringLiteral("1:05"), true, true),
             QStringLiteral("60% 1:05 pending-discharge"));

    // Discharging and unknown draw no word: the first is what a laptop does all day and the second is the
    // daemon not having decided, so neither is spelled — and `unknown` still has a readout, which is the
    // direction a widget that drew nothing for a state it did not recognise would get wrong.
    QCOMPARE(drawsWith(true, true, true, 74, QStringLiteral("discharging"), QStringLiteral("none"), true,
                       QStringLiteral("4:12"), true, true),
             QStringLiteral("74% 4:12"));
    QCOMPARE(drawsWith(true, true, true, 74, QStringLiteral("unknown"), QStringLiteral("unknown"), true,
                       QStringLiteral("4:12"), true, true),
             QStringLiteral("74% 4:12"));

    // The daemon's warning, which is its own judgement rather than a threshold this file applies: the level is
    // drawn as the word UPower uses and it supersedes the state word, so a low battery being charged says `low`
    // and not `low charging` — the colour the widget also applies repeats that word instead of carrying it
    // alone, which is the rule the network readout applies to a captive portal.
    QCOMPARE(drawsWith(true, true, true, 9, QStringLiteral("discharging"), QStringLiteral("low"), true,
                       QStringLiteral("0:20"), true, true),
             QStringLiteral("9% 0:20 low"));
    QCOMPARE(drawsWith(true, true, true, 20, QStringLiteral("charging"), QStringLiteral("critical"), true,
                       QStringLiteral("0:35"), true, true),
             QStringLiteral("20% 0:35 critical"));
    QCOMPARE(drawsWith(true, true, true, 20, QStringLiteral("charging"), QStringLiteral("action"), true,
                       QStringLiteral("0:10"), true, true),
             QStringLiteral("20% 0:10 action"));
    // `none` and `discharging` are the daemon's levels for "nothing wrong" and "this is where the charge is
    // going", and neither is trouble: they fall through to the state word and to nothing at all.
    QCOMPARE(drawsWith(true, true, true, 50, QStringLiteral("discharging"), QStringLiteral("discharging"),
                       true, QStringLiteral("1:30"), true, true),
             QStringLiteral("50% 1:30"));

    // A part of the reading the daemon did not publish is left out rather than drawn as zero: no level is no
    // number, and no clock is no time — the two cases `hasPercentage` and `hasTime` exist for, and the reason
    // neither is a sentinel value.
    QCOMPARE(drawsWith(true, true, false, 0, QStringLiteral("charging"), QStringLiteral("none"), true,
                       QStringLiteral("2:15"), true, true),
             QStringLiteral("2:15 charging"));
    QCOMPARE(drawsWith(true, true, true, 42, QStringLiteral("discharging"), QStringLiteral("none"), false,
                       QString(), true, true),
             QStringLiteral("42%"));

    // The two flags that decide which of those is drawn. With the level off the time stands alone; with the
    // time off the level does; with both off the readout falls back to the daemon's state word rather than to an
    // empty value beside a label, which is what makes the two flags a choice about what is drawn rather than a
    // way to switch the readout off — `show_status` is that, and it is a different key.
    QCOMPARE(drawsWith(true, true, true, 42, QStringLiteral("charging"), QStringLiteral("none"), true,
                       QStringLiteral("2:15"), false, true),
             QStringLiteral("2:15 charging"));
    QCOMPARE(drawsWith(true, true, true, 42, QStringLiteral("charging"), QStringLiteral("none"), true,
                       QStringLiteral("2:15"), true, false),
             QStringLiteral("42% charging"));
    QCOMPARE(drawsWith(true, true, true, 42, QStringLiteral("discharging"), QStringLiteral("none"), true,
                       QStringLiteral("2:15"), false, false),
             QStringLiteral("discharging"));

    // The two ways there is nothing to draw, each from the argument that decides it: a daemon that has not
    // answered is a dash even when the values handed in look like a reading, and a daemon that has answered that
    // there is no battery is nothing at all — not a dash, which would claim a battery whose level cannot be
    // read.
    QCOMPARE(drawsWith(false, true, true, 42, QStringLiteral("charging"), QStringLiteral("none"), true,
                       QStringLiteral("2:15"), true, true),
             QString::fromUtf8("—"));
    QCOMPARE(drawsWith(true, false, true, 42, QStringLiteral("charging"), QStringLiteral("none"), true,
                       QStringLiteral("2:15"), true, true),
             QString());

    // And each flag reaches the widget the way a person sets it: written into a real configuration file, parsed
    // by the schema and applied, with the widget's own property read back. That is what a renamed key cannot
    // survive — the schema refuses a spelling it does not know — and it is the half a rule comparison cannot
    // see, because a widget that ignored the flags and drew everything would pass every comparison above.
    const auto applyBattery = [this](const QByteArray& body) {
        const quantum::config::ParseResult parsed = quantum::config::parseConfig(
            QByteArrayLiteral("schema_version = 1\n") + body);
        QVERIFY2(parsed.errors.isEmpty() && parsed.warnings.isEmpty(),
                 qPrintable(parsed.errors.join(QStringLiteral("; "))
                            + parsed.warnings.join(QStringLiteral("; "))));
        config_.apply(parsed.values);
    };
    applyBattery(QByteArrayLiteral("[bar.battery]\nshow_percentage = false\nshow_time = false\n"));
    QCOMPARE(widget->property("levelShown").toBool(), false);
    QCOMPARE(widget->property("timeShown").toBool(), false);
    applyBattery(QByteArrayLiteral("[bar.battery]\nshow_status = false\nshow_percentage = true\n"));
    QCOMPARE(widget->property("shown").toBool(), false);
    QCOMPARE(widget->property("levelShown").toBool(), true);
    // A hidden readout is hidden *and* has taken its room out of the bar, which is the half a visibility check
    // alone would miss.
    QVERIFY2(!widget->isVisible(), "the battery readout is drawn with show_status false");
    QTRY_COMPARE_WITH_TIMEOUT(widget->width(), 0.0, 5000);

    // Back to what the shell ships, so a slot running after this one reads the defaults — and the readout is a
    // dash again under them, because what a flag decides is what is drawn and never whether a reading exists.
    config_.apply(ConfigValues{});
    QTRY_VERIFY_WITH_TIMEOUT(widget->isVisible(), 5000);
    QCOMPARE(widget->property("shown").toBool(), true);
    QCOMPARE(widget->property("levelShown").toBool(), true);
    QCOMPARE(widget->property("timeShown").toBool(), true);
    QCOMPARE(textOf(QStringLiteral("batteryValue")), QString::fromUtf8("—"));
}

void BarInteractionTest::aClickAndAWheelOnTheVolumeReadoutReachTheService() {
    QQuickItem* widget = itemNamed(QStringLiteral("volume"));
    QVERIFY2(widget != nullptr, "the bar has no volume readout to click");
    QVERIFY2(widget->width() > 0, "the readout has no width, so a pointer has nowhere to land");
    const QPointF overReadout = centreOf(widget);
    // Counted rather than assumed: how many requests the earlier slots made is not this slot's business, and
    // asserting a running total would be asserting an order — which is the one thing these tests are checked
    // not to depend on.
    const int requestsBefore = requestsSent();

    // A left click, delivered as a real window event through the platform layer the way a person's pointer
    // arrives. The record it produces is the service refusing to mute with no sink behind it — which is only
    // written if the click got as far as the service, so this fails if the mouse area is removed or if the
    // widget it fills is not the one on the bar.
    const QtMessageHandler previous = qInstallMessageHandler(&captureRecords);
    records.clear();
    QTest::mouseClick(window_.get(), Qt::LeftButton, Qt::NoModifier, overReadout.toPoint());
    QTest::qWait(50);
    qInstallMessageHandler(previous);

    QVERIFY2(!records.isEmpty(), "a click on the volume readout reached nothing");
    QVERIFY2(mentionsRecord(QStringLiteral("no reading to mute")),
             qPrintable(QStringLiteral("the click did not ask the service to mute; records: %1")
                            .arg(records.size())));
    QVERIFY2(records.first().category == quantum::app::audioLog().categoryName(),
             qPrintable(records.first().category));

    // And the wheel, in both directions, each reaching the service's own step. A wheel over the readout must
    // not be the strip's gesture: the two are different widgets and the requests they make are different
    // things, so a wheel here that moved workspaces would be a widget declaring a gesture it does not own.
    for (const int delta : {120, -120}) {
        const QtMessageHandler handler = qInstallMessageHandler(&captureRecords);
        records.clear();
        QTest::wheelEvent(window_.get(), overReadout, QPoint(0, delta));
        QTest::qWait(50);
        qInstallMessageHandler(handler);
        QVERIFY2(mentionsRecord(QStringLiteral("no reading to step from")),
                 qPrintable(QStringLiteral("a wheel of %1 over the volume readout reached nothing").arg(delta)));
        QVERIFY2(!mentionsRecord(QStringLiteral("FocusWorkspace")),
                 "a wheel over the volume readout moved workspaces");
    }

    // Nothing was asked of the compositor by any of it, which is the same claim from the other side: the bar's
    // two gestures are different gestures.
    QCOMPARE(requestsSent(), requestsBefore);
}

void BarInteractionTest::aGroupGivesEveryWidgetOfItTheSameHeightAndOrder() {
    // The group component on its own, so it can be driven with widgets of sizes the bar itself happens not
    // to have: the bar's two are both its own height, which would not tell "the group sized them" apart from
    // "they happened to be that size".
    QQmlComponent groupComponent(engine_.get(), QUrl::fromLocalFile(QStringLiteral(QS_GROUP_QML)));
    QVERIFY2(groupComponent.isReady(), qPrintable(groupComponent.errorString()));
    std::unique_ptr<QObject> groupObject(groupComponent.create());
    QVERIFY2(groupObject != nullptr, qPrintable(groupComponent.errorString()));
    auto* group = qobject_cast<QQuickItem*>(groupObject.get());
    QVERIFY(group != nullptr);

    // A group is given its height by the bar it is declared in — `Bar.qml` binds it to the bar's — so the
    // test gives it one the same way.
    group->setHeight(barHeight);

    // Three widgets of three different natural heights, none of them the group's, and three different widths
    // so a swap of two of them would show up in the positions as well. They are this test's items: what is
    // under test is the group, which has no opinion about what a widget draws.
    //
    // Held here, because being a child of the group is not the same as the group owning it: `setParentItem`
    // puts an item in the scene graph, and an item C++ creates is deleted by whoever wrote `new` — a QML item
    // belongs to the engine that made it. These three used to be created parentless and only re-parented, and
    // the sanitizer run reported exactly them as a leak; passing the group as the constructor's parent instead
    // fixes the ownership but drops them out of the group's item list, so they stop being laid out at all.
    // Both calls, then, and the ownership here — where the group dies after them.
    auto first = std::make_unique<QQuickItem>();
    first->setWidth(30);
    first->setHeight(8);
    first->setParentItem(group);

    auto second = std::make_unique<QQuickItem>();
    second->setWidth(45);
    second->setHeight(40);
    second->setParentItem(group);

    auto third = std::make_unique<QQuickItem>();
    third->setWidth(20);
    third->setHeight(22);
    third->setParentItem(group);

    // A window, because positioning happens in a window's polish pass: a group nothing is showing would lay
    // nothing out, and every assertion below would pass against zeroes it never earned.
    auto groupWindow = std::make_unique<QQuickWindow>();
    groupWindow->resize(200, 60);
    group->setParentItem(groupWindow->contentItem());
    groupWindow->show();
    QVERIFY2(QTest::qWaitForWindowExposed(groupWindow.get()), "the group's window never became exposed");

    // The spacing is read from the group rather than written down here, so what is asserted is that the gap
    // between two widgets is the group's own and not a number that happens to match it.
    const qreal spacing = group->property("spacing").toDouble();
    QVERIFY2(spacing > 0, "the group reported no spacing, so nothing here would tell order from overlap");

    // The height convention: every widget of the group is the group's height, whatever it asked for.
    QTRY_COMPARE_WITH_TIMEOUT(first->height(), qreal(barHeight), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(second->height(), qreal(barHeight), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(third->height(), qreal(barHeight), 5000);

    // The order is the order they were declared in, each one starting after the width of the one before it
    // plus the group's spacing — so a widget that is declared here is placed here, and no widget's own
    // geometry decided any of it.
    QTRY_COMPARE_WITH_TIMEOUT(first->x(), 0.0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(second->x(), 30.0 + spacing, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(third->x(), 30.0 + spacing + 45.0 + spacing, 5000);
}

void BarInteractionTest::theSystemStatusSitsInTheCentreGroup() {
    auto* bar = qobject_cast<QQuickItem*>(bar_.get());
    QVERIFY(bar != nullptr);

    QQuickItem* centre = itemNamed(QStringLiteral("centre"));
    QVERIFY2(centre != nullptr, "the bar has no group named centre");

    // Centred on the bar, which is what makes the middle group's placement the bar's decision rather than the
    // widget's: the widget inside it carries no anchor that could move it off the middle. A pixel of
    // tolerance, and not because the anchor is approximate: the group's width is its widget's, that width
    // comes from font metrics and is therefore fractional, and the compositor places surfaces on whole logical
    // pixels — so "the middle" is exact in the arithmetic and within a pixel on screen.
    const double centreOfBar = bar->width() / 2.0;
    const double centreOfGroup = centre->x() + centre->width() / 2.0;
    QVERIFY2(qAbs(centreOfGroup - centreOfBar) <= 1.0,
             qPrintable(QStringLiteral("the centre group is at %1 where the bar's middle is %2")
                            .arg(centreOfGroup)
                            .arg(centreOfBar)));
    QCOMPARE(centre->height(), bar->height());

    const QList<QQuickItem*> widgets = centre->childItems();
    QCOMPARE(widgets.size(), 1);
    QCOMPARE(widgets.first()->objectName(), QStringLiteral("systemMonitor"));

    // The height convention as it lands on this widget: the group's height, at the group's leading edge, so
    // no part of its position was written down by the widget.
    QCOMPARE(widgets.first()->height(), centre->height());
    QCOMPARE(widgets.first()->x(), 0.0);
    QCOMPARE(widgets.first()->y(), 0.0);
}

void BarInteractionTest::theSystemStatusDrawsTheMemoryFormTheConfigurationNames() {
    // The reading is this test's own: 11.5 GiB used of a 16 GiB machine, written into this test's
    // `/proc/meminfo` and taken by hand. So the four forms below are compared with four strings that are the
    // answer for *that* reading, worked out from its numbers — and the point of the fixture is that the answer
    // cannot change while the loop runs, whether the loop takes a millisecond or a second.
    //
    // `available` is the one worth a second look. `MemAvailable` is 4.5 GiB and the form rounds a GiB to no
    // decimal places, so it draws `5G` rather than `4G`: that value is a tie, chosen deliberately, because a
    // truncating rule and a rounding rule give different answers at it and the assertion can tell them apart.
    QVERIFY2(writeFixtureReading(4718592), "the fixture could not be written");
    sysMon_->sampleNow();
    QVERIFY2(sysMon_->memoryAvailable(), "one reading of the fixture produced no memory reading");

    // Each token is written into a configuration file and parsed by the schema rather than pushed into the
    // tree as a value. That is the path a person's file takes, and it is what puts the schema's own list
    // between the token and the widget: a token renamed on the schema's side is refused there (or, if the
    // schema still accepted it, would fall through the widget's branches), and either way this slot names it
    // instead of drawing the default and passing. The tokens are written out here rather than read from
    // `MemoryFormats`, because a list taken from the same place the widget switches on would agree with a
    // renamed one; `config-test` holds the schema's list to its own copy of these four at compile time.
    const struct {
        const char* token;
        const char* expected;
    } forms[] = {
        {"used_of_total", "11.5/16G"},
        {"used", "11.5G"},
        {"available", "5G"},
        {"percent", "72%"},
    };

    QStringList seen;
    for (const auto& form : forms) {
        const QByteArray text = QByteArrayLiteral("schema_version = 1\n[bar.system]\nmemory_format = \"")
            + QByteArray(form.token) + "\"\n";
        const quantum::config::ParseResult parsed = quantum::config::parseConfig(text);
        QVERIFY2(parsed.errors.isEmpty() && parsed.warnings.isEmpty(),
                 qPrintable(QStringLiteral("%1: %2 %3")
                                .arg(QString::fromLatin1(form.token),
                                     parsed.errors.join(QStringLiteral("; ")),
                                     parsed.warnings.join(QStringLiteral("; ")))));
        config_.apply(parsed.values);

        const QString drawn = textOf(QStringLiteral("memoryValue"));
        QCOMPARE(drawn, QString::fromLatin1(form.expected));
        // And the four are four different strings, so a widget that ignored the token and drew one form for
        // all of them could not pass the loop above by coincidence.
        QVERIFY2(!seen.contains(drawn),
                 qPrintable(QStringLiteral("%1 draws the same text as an earlier form: %2")
                                .arg(QString::fromLatin1(form.token), drawn)));
        seen.append(drawn);
    }

    // Back to what the shell ships *before* the two readings below, and that order is part of the assertion
    // rather than housekeeping. The loop above leaves its last token applied, so a reading compared before
    // this line would be drawn in the percent form — and a slot whose point is "the widget drew the number the
    // service published" would have asserted the form instead, which is a different claim and one the loop
    // has already made four times.
    config_.apply(ConfigValues{});

    // And the binding follows a reading rather than remembering one. The fixture's memory changes under it —
    // to 8 GiB available of the same 16 GiB, which is 8.0 used, on a machine that had 11.5 — and the widget
    // draws the new number because the service published it. A widget that had captured the first reading, or
    // a service that had, would still be drawing 11.5 here.
    QVERIFY2(writeFixtureReading(8388608), "the fixture could not be written");
    sysMon_->sampleNow();
    QCOMPARE(textOf(QStringLiteral("memoryValue")), QStringLiteral("8.0/16G"));

    // And back to the reading the fixture starts at, so a slot running after this one reads the memory this
    // file's comments describe rather than this slot's last edit.
    QVERIFY2(writeFixtureReading(4718592), "the fixture could not be written");
    sysMon_->sampleNow();
    QCOMPARE(textOf(QStringLiteral("memoryValue")), QStringLiteral("11.5/16G"));
}

void BarInteractionTest::aReadoutTheConfigurationHidesIsNotDrawn() {
    QQuickItem* cpuReadout = itemNamed(QStringLiteral("cpuReadout"));
    QQuickItem* memoryReadout = itemNamed(QStringLiteral("memoryReadout"));
    QVERIFY2(cpuReadout != nullptr, "the widget has no CPU readout");
    QVERIFY2(memoryReadout != nullptr, "the widget has no memory readout");

    // Both drawn with no file at all, which is what the schema defaults to and what the shell ships.
    QVERIFY2(cpuReadout->isVisible(), "the CPU readout is hidden by default");
    QVERIFY2(memoryReadout->isVisible(), "the memory readout is hidden by default");

    ConfigValues values;
    values.bar.system.showCpu = false;
    config_.apply(values);
    // The row the flag hides, label and number together — not the number with a label left behind.
    QVERIFY2(!cpuReadout->isVisible(), "the CPU readout is still drawn with show_cpu false");
    QVERIFY2(!itemNamed(QStringLiteral("cpuValue"))->isVisible(),
             "the CPU readout's row is hidden but its value is still visible");
    QVERIFY2(memoryReadout->isVisible(), "hiding the CPU readout hid the memory one as well");

    // And the other way round, so neither flag is being read from the other.
    values.bar.system.showCpu = true;
    values.bar.system.showMemory = false;
    config_.apply(values);
    QVERIFY(cpuReadout->isVisible());
    QVERIFY2(!memoryReadout->isVisible(), "the memory readout is still drawn with show_memory false");

    // Both hidden leaves the widget no width at all: the centre group is empty rather than holding a gap
    // where the readouts used to be.
    values.bar.system.showCpu = false;
    values.bar.system.showMemory = false;
    config_.apply(values);
    QQuickItem* widget = itemNamed(QStringLiteral("systemMonitor"));
    QVERIFY(widget != nullptr);
    // Waited for rather than read straight away, and not because the value is uncertain: a QML binding
    // re-evaluates when the property it reads changes, which for a width fed by a row's implicit width is
    // the next turn of the event loop rather than this line.
    QTRY_COMPARE_WITH_TIMEOUT(widget->width(), 0.0, 5000);

    // Put back, so no slot after this one reads this slot's edit.
    config_.apply(ConfigValues{});
    QVERIFY(cpuReadout->isVisible());
    QVERIFY(memoryReadout->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(widget->width() > 0, 5000);
}

void BarInteractionTest::theSystemStatusShowsAnEmptyStateBeforeThereIsAReading() {
    // The state a shell starts in, and the two moments that produce it: the bar appearing, which takes a
    // reading, and the bar going away, which forgets one. Brought about here rather than assumed, so the
    // slot does not depend on what the slot before it did — and it is worth saying which way round that is,
    // because "no reading" is a state the service enters by being deactivated rather than by never being
    // used. That is why appearing is preceded by a reading of the fixture here: appearing takes a sample, and
    // the sample has to be one the fixture can answer.
    QVERIFY2(writeFixtureReading(4718592), "the fixture could not be written");
    sysMon_->setActive(true);
    sysMon_->setActive(false);
    QTRY_VERIFY(!sysMon_->cpuAvailable());
    QTRY_VERIFY(!sysMon_->memoryAvailable());
    QCOMPARE(textOf(QStringLiteral("cpuValue")), QStringLiteral("—"));
    QCOMPARE(textOf(QStringLiteral("memoryValue")), QStringLiteral("—"));

    // Then a reading, taken by hand from the fixture: 11.5 GiB used of 16 GiB. The widget draws that number
    // rather than a dash, because the service published it — which is the binding, and the numbers are the
    // test's own so the string it draws is written down here.
    QVERIFY2(writeFixtureReading(4718592), "the fixture could not be written");
    sysMon_->sampleNow();
    QVERIFY2(sysMon_->memoryAvailable(), "one reading of the fixture produced no memory reading");
    QCOMPARE(textOf(QStringLiteral("memoryValue")), QStringLiteral("11.5/16G"));

    // And the CPU is *still* a dash, which is the other half of the empty state and the one a widget gets
    // wrong by reading the number instead of the flag: the aggregate counters are cumulative since boot, so
    // one reading is a baseline and not a percentage, and `cpuPercent` holds 0.0 while `cpuAvailable` is
    // false. A readout that drew the percentage property would print `0%` here.
    QVERIFY2(!sysMon_->cpuAvailable(), "a CPU percentage was published from a single reading");
    QCOMPARE(textOf(QStringLiteral("cpuValue")), QStringLiteral("—"));

    // The reading after it is the percentage: 1,000 jiffies passed on the fixture's aggregate line and 700 of
    // them were idle, so 300 were busy — 30%, which is what the widget draws.
    QVERIFY2(writeFixtureReading(4718592), "the fixture could not be written");
    sysMon_->sampleNow();
    QVERIFY2(sysMon_->cpuAvailable(), "two readings of the fixture produced no CPU percentage");
    QCOMPARE(textOf(QStringLiteral("cpuValue")), QStringLiteral("30%"));

    // And the one after that follows the counters rather than a percentage that was published once. The same
    // 1,000 jiffies with 400 of them idle is 600 busy — 60% — and a readout that had drawn the first
    // percentage and kept it, or a service that had stopped measuring, would still be saying 30 here.
    QVERIFY2(writeFixtureReading(4718592, 1000, 400), "the fixture could not be written");
    sysMon_->sampleNow();
    QCOMPARE(textOf(QStringLiteral("cpuValue")), QStringLiteral("60%"));

    // Nothing was ever armed here — every reading above was taken by hand — and the service is left inactive,
    // which is the state the slots after this one expect the bar to be in.
    QVERIFY2(sysMon_->findChild<QTimer*>() != nullptr, "the service has no timer to be armed or not");
    QVERIFY2(!sysMon_->findChild<QTimer*>()->isActive(), "this slot left a timer running");
}

void BarInteractionTest::theSystemStatusReadsTheMachineItRunsOn() {
    // The one slot here whose reading is the machine's own. Every other system-status slot writes the numbers
    // it asserts, and a suite of only those would pass on a service that never opened a file at all — a
    // fixture proves the parsing and the binding, and something has to prove the reading is real.
    //
    // The path is still this test's; that is the shell's own read path, though: the service opens
    // `<root>/stat` and `<root>/meminfo`, so a root whose two names are links to the machine's files is the
    // machine's reading through the same code with a different prefix.
    //
    // The baseline is cleared before the prefix changes, and that is not decoration either. The service
    // measures a percentage between the reading it holds and the next one, and a point held from the fixture —
    // 1,000 jiffies into a boot — compared with this machine's 11,720,371 is a pair from two different
    // machines. It is *not* caught by the refusal in `busyPercent`, which refuses a pair that went backwards:
    // this one goes forwards, because the machine's counters are the larger. What it measures instead is the
    // difference between the first second of a boot and now, which is the busy share of the whole boot — so
    // appearing and going away is how a service loses its baseline, the fixture answers the sample that
    // appearing takes, and the assertion on the first machine reading below is what holds the clearing in
    // place.
    QVERIFY2(writeFixtureReading(4718592), "the fixture could not be written");
    sysMon_->setActive(true);
    sysMon_->setActive(false);

    QVERIFY2(pointTheReadingsAtTheRealProc(), "the fixture directory could not be pointed at /proc");

    // The first reading of the machine is a baseline, not a percentage — and this is the assertion that says
    // *where the baseline came from*, which is the half of the clearing above that a comment cannot carry. A
    // point left over from the fixture makes the pair span two different machines, and that pair is not
    // refused: the fixture's counters are 1,000 jiffies against this machine's 11,720,371 of them (measured on
    // the machine this was written on), so the fixture's share is 0.0085% of the total and what falls out of
    // the difference is the busy share of this machine's whole *boot* — 12.07% there — published as though it
    // were the share of the interval, which was the one measurement this slot exists to make. Removed, this
    // line fails and names it.
    sysMon_->sampleNow();
    QVERIFY2(!sysMon_->cpuAvailable(), "a percentage was published before the machine had been read twice");

    // The memory half needs no interval — one read of a file is a reading — so it is published by that same
    // sample, and it is asserted before the wait rather than after a second sample taken to get it. That
    // second sample was here and is deliberately gone: two readings microseconds apart find no tick between
    // them on a `/proc/stat` this machine is writing, which the service refuses and records, so the slot was
    // provoking a refusal about its own sampling. A slot whose readings produce a record it does not assert
    // is a slot adding noise to somebody else's run, and this file's fixtures exist to avoid exactly that.
    QVERIFY2(sysMon_->memoryAvailable(), "the machine this test runs on produced no memory reading");

    // Two readings, one tick apart at least, which is what a percentage needs. The first of the two is the one
    // taken above, and it is the machine's as well. `/proc/stat` counts in `USER_HZ`, which is 100 Hz on every
    // architecture this builds on, so two readings taken microseconds apart can legitimately find no tick
    // between them — and a percentage over no interval is refused rather than published as zero. 200 ms is
    // twenty ticks, the same wait `sysmon-test` makes against the real /proc for the same reason.
    QTest::qWait(200);
    sysMon_->sampleNow();
    QVERIFY2(sysMon_->cpuAvailable(), "two readings of the real /proc produced no CPU percentage");

    // What the widget draws is the reading the service holds, both read in one expression with nothing
    // between them that turns the event loop: on a live machine the numbers move, so the pair has to describe
    // one sample rather than two.
    QCOMPARE(textOf(QStringLiteral("cpuValue")), drawsCpuPercent(*sysMon_));
    QCOMPARE(textOf(QStringLiteral("memoryValue")), drawsUsedOfTotal(*sysMon_));

    // And the reading is the machine's rather than a number the service made up. Two paths to the same file:
    // the service read it through this test's directory, and this reads `/proc/meminfo` itself.
    const std::optional<qulonglong> machineTotal = memInfoFieldFromTheMachine(QStringLiteral("MemTotal"));
    const std::optional<qulonglong> machineAvailable =
        memInfoFieldFromTheMachine(QStringLiteral("MemAvailable"));
    QVERIFY2(machineTotal.has_value() && machineAvailable.has_value(),
             "/proc/meminfo could not be read here");
    // `MemTotal` is fixed for the boot, so this is an equality and not a near miss.
    QCOMPARE(sysMon_->memoryTotalKb(), *machineTotal);
    QVERIFY2(machineTotal > 0, "a machine with no memory at all");
    QVERIFY2(sysMon_->memoryUsedKb() <= sysMon_->memoryTotalKb(), "used memory exceeds total memory");
    QVERIFY2(sysMon_->cpuPercent() >= 0.0 && sysMon_->cpuPercent() <= 100.0,
             qPrintable(QStringLiteral("a share of %1% is not a share of anything")
                            .arg(sysMon_->cpuPercent())));

    // `MemAvailable` moves, so it is compared with a bound rather than for equality, and the bound is the
    // honest part of the claim. A gigabyte is loose enough that an unrelated process taking memory between
    // the two reads does not fail this run — allocating and touching 0.68 GiB is what that does to this
    // number, measured — and tight enough to say something: the field this shell must not be reporting,
    // `MemFree`, was 18.3 GiB away from `MemAvailable` on the machine this was measured on, because
    // `MemAvailable` counts reclaimable page cache and `MemFree` does not.
    const qint64 difference = qAbs(qint64(sysMon_->memoryAvailableKb()) - qint64(*machineAvailable));
    QVERIFY2(difference < 1048576,
             qPrintable(QStringLiteral("the service reports %1 KiB available where /proc/meminfo says %2: "
                                       "%3 KiB apart")
                            .arg(sysMon_->memoryAvailableKb())
                            .arg(*machineAvailable)
                            .arg(difference)));

    // And left as the slots that read the fixture expect to find it: inactive, holding no reading, with no
    // baseline to measure the next one against. The baseline matters as much as the reading does, because
    // whichever slot samples next may be a fixture one and its counters are the fixture's small ones — a pair
    // spanning the two would be refused and recorded, and this slot would have contributed a record to a run
    // it is not asserting anything about. The wait is the interval: `/proc/stat` counts in `USER_HZ`, so a
    // tick has to pass between the last reading and this one for the sample itself not to be that refusal.
    QTest::qWait(50);
    sysMon_->setActive(true);
    sysMon_->setActive(false);
    QVERIFY2(!sysMon_->cpuAvailable() && !sysMon_->memoryAvailable(),
             "this slot left a reading behind rather than the empty state");
}

// LeakSanitizer's suppressions for this binary, and this binary only.
//
// This is the one test that loads Qt Quick: creating a window and loading the bar's QML pulls in font
// configuration (fontconfig, pango, cairo), a GL driver and Qt's own scene-graph render context, each of
// which holds process-lifetime allocations that LeakSanitizer reports as leaks at exit. They are not this
// project's to free, and without these rules the sanitizer run is red for a reason that says nothing
// about this repository — a check that is always red is a check nobody reads. No rule names a symbol in
// `src/`, so a leak in the shell's own code still fails this run, which is why the list is libraries
// rather than `detect_leaks=0`.
//
// The hook rather than `LSAN_OPTIONS=suppressions=<path>`, which is parsed by splitting on spaces: this
// checkout lives under a path with one in it, so an environment variable cannot name the file. The
// function only exists in a sanitized build, which is the only build whose runtime looks for it.
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
extern "C" const char* __lsan_default_suppressions();
extern "C" const char* __lsan_default_suppressions() {
    return "leak:libfontconfig\n"
           "leak:libpango\n"
           "leak:libpangocairo\n"
           "leak:libcairo\n"
           "leak:libnvidia\n"
           "leak:libEGL\n"
           "leak:libGLX\n"
           "leak:libgbm\n"
           "leak:libdbus-1\n"
           "leak:QSGDefaultRenderContext\n"
           "leak:QSGRenderContext\n";
}
#endif

// The platform plugin and the scene graph backend, set here rather than in the test's environment: the
// slot-order checks run this binary directly, once per slot and in shuffled order, and inherit no ctest
// environment properties.
//
// They are set unconditionally rather than only when unset, and that is a correction rather than a
// preference. On a Wayland session `QT_QPA_PLATFORM` is already `wayland` — the session's choice for the
// shell, not a choice anyone made for this binary — so the earlier "a value the environment carries wins"
// rule meant this test connected to the compositor on a developer's machine and to nothing at all in CI,
// where the variable is unset. The connection was visible under the sanitizer: libwayland allocates a proxy
// for it and does not free it at exit, which LeakSanitizer reported as 96 bytes in
// `wl_proxy_marshal_array_flags` with no frame of this repository's on the stack. Forcing the same plugin
// here makes a run on a session identical to a run in CI, and the report goes away rather than being
// suppressed — nothing this binary asserts is a pixel or a surface.
//
// The backend is `software` for two measured reasons. Nothing asserted here is a pixel — the test reads
// what the gestures asked the compositor for — so the renderer is not part of the claim. And initialising
// GL under the offscreen plugin loads a driver (NVIDIA's here) whose process-lifetime allocations
// LeakSanitizer reports with no module name on the stack, which no `leak:` rule can match; the software
// backend does not load it, and the same run under AddressSanitizer goes from red to green and from
// 1178 ms to 306 ms by asking for it. The GL path's remaining third-party leaks are suppressed in the
// list below, which stays because fontconfig's are still there either way.
int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("QT_QUICK_BACKEND", QByteArrayLiteral("software"));
    QGuiApplication application(argc, argv);
    BarInteractionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "bar_interaction_test.moc"
