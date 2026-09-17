// The network module: what NetworkManager actually publishes, read two ways.
//
// The first half is the pure half — the D-Bus names, the daemon's numbers as tokens, the property map as a
// function of the bytes that arrived, and the reading the four objects of the chain add up to. Every case there
// is a string or a map and needs nothing running, which is what makes the shapes a healthy daemon never sends
// testable at all: an entity wrapped where a value was expected, a state that arrived as text, a strength above
// what a byte holds.
//
// The second half is the service against a daemon's end of the bus, and neither is a stub: the fake is a
// `QDBusVirtualObject` on a bus of this test's own that answers `GetAll` the way NetworkManager answers it and
// announces changes by building the `PropertiesChanged` signal itself. Three things follow from that and each
// is a claim in this file. The shell's *reading* is checked against payloads it did not choose. The shell's
// *asking* is counted, which is the only way "this module is driven by signals and not by a schedule" can be
// measured rather than asserted. And an object that goes away can be arranged to answer late, which is the one
// race in this module that a real daemon cannot be asked to produce on demand.
//
// The bus is a `dbus-daemon` this test starts in a scratch directory of its own, so the name
// `org.freedesktop.NetworkManager` — the daemon the desktop is actually using — is free for the fake to own
// here and can never be taken from it. That is the same rule `SysMonService`'s directory and
// `PipeWireService`'s remote follow: a test names its own instance of the real thing.
#include "app/Logging.h"
#include "dbus/NetworkService.h"
#include "dbus/NetworkStatus.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVirtualObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <memory>
#include <string_view>

namespace {

// The daemon's well-known name, and the paths this test's fake answers at. They are the same strings the module
// declares — the compile-time mirror below is what holds the two together — and the fake uses the module's own
// constants rather than repeating them, because a fake that spelled a path differently from the daemon would be
// a test of a mistake rather than of the code.
using quantum::dbus::AccessPointInterface;
using quantum::dbus::ActiveConnectionInterface;
using quantum::dbus::DeviceInterface;
using quantum::dbus::ManagerInterface;
using quantum::dbus::ManagerPath;
using quantum::dbus::NetworkManagerService;
using quantum::dbus::WirelessInterface;

// Where this test's daemon and objects live. The device path is one of NetworkManager's own shapes
// (`/org/freedesktop/NetworkManager/Devices/3` on the session this was written against), so a reader that
// hard-coded a path rather than following the chain would still have to follow it here.
constexpr auto kConnectionPath = "/org/freedesktop/NetworkManager/ActiveConnection/2";
constexpr auto kDevicePath = "/org/freedesktop/NetworkManager/Devices/3";
constexpr auto kAccessPointPath = "/org/freedesktop/NetworkManager/AccessPoint/1";
// A second chain, for the case where the daemon moves to another connection while the shell is following the
// first one.
constexpr auto kOtherConnectionPath = "/org/freedesktop/NetworkManager/ActiveConnection/5";
constexpr auto kOtherDevicePath = "/org/freedesktop/NetworkManager/Devices/4";
constexpr auto kOtherAccessPointPath = "/org/freedesktop/NetworkManager/AccessPoint/9";

// A bus of this test's own. The address is read from the daemon's own output rather than written down, because
// the socket it listens on is a path under `/tmp` that the daemon chooses.
class PrivateBus {
public:
    bool start() {
        // `--nofork` and the address on stdout, which is the reference daemon's own way of telling a parent
        // where its bus is. Nothing about the desktop's bus is read, touched or assumed.
        process_.start(QStringLiteral("dbus-daemon"),
                       {QStringLiteral("--session"), QStringLiteral("--print-address=1"),
                        QStringLiteral("--nofork")});
        if (!process_.waitForStarted(5000)) {
            return false;
        }
        if (!process_.waitForReadyRead(5000)) {
            return false;
        }
        address_ = QString::fromUtf8(process_.readLine()).trimmed();
        return !address_.isEmpty();
    }

    QString address() const { return address_; }

    ~PrivateBus() {
        process_.terminate();
        if (!process_.waitForFinished(3000)) {
            process_.kill();
            process_.waitForFinished(3000);
        }
    }

private:
    QProcess process_;
    QString address_;
};

// The daemon's end of the bus. A `QDBusVirtualObject` rather than a registered `QObject` with properties, and
// deliberately: Qt's own property handling would answer `GetAll` for us, from a meta-object rather than from the
// payloads the daemon sends, and a double that answers through a different mechanism is a test of Qt's
// marshalling rather than of this module's reading of what arrives. A virtual object sees the call as it came in
// and answers it here — which is also what makes a *late* reply arrangeable, the case that catches a reply from
// a daemon that is already gone.
class FakeNetworkManager : public QDBusVirtualObject {
public:
    FakeNetworkManager(QDBusConnection connection, QObject* parent)
        : QDBusVirtualObject(parent), connection_(std::move(connection)) {
        for (const QString& path : {QString::fromLatin1(ManagerPath), QString::fromLatin1(kConnectionPath),
                                    QString::fromLatin1(kDevicePath), QString::fromLatin1(kAccessPointPath),
                                    QString::fromLatin1(kOtherConnectionPath),
                                    QString::fromLatin1(kOtherDevicePath),
                                    QString::fromLatin1(kOtherAccessPointPath)}) {
            connection_.registerVirtualObject(path, this);
        }
    }

    // The daemon's own answer to `Introspectable`. An empty document is honest for a double whose subject is
    // `Properties.GetAll`, and `busctl introspect` against it fails rather than lying.
    QString introspect(const QString&) const override { return QString(); }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection&) override {
        // The object is the message's own path — a virtual object is registered once and answers for every path
        // it was registered under, so which object is being asked about is the caller's argument and not a
        // property of the call.
        const QString path = message.path();
        if (message.interface() != QLatin1String("org.freedesktop.DBus.Properties")
            || message.member() != QLatin1String("GetAll")) {
            return false;
        }
        ++calls_;
        ++callsByPath_[path];
        const auto arguments = message.arguments();
        const QString interface = arguments.isEmpty() ? QString() : arguments.first().toString();

        QDBusMessage reply = message.createReply();
        const auto known = propertiesFor(path, interface);
        if (known == nullptr) {
            // An object the daemon has not described — or an interface it does not have — is the daemon's own
            // `UnknownInterface` refusal rather than an empty map, because the two mean different things to a
            // reader: empty is an object with no properties, and this is an object that cannot be read at all.
            reply = message.createErrorReply(QDBusError::UnknownInterface,
                                             QStringLiteral("no such interface %1 on %2")
                                                 .arg(interface, path));
        } else {
            reply << *known;
        }

        // Sent from a timer even when the delay is zero, so that every reply in this test takes the asynchronous
        // path — a reply that arrived inline would let a caller pass without ever handling "not yet".
        const int delay = delayMs_;
        QTimer::singleShot(delay, this, [this, reply] { connection_.send(reply); });
        return true;
    }

    bool own() { return connection_.registerService(QString::fromLatin1(NetworkManagerService)); }

    // Gives the name up, which is what a daemon stopping on the bus looks like: the bus announces the change and
    // every client watching the name is told. Stopping the process would do the same and take the objects with
    // it, which is why the name is what this test moves.
    void release() { connection_.unregisterService(QString::fromLatin1(NetworkManagerService)); }

    void setProperties(const QString& path, const QString& interface, const QVariantMap& properties) {
        props_[path][interface] = properties;
    }

    // An announcement, built by hand with the signal's own signature: the interface whose properties moved, the
    // ones that moved, and the ones it invalidated. This is the whole of what makes the module event-driven, so
    // the test sends exactly what the daemon sends rather than calling a helper of the module's.
    void announce(const QString& path, const QString& interface, const QVariantMap& changed,
                  const QStringList& invalidated = {}) {
        QDBusMessage signal = QDBusMessage::createSignal(path, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                        QStringLiteral("PropertiesChanged"));
        signal << interface << changed << invalidated;
        connection_.send(signal);
        // The daemon's own copy follows, so a later `GetAll` answers with what the signal announced — which is
        // what the daemon does and what makes a read after a change agree with the change.
        for (auto it = changed.constBegin(); it != changed.constEnd(); ++it) {
            props_[path][interface].insert(it.key(), it.value());
        }
        for (const QString& name : invalidated) {
            props_[path][interface].remove(name);
        }
    }

    int calls() const { return calls_; }
    int callsFor(const QString& path) const { return callsByPath_.value(path); }
    void setDelayMs(int ms) { delayMs_ = ms; }

private:
    // The properties one object has under one interface, or nothing when the object has never been described —
    // which is the daemon's `UnknownInterface` case rather than an object with an empty property map.
    const QVariantMap* propertiesFor(const QString& path, const QString& interface) const {
        const auto object = props_.constFind(path);
        if (object == props_.constEnd()) {
            return nullptr;
        }
        const auto known = object->constFind(interface);
        return known == object->constEnd() ? nullptr : &known.value();
    }

    QDBusConnection connection_;
    QHash<QString, QHash<QString, QVariantMap>> props_;
    QHash<QString, int> callsByPath_;
    int calls_ = 0;
    int delayMs_ = 0;
};

// The chain this test's daemon publishes, in the shape NetworkManager publishes it: the manager names the
// connection, the connection names its device, the device's wireless half names the access point, and the access
// point carries the signal quality. The values are the ones the session this was written against reports —
// `State` 70 with `nmcli general` printing `connected`, `Connectivity` 4 printing `full`, a wifi device
// reporting `DeviceType` 2 and a strength of 74 — so a change in the module's reading of any of them shows up
// against numbers the daemon really sends.
void publishChain(FakeNetworkManager& daemon, const QString& connectionPath = QString::fromLatin1(kConnectionPath),
                  const QString& devicePath = QString::fromLatin1(kDevicePath),
                  const QString& accessPointPath = QString::fromLatin1(kAccessPointPath),
                  const QString& name = QStringLiteral("Victor Salin 5 GHz"),
                  const QString& connectionType = QStringLiteral("802-11-wireless"),
                  quint32 deviceType = 2, int strength = 74)
{
    daemon.setProperties(QString::fromLatin1(ManagerPath), QString::fromLatin1(ManagerInterface),
                         {{QStringLiteral("State"), quint32(70)},
                          {QStringLiteral("Connectivity"), quint32(4)},
                          {QStringLiteral("PrimaryConnection"), QVariant::fromValue(QDBusObjectPath(connectionPath))}});
    daemon.setProperties(connectionPath, QString::fromLatin1(ActiveConnectionInterface),
                         {{QStringLiteral("Id"), name},
                          {QStringLiteral("Type"), connectionType},
                          {QStringLiteral("Devices"),
                           QVariantList{QVariant::fromValue(QDBusObjectPath(devicePath))}}});
    daemon.setProperties(devicePath, QString::fromLatin1(DeviceInterface),
                         {{QStringLiteral("Interface"), QStringLiteral("wlan0")},
                          {QStringLiteral("DeviceType"), deviceType},
                          {QStringLiteral("ActiveAccessPoint"),
                           QVariant::fromValue(QDBusObjectPath(accessPointPath))}});
    daemon.setProperties(devicePath, QString::fromLatin1(WirelessInterface),
                         {{QStringLiteral("ActiveAccessPoint"),
                           QVariant::fromValue(QDBusObjectPath(accessPointPath))}});
    daemon.setProperties(accessPointPath, QString::fromLatin1(AccessPointInterface),
                         {{QStringLiteral("Strength"), quint8(strength)}});
}

// Records what the shell logs, so a refusal can be read rather than only inferred. The shell's own handler is
// installed and removed around one scope, which is the same shape the other tests read a record with.
class RecordCapture {
public:
    RecordCapture() {
        previous_ = qInstallMessageHandler(handler);
    }
    ~RecordCapture() { qInstallMessageHandler(previous_); }

    struct Record {
        QtMsgType type;
        QString category;
        QString message;
    };
    const QList<Record>& records() const { return records_; }

private:
    static void handler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
        records_.append(Record{type, QString::fromUtf8(context.category ? context.category : ""), message});
    }
    // A message handler is process state and Qt takes no closure, so the sink is one per process — which is why
    // this class is used for one scope at a time and nothing else logs while it is up.
    static QList<Record> records_;
    QtMessageHandler previous_ = nullptr;
};

QList<RecordCapture::Record> RecordCapture::records_;

}  // namespace

class NetworkTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // The names, as declared and as the daemon spells them.
    void theNamesAreTheDaemonsOwnAndTheTypeNameIsTheOneQmlBinds();
    // The daemon's numbers, as tokens and as refusals.
    void aStateIsReadAsTheTokenTheWidgetSwitchesOn();
    void aConnectivityIsReadAndOneThisBuildDoesNotKnowIsRefused();
    void aDeviceIsReadFromTheDeviceAndTheConnectionThatCarriesIt();
    // One object's properties, as the bytes that arrived.
    void aPropertyIsReadThroughItsVariantAndAValueOfTheWrongTypeIsNotAValue();
    void aChangeThatOvertakesTheFirstReadOfAnObjectIsNotUndoneByTheReply();
    // The reading the chain adds up to.
    void theReadingIsTheWholeChainAndAnOfflineMachineIsAReadingToo();
    // The service, against a daemon on a bus of this test's own.
    void theReadingArrivesFromTheDaemonAndFollowsItsSignals();
    void theDaemonIsAskedOncePerObjectAndNotAgainWhileSignalsArrive();
    void aChainThatMovesIsReadAndTheObjectsItLeftAreNoLongerFollowed();
    void aDaemonThatLeavesWithdrawsTheReadingAndOneThatArrivesIsFollowed();
    void aReplyFromADaemonThatIsGoneIsNotApplied();
    void anObjectThatRefusesLeavesTheRestOfTheReadingStanding();
    void anUnreachableBusIsRefusedWithAReason();

private:
    // The reading, waited for: the walk from the manager to the access point is four asynchronous replies, so
    // every assertion about a reading that has been published is a wait rather than an assumption about how
    // many turns of the event loop four round trips take.
    bool waitForReading(const QString& expected);
    bool waitForState(const QString& expected);

    PrivateBus bus_;
    QDBusConnection connection_{QString()};
    std::unique_ptr<FakeNetworkManager> daemon_;
    std::unique_ptr<quantum::dbus::NetworkService> service_;
    QString busName_;
};

void NetworkTest::initTestCase()
{
    if (!bus_.start()) {
        QSKIP("dbus-daemon is not available, so the private bus this test's fake daemon needs could not be "
              "started. The module's reading of the bus is not covered by anything else.");
    }
    // A name of its own, so the connection this test holds is not the desktop's session bus by any accident and
    // `disconnectFromBus` at the end cannot take the session's connection with it.
    busName_ = QStringLiteral("qs-network-test");
    connection_ = QDBusConnection::connectToBus(bus_.address(), busName_);
    QVERIFY2(connection_.isConnected(), qPrintable(connection_.lastError().message()));
}

void NetworkTest::cleanupTestCase()
{
    QDBusConnection::disconnectFromBus(busName_);
}

void NetworkTest::init()
{
    // A fresh daemon and a fresh service per slot: the objects a slot registers belong to it, and a slot that
    // forgot to release the name cannot decide what the next one finds. Nothing here holds state between slots
    // for the same reason — the order check runs every one of them on its own and in a fresh process.
    daemon_ = std::make_unique<FakeNetworkManager>(connection_, nullptr);
    service_ = std::make_unique<quantum::dbus::NetworkService>();
}

void NetworkTest::cleanup()
{
    service_.reset();
    daemon_.reset();
    connection_.unregisterService(QString::fromLatin1(NetworkManagerService));
}

bool NetworkTest::waitForState(const QString& expected)
{
    return QTest::qWaitFor([this, &expected] { return service_->state() == expected; }, 5000);
}

bool NetworkTest::waitForReading(const QString& expected)
{
    return QTest::qWaitFor([this, &expected] { return service_->available() && service_->state() == expected; },
                           5000);
}

// --- the names ---------------------------------------------------------------------------------------------

void NetworkTest::theNamesAreTheDaemonsOwnAndTheTypeNameIsTheOneQmlBinds()
{
    // Every one of these is a name that cannot fail a build by being wrong: a D-Bus name that does not match the
    // daemon's is a readout that never appears, which is the one failure a bar cannot report to itself. Each was
    // read off a running daemon with `busctl introspect --xml-interface`, and each is mirrored here at compile
    // time so that a rename in the module stops this build rather than reaching a screen as an empty readout.
    using namespace quantum::dbus;
    // `QLatin1StringView`'s own comparison is not a constant expression in this Qt, so the mirror compares the
    // characters the view points at — which is the same claim and still a compile-time one.
    constexpr auto text = [](QLatin1StringView view) {
        return std::string_view(view.data(), static_cast<std::size_t>(view.size()));
    };
    static_assert(text(NetworkManagerService) == "org.freedesktop.NetworkManager");
    static_assert(text(ManagerPath) == "/org/freedesktop/NetworkManager");
    static_assert(text(ManagerInterface) == "org.freedesktop.NetworkManager");
    static_assert(text(DeviceInterface) == "org.freedesktop.NetworkManager.Device");
    static_assert(text(WirelessInterface) == "org.freedesktop.NetworkManager.Device.Wireless");
    static_assert(text(AccessPointInterface) == "org.freedesktop.NetworkManager.AccessPoint");
    static_assert(text(ActiveConnectionInterface) == "org.freedesktop.NetworkManager.Connection.Active");
    static_assert(text(PropertiesInterface) == "org.freedesktop.DBus.Properties");
    static_assert(text(PropertiesChangedSignal) == "PropertiesChanged");
    // The property names, as the daemon's introspection spells them. It is these that a typo would turn into an
    // absent property, since `GetAll` answers with whatever names it likes and a reader asking for another one
    // reads nothing.
    static_assert(text(PropertyState) == "State");
    static_assert(text(PropertyConnectivity) == "Connectivity");
    static_assert(text(PropertyPrimaryConnection) == "PrimaryConnection");
    static_assert(text(PropertyId) == "Id");
    static_assert(text(PropertyType) == "Type");
    static_assert(text(PropertyDeviceInterface) == "Interface");
    static_assert(text(PropertyDeviceType) == "DeviceType");
    static_assert(text(PropertyActiveConnection) == "ActiveConnection");
    static_assert(text(PropertyActiveAccessPoint) == "ActiveAccessPoint");
    static_assert(text(PropertyStrength) == "Strength");
    static_assert(text(PropertyDevices) == "Devices");

    // The token a QML file switches on, which is interface of the same kind: a rename here without a matching
    // edit to the widget is a readout that falls through to its default branch on screen.
    static_assert(std::string_view(quantum::dbus::NetworkService::QmlTypeName) == "NetworkService");
}

// --- the daemon's numbers ----------------------------------------------------------------------------------

void NetworkTest::aStateIsReadAsTheTokenTheWidgetSwitchesOn()
{
    using namespace quantum::dbus;
    // `NM_STATE_*` from the installed `nm-dbus-interface.h`, and the tokens the widget branches on. The four
    // values that mean nothing is being carried are one token because a bar draws them the same way; the three
    // where a link exists and routing may not are one token that is not `connected`, which is the distinction a
    // bar exists to make.
    const std::array<std::pair<quint32, const char*>, 5> cases{{
        {10, "disconnected"},   // asleep
        {20, "disconnected"},   // disconnected
        {30, "disconnected"},   // disconnecting
        {70, "connected"},      // connected (global), which is what `nmcli general` calls connected
        {40, "connecting"},     // connecting
    }};
    for (const auto& [value, expected] : cases) {
        const auto state = connectionStateFromNmState(value);
        QVERIFY2(state.has_value(), qPrintable(QStringLiteral("state %1 was refused").arg(value)));
        QCOMPARE(connectionStateToken(*state), QString::fromLatin1(expected));
    }
    // The two values between the link coming up and the network working: connected (local only) and connected
    // (site only). Both are `connecting`, which says what is true — there is not a route to everything yet —
    // rather than the `connected` a bar would rather show.
    for (const quint32 value : {50u, 60u}) {
        QCOMPARE(connectionStateToken(*connectionStateFromNmState(value)), QStringLiteral("connecting"));
    }
    // A value this build does not know is refused rather than rounded into a state: the numbers are the daemon's
    // and a newer daemon may mean something else by one this build has never heard of.
    QVERIFY(!connectionStateFromNmState(0).has_value());
    QVERIFY(!connectionStateFromNmState(80).has_value());
    QVERIFY(!connectionStateFromNmState(4294967295u).has_value());
}

void NetworkTest::aConnectivityIsReadAndOneThisBuildDoesNotKnowIsRefused()
{
    using namespace quantum::dbus;
    QCOMPARE(connectivityToken(*connectivityFromNmValue(1)), QStringLiteral("none"));
    QCOMPARE(connectivityToken(*connectivityFromNmValue(2)), QStringLiteral("portal"));
    QCOMPARE(connectivityToken(*connectivityFromNmValue(3)), QStringLiteral("limited"));
    QCOMPARE(connectivityToken(*connectivityFromNmValue(4)), QStringLiteral("full"));
    // Zero is `NM_CONNECTIVITY_UNKNOWN` in NetworkManager's own header and is deliberately not a token: a daemon
    // saying "I do not know" is not a daemon saying "there is no connectivity".
    QVERIFY(!connectivityFromNmValue(0).has_value());
    QVERIFY(!connectivityFromNmValue(5).has_value());
}

void NetworkTest::aDeviceIsReadFromTheDeviceAndTheConnectionThatCarriesIt()
{
    using namespace quantum::dbus;
    // `NM_DEVICE_TYPE_*`: 1 is ethernet, 2 is wifi, and everything else — a loopback, a bridge, a p2p device —
    // is a connection with no quality to draw.
    QCOMPARE(deviceKindFromDeviceType(2), DeviceKind::Wifi);
    QCOMPARE(deviceKindFromDeviceType(1), DeviceKind::Ethernet);
    QCOMPARE(deviceKindFromDeviceType(0), DeviceKind::Other);
    QCOMPARE(deviceKindFromDeviceType(32), DeviceKind::Other);

    // The connection `Type` strings, which are the settings keys NetworkManager writes its own profiles with.
    QCOMPARE(deviceKindFromConnectionType(QStringLiteral("802-11-wireless")), DeviceKind::Wifi);
    QCOMPARE(deviceKindFromConnectionType(QStringLiteral("802-3-ethernet")), DeviceKind::Ethernet);
    QCOMPARE(deviceKindFromConnectionType(QStringLiteral("vpn")), DeviceKind::Other);
    QCOMPARE(deviceKindFromConnectionType(QString()), DeviceKind::Other);

    // The two spellings agreeing, which is the ordinary case.
    QCOMPARE(deviceKindFor(2, QStringLiteral("802-11-wireless")), DeviceKind::Wifi);
    QCOMPARE(deviceKindFor(1, QStringLiteral("802-3-ethernet")), DeviceKind::Ethernet);
    // And disagreeing, which is the case a vpn profile over wifi produces. The device wins, and that is a
    // decision rather than an accident: the strength this readout draws lives on the device's own access point,
    // so a reading that took the connection's word for it would ask a wired device for a signal it cannot have.
    QCOMPARE(deviceKindFor(2, QStringLiteral("802-3-ethernet")), DeviceKind::Wifi);
    QCOMPARE(deviceKindFor(1, QStringLiteral("802-11-wireless")), DeviceKind::Ethernet);
    // With no device type at all — a device the daemon has published before saying what it is — the connection
    // is what there is, and `Other` is the answer when neither recognises anything.
    QCOMPARE(deviceKindFor(0, QStringLiteral("802-11-wireless")), DeviceKind::Wifi);
    QCOMPARE(deviceKindFor(0, QStringLiteral("vpn")), DeviceKind::Other);
}

// --- one object's properties ------------------------------------------------------------------------------

void NetworkTest::aPropertyIsReadThroughItsVariantAndAValueOfTheWrongTypeIsNotAValue()
{
    using namespace quantum::dbus;
    // Both routes a property arrives by wrap it in a `v`: the `a{sv}` of a `GetAll` reply and the `a{sv}` of a
    // `PropertiesChanged` payload. A reader that did not unwrap found no converter and read nothing — the failure
    // that looks exactly like a daemon that never sent the property, which is why both wrappings are here.
    const ObjectProperties wrapped(QVariantMap{
        {QStringLiteral("State"), QVariant::fromValue(QDBusVariant(quint32(70)))},
        {QStringLiteral("Id"), QVariant::fromValue(QDBusVariant(QStringLiteral("wlan0")))},
        {QStringLiteral("PrimaryConnection"),
         QVariant::fromValue(QDBusVariant(QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kConnectionPath)))))},
        {QStringLiteral("Strength"), QVariant::fromValue(QDBusVariant(quint8(74)))},
    });
    QCOMPARE(wrapped.uint32Value(PropertyState), std::optional<quint32>(70));
    QCOMPARE(wrapped.textValue(PropertyId), std::optional<QString>(QStringLiteral("wlan0")));
    QCOMPARE(wrapped.pathValue(PropertyPrimaryConnection), std::optional<QString>(QString::fromLatin1(kConnectionPath)));
    QCOMPARE(wrapped.byteValue(PropertyStrength), std::optional<int>(74));

    // And the same values bare, which is how a hand-built map — a test's, or a daemon that thinks in strings —
    // arrives. A reading is what the map says, not what wrapped it.
    const ObjectProperties bare(QVariantMap{
        {QStringLiteral("State"), quint32(70)},
        {QStringLiteral("Id"), QStringLiteral("wlan0")},
        {QStringLiteral("PrimaryConnection"), QString::fromLatin1(kConnectionPath)},
        {QStringLiteral("Strength"), quint8(74)},
    });
    QCOMPARE(bare.uint32Value(PropertyState), std::optional<quint32>(70));
    QCOMPARE(bare.textValue(PropertyId), std::optional<QString>(QStringLiteral("wlan0")));
    QCOMPARE(bare.pathValue(PropertyPrimaryConnection), std::optional<QString>(QString::fromLatin1(kConnectionPath)));
    QCOMPARE(bare.byteValue(PropertyStrength), std::optional<int>(74));

    // A value of the wrong type is not a value. A state that arrived as text is not a state, and reading it as
    // one — by converting, or by taking the first thing that looked numeric — would put a made-up state on the
    // bar. `Strength` gets two extra refusals of its own: it is a byte on the wire, so anything above 255 is a
    // reader that has read the wrong property, and nothing honest comes of clamping it.
    const ObjectProperties wrong(QVariantMap{
        {QStringLiteral("State"), QStringLiteral("70")},
        {QStringLiteral("Connectivity"), QVariant::fromValue(QDBusVariant(QStringLiteral("full")))},
        {QStringLiteral("Strength"), QStringLiteral("74")},
        {QStringLiteral("DeviceType"), -2},
        {QStringLiteral("Id"), quint32(7)},
    });
    QVERIFY(!wrong.uint32Value(PropertyState).has_value());
    QVERIFY(!wrong.uint32Value(PropertyConnectivity).has_value());
    QVERIFY(!wrong.byteValue(PropertyStrength).has_value());
    // The byte reader's own bound, which is the width of the thing it reads: a value a byte cannot hold is not
    // this property whatever it means, and the reading's own clamp — `Strength` is documented as 0-100 — is a
    // separate rule that lives where the reading is composed.
    const ObjectProperties tooWide(QVariantMap{{QStringLiteral("Strength"), quint32(256)}});
    QVERIFY(!tooWide.byteValue(PropertyStrength).has_value());
    QVERIFY(!wrong.uint32Value(PropertyDeviceType).has_value());
    // Text is read only where text is what is expected, and a number in a text property is not text: the
    // property is the connection's name, and `7` is not one.
    QVERIFY(!wrong.textValue(PropertyId).has_value());
    // An absent property is absent rather than a default, whatever the default would have been.
    QVERIFY(!wrong.uint32Value(PropertyDevices).has_value());

    // An array of object paths, nested one variant deep per element, which is how `ao` arrives.
    const ObjectProperties list(QVariantMap{
        {QStringLiteral("Devices"),
         QVariantList{QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kDevicePath))),
                      QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kOtherDevicePath)))}},
    });
    QCOMPARE(list.pathList(PropertyDevices),
             QStringList({QString::fromLatin1(kDevicePath), QString::fromLatin1(kOtherDevicePath)}));
    // And what is not such an array is no devices at all: a request that has no answer is not a reason to invent
    // one, and a device list of one wrong element would send the walk to an object that does not exist.
    const ObjectProperties notAList(QVariantMap{{QStringLiteral("Devices"), QString::fromLatin1(kDevicePath)}});
    QVERIFY(notAList.pathList(PropertyDevices).isEmpty());
}

void NetworkTest::aChangeThatOvertakesTheFirstReadOfAnObjectIsNotUndoneByTheReply()
{
    using namespace quantum::dbus;
    // The race this class exists for, as a pure function of two payloads: the service subscribes to an object and
    // asks it for everything in the same breath, so a change can arrive while the first reply is still in flight
    // — and the reply, being older than the change, must not overwrite it. On a busy radio this is the common
    // case rather than a rare one: a device's first signal reading and its first change are milliseconds apart.
    ObjectProperties properties;
    properties.beginRead();
    QVERIFY(properties.isReading());
    properties.applyChange({{QStringLiteral("Strength"), quint8(11)}}, {});
    // While the read is in flight the value is held, so nothing reads a half-filled object as a reading.
    QVERIFY(!properties.uint32Value(PropertyState).has_value());
    properties.adoptAll({{QStringLiteral("Strength"), quint8(74)}, {QStringLiteral("State"), quint32(70)}});
    QVERIFY(!properties.isReading());
    QCOMPARE(properties.byteValue(PropertyStrength), std::optional<int>(11));  // the change won, not the reply
    QCOMPARE(properties.uint32Value(PropertyState), std::optional<quint32>(70));  // and the reply filled the rest

    // Two changes held, replayed in arrival order: the later one is the newer fact, so replaying them backwards
    // would end on the value the daemon had already replaced — the same mistake as assigning the reply over them.
    ObjectProperties ordered;
    ordered.beginRead();
    ordered.applyChange({{QStringLiteral("Strength"), quint8(20)}}, {});
    ordered.applyChange({{QStringLiteral("Strength"), quint8(35)}}, {});
    ordered.adoptAll({{QStringLiteral("Strength"), quint8(9)}});
    QCOMPARE(ordered.byteValue(PropertyStrength), std::optional<int>(35));

    // An invalidation held the same way, and it beats the reply: a property the daemon withdrew while the read
    // was in flight is gone, even though the reply that was already on its way still carries it.
    ObjectProperties invalidated;
    invalidated.beginRead();
    invalidated.applyChange({}, {QStringLiteral("PrimaryConnection")});
    invalidated.adoptAll({{QStringLiteral("PrimaryConnection"),
                           QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kConnectionPath)))}});
    QVERIFY(!invalidated.pathValue(PropertyPrimaryConnection).has_value());

    // With no read in flight, a change is merged at once and nothing is held: the ordinary path, where a signal
    // is the newest thing the object knows.
    ObjectProperties settled;
    settled.adoptAll({{QStringLiteral("Strength"), quint8(74)}});
    settled.applyChange({{QStringLiteral("Strength"), quint8(3)}}, {});
    QCOMPARE(settled.byteValue(PropertyStrength), std::optional<int>(3));

    // And `clear` forgets both kinds of state: what a daemon that left the bus said is not part of what the
    // daemon that replaced it says.
    ObjectProperties cleared;
    cleared.beginRead();
    cleared.applyChange({{QStringLiteral("Strength"), quint8(50)}}, {});
    cleared.clear();
    QVERIFY(cleared.isEmpty());
    QVERIFY(!cleared.isReading());
    cleared.adoptAll({});
    QVERIFY(!cleared.byteValue(PropertyStrength).has_value());
}

// --- the reading -------------------------------------------------------------------------------------------

void NetworkTest::theReadingIsTheWholeChainAndAnOfflineMachineIsAReadingToo()
{
    using namespace quantum::dbus;
    const ObjectProperties manager(QVariantMap{
        {PropertyState.toString(), quint32(70)},
        {PropertyConnectivity.toString(), quint32(4)},
        {PropertyPrimaryConnection.toString(),
         QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kConnectionPath)))},
    });
    const ObjectProperties connection(QVariantMap{
        {PropertyId.toString(), QStringLiteral("Victor Salin 5 GHz")},
        {PropertyType.toString(), QStringLiteral("802-11-wireless")},
        {PropertyDevices.toString(),
         QVariantList{QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kDevicePath)))}},
    });
    const ObjectProperties device(QVariantMap{
        {PropertyDeviceInterface.toString(), QStringLiteral("wlan0")},
        {PropertyDeviceType.toString(), quint32(2)},
    });
    const ObjectProperties accessPoint(QVariantMap{{PropertyStrength.toString(), quint8(74)}});

    const NetworkReading wifi = composeReading(manager, connection, device, accessPoint);
    QCOMPARE(wifi.state, ConnectionState::Connected);
    QCOMPARE(wifi.name, QStringLiteral("Victor Salin 5 GHz"));
    QCOMPARE(wifi.interfaceName, QStringLiteral("wlan0"));
    QCOMPARE(wifi.kind, DeviceKind::Wifi);
    QCOMPARE(wifi.strength, std::optional<int>(74));
    QCOMPARE(wifi.connectivity, std::optional<Connectivity>(Connectivity::Full));

    // A wired connection has no access point and therefore no strength: absent rather than zero, because zero is
    // a reading — a wireless device at the edge of range — and the widget draws the two differently.
    const ObjectProperties wiredDevice(QVariantMap{
        {PropertyDeviceInterface.toString(), QStringLiteral("enp39s0")},
        {PropertyDeviceType.toString(), quint32(1)},
    });
    const NetworkReading wired = composeReading(
        manager,
        ObjectProperties(QVariantMap{{PropertyId.toString(), QStringLiteral("Wired connection 1")},
                                     {PropertyType.toString(), QStringLiteral("802-3-ethernet")}}),
        wiredDevice, ObjectProperties());
    QCOMPARE(wired.kind, DeviceKind::Ethernet);
    QVERIFY(!wired.strength.has_value());

    // A wireless device that is not associated: the daemon names no active access point, so there is no
    // strength — which is a different fact from a strength of zero and is drawn differently.
    const NetworkReading unassociated = composeReading(manager, connection, device, ObjectProperties());
    QCOMPARE(unassociated.kind, DeviceKind::Wifi);
    QVERIFY(!unassociated.strength.has_value());

    // A strength outside what the daemon documents is clamped into the range rather than drawn as it arrived,
    // because a readout of 200% is a readout nothing can be done with.
    const NetworkReading loud = composeReading(manager, connection, device,
                                              ObjectProperties(QVariantMap{{PropertyStrength.toString(), quint8(255)}}));
    QCOMPARE(loud.strength, std::optional<int>(100));

    // A machine with no primary connection is a complete reading, not a missing one: the state is the whole of
    // it, and the name, interface, kind and strength stay absent rather than becoming an empty string a widget
    // would draw. `offline` is what the bar shows, which is what the daemon said.
    const NetworkReading offline = composeReading(
        ObjectProperties(QVariantMap{{PropertyState.toString(), quint32(20)},
                                     {PropertyConnectivity.toString(), quint32(1)},
                                     {PropertyPrimaryConnection.toString(), QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")))}}),
        connection, device, accessPoint);
    QCOMPARE(offline.state, ConnectionState::Disconnected);
    QVERIFY(offline.name.isEmpty());
    QVERIFY(offline.interfaceName.isEmpty());
    QVERIFY(!offline.strength.has_value());

    // The state before the daemon has said anything about connectivity: absent, which is not `full`. A bar draws
    // the same nothing for both, but the value here is what a future readout would need to tell them apart.
    const NetworkReading noVerdict = composeReading(
        ObjectProperties(QVariantMap{{PropertyState.toString(), quint32(70)},
                                     {PropertyPrimaryConnection.toString(),
                                      QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kConnectionPath)))}}),
        connection, device, accessPoint);
    QVERIFY(!noVerdict.connectivity.has_value());
    QCOMPARE(noVerdict.strength, std::optional<int>(74));

    // A state this build does not know, arriving as the first reading of a newer daemon: the reading claims the
    // least it can rather than claiming a connection nothing told it about. (The *service* keeps the last reading
    // it had when a property it cannot read arrives, so this path is only reached at the start.)
    const NetworkReading unknown = composeReading(
        ObjectProperties(QVariantMap{{PropertyState.toString(), quint32(99)}}), ObjectProperties(),
        ObjectProperties(), ObjectProperties());
    QCOMPARE(unknown.state, ConnectionState::Disconnected);
}

// --- the service -------------------------------------------------------------------------------------------

void NetworkTest::theReadingArrivesFromTheDaemonAndFollowsItsSignals()
{
    publishChain(*daemon_);
    // The second chain, described before it is used: in the ordinary case a daemon names a connection that is
    // already there, so the move this slot follows is a daemon moving between two objects it has both described.
    daemon_->setProperties(QString::fromLatin1(kOtherConnectionPath), QString::fromLatin1(ActiveConnectionInterface),
                           {{QStringLiteral("Id"), QStringLiteral("Ethernet in the wall")},
                            {QStringLiteral("Type"), QStringLiteral("802-3-ethernet")},
                            {QStringLiteral("Devices"),
                             QVariantList{QVariant::fromValue(
                                 QDBusObjectPath(QString::fromLatin1(kOtherDevicePath)))}}});
    daemon_->setProperties(QString::fromLatin1(kOtherDevicePath), QString::fromLatin1(DeviceInterface),
                           {{QStringLiteral("Interface"), QStringLiteral("enp39s0")},
                            {QStringLiteral("DeviceType"), quint32(1)},
                            {QStringLiteral("ActiveAccessPoint"),
                             QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")))}});
    QVERIFY(daemon_->own());

    QSignalSpy readings(service_.get(), &quantum::dbus::NetworkService::readingChanged);
    service_->start(connection_, QString::fromLatin1(NetworkManagerService));

    QVERIFY(waitForReading(QStringLiteral("connected")));
    QCOMPARE(service_->connectionName(), QStringLiteral("Victor Salin 5 GHz"));
    QCOMPARE(service_->interfaceName(), QStringLiteral("wlan0"));
    QCOMPARE(service_->deviceKind(), QStringLiteral("wifi"));
    QVERIFY(service_->hasStrength());
    QCOMPARE(service_->strength(), 74);
    QCOMPARE(service_->connectivity(), QStringLiteral("full"));

    // The signal strength changes, announced the way the daemon announces it — a `PropertiesChanged` on the
    // access point carrying only what moved. The reading follows without a request having been made for it.
    daemon_->announce(QString::fromLatin1(kAccessPointPath), QString::fromLatin1(AccessPointInterface),
                      {{QStringLiteral("Strength"), quint8(31)}});
    QVERIFY(QTest::qWaitFor([this] { return service_->strength() == 31; }, 5000));
    QVERIFY(readings.count() >= 2);

    // The daemon moving to another connection, announced on the manager with only `PrimaryConnection` in the
    // payload. The name and the interface come from objects the shell has never read, so this is the case where
    // it does ask — once each, which is counted in the slot below — and the ask is driven by the signal rather
    // than by anything on a schedule.
    daemon_->announce(QString::fromLatin1(ManagerPath), QString::fromLatin1(ManagerInterface),
                      {{QStringLiteral("PrimaryConnection"),
                        QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kOtherConnectionPath)))}});
    QVERIFY(QTest::qWaitFor([this] { return service_->connectionName() == QStringLiteral("Ethernet in the wall"); },
                            5000));
    QCOMPARE(service_->deviceKind(), QStringLiteral("ethernet"));
    QCOMPARE(service_->interfaceName(), QStringLiteral("enp39s0"));
    QVERIFY(!service_->hasStrength());

    // The connection drops: the manager names no primary connection, which is the daemon saying "nothing is
    // connected" rather than saying nothing at all. The reading is `disconnected` — a reading — and the name and
    // the interface are gone rather than stale.
    daemon_->announce(QString::fromLatin1(ManagerPath), QString::fromLatin1(ManagerInterface),
                      {{QStringLiteral("PrimaryConnection"), QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")))},
                       {QStringLiteral("State"), quint32(20)},
                       {QStringLiteral("Connectivity"), quint32(1)}});
    QVERIFY(waitForState(QStringLiteral("disconnected")));
    QVERIFY(service_->available());
    QVERIFY(service_->connectionName().isEmpty());
    QVERIFY(service_->interfaceName().isEmpty());
    QVERIFY(!service_->hasStrength());
    QVERIFY(readings.count() >= 4);
}

void NetworkTest::theDaemonIsAskedOncePerObjectAndNotAgainWhileSignalsArrive()
{
    publishChain(*daemon_);
    QVERIFY(daemon_->own());
    service_->start(connection_, QString::fromLatin1(NetworkManagerService));
    QVERIFY(waitForReading(QStringLiteral("connected")));

    // One `GetAll` per object of the chain, and there are four of them: the manager, the active connection, the
    // device, and the access point. (The wireless interface is read through the device's own path, which is the
    // fifth call and the second on that path.) This is the count that says the chain is walked once rather than
    // re-read whenever something changes.
    const int atFirstReading = daemon_->calls();
    QCOMPARE(daemon_->callsFor(QString::fromLatin1(ManagerPath)), 1);
    QCOMPARE(daemon_->callsFor(QString::fromLatin1(kConnectionPath)), 1);
    QCOMPARE(daemon_->callsFor(QString::fromLatin1(kAccessPointPath)), 1);
    // Two on the device path: the device's own properties and its wireless half, which is a second interface on
    // the same object rather than a second object.
    QCOMPARE(daemon_->callsFor(QString::fromLatin1(kDevicePath)), 2);
    QVERIFY(atFirstReading >= 4);

    // Now the daemon changes everything it can change without moving the chain: the strength, the connection's
    // name, the connectivity, and the state. Not one of them costs a call, because every one of them arrives as a
    // signal carrying the new value. This is the module's whole claim — it is driven by events and does not ask —
    // and it is a count rather than a description.
    daemon_->announce(QString::fromLatin1(kAccessPointPath), QString::fromLatin1(AccessPointInterface),
                      {{QStringLiteral("Strength"), quint8(52)}});
    daemon_->announce(QString::fromLatin1(kConnectionPath), QString::fromLatin1(ActiveConnectionInterface),
                      {{QStringLiteral("Id"), QStringLiteral("Victor Salin 5 GHz")}});
    daemon_->announce(QString::fromLatin1(ManagerPath), QString::fromLatin1(ManagerInterface),
                      {{QStringLiteral("Connectivity"), quint32(2)}, {QStringLiteral("State"), quint32(70)}});
    QVERIFY(QTest::qWaitFor([this] { return service_->connectivity() == QStringLiteral("portal"); }, 5000));
    QCOMPARE(service_->strength(), 52);
    QCOMPARE(daemon_->calls(), atFirstReading);

    // A change to a property this readout does not draw costs no repaint either: `Metered` is a real NetworkManager
    // property and nothing here shows it, so the reading is recomposed, compared, and found equal.
    QSignalSpy readings(service_.get(), &quantum::dbus::NetworkService::readingChanged);
    daemon_->announce(QString::fromLatin1(ManagerPath), QString::fromLatin1(ManagerInterface),
                      {{QStringLiteral("Metered"), quint32(2)}});
    QTest::qWait(100);
    QCOMPARE(readings.count(), 0);
}

void NetworkTest::aChainThatMovesIsReadAndTheObjectsItLeftAreNoLongerFollowed()
{
    publishChain(*daemon_);
    // The second chain, described before it is used so that the move is the only thing that changes.
    daemon_->setProperties(QString::fromLatin1(kOtherConnectionPath), QString::fromLatin1(ActiveConnectionInterface),
                           {{QStringLiteral("Id"), QStringLiteral("Other network")},
                            {QStringLiteral("Type"), QStringLiteral("802-11-wireless")},
                            {QStringLiteral("Devices"),
                             QVariantList{QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kOtherDevicePath)))}}});
    daemon_->setProperties(QString::fromLatin1(kOtherDevicePath), QString::fromLatin1(DeviceInterface),
                           {{QStringLiteral("Interface"), QStringLiteral("wlan1")},
                            {QStringLiteral("DeviceType"), quint32(2)},
                            {QStringLiteral("ActiveAccessPoint"),
                             QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kOtherAccessPointPath)))}});
    daemon_->setProperties(QString::fromLatin1(kOtherDevicePath), QString::fromLatin1(WirelessInterface),
                           {{QStringLiteral("ActiveAccessPoint"),
                             QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kOtherAccessPointPath)))}});
    daemon_->setProperties(QString::fromLatin1(kOtherAccessPointPath), QString::fromLatin1(AccessPointInterface),
                           {{QStringLiteral("Strength"), quint8(88)}});
    QVERIFY(daemon_->own());

    service_->start(connection_, QString::fromLatin1(NetworkManagerService));
    QVERIFY(waitForReading(QStringLiteral("connected")));
    QCOMPARE(service_->connectionName(), QStringLiteral("Victor Salin 5 GHz"));

    daemon_->announce(QString::fromLatin1(ManagerPath), QString::fromLatin1(ManagerInterface),
                      {{QStringLiteral("PrimaryConnection"),
                        QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kOtherConnectionPath)))}});
    QVERIFY(QTest::qWaitFor([this] { return service_->interfaceName() == QStringLiteral("wlan1"); }, 5000));
    QCOMPARE(service_->strength(), 88);

    // The rule for the access point the machine left is gone, and this is what proves it: a change announced on
    // the *old* path must not reach the reading. A rule left installed would merge it into the object that
    // replaced it — the reading would follow a network the machine is no longer on, which is the quietest
    // possible way for a bar to be wrong.
    QSignalSpy readings(service_.get(), &quantum::dbus::NetworkService::readingChanged);
    daemon_->announce(QString::fromLatin1(kAccessPointPath), QString::fromLatin1(AccessPointInterface),
                      {{QStringLiteral("Strength"), quint8(7)}});
    QTest::qWait(100);
    QCOMPARE(readings.count(), 0);
    QCOMPARE(service_->strength(), 88);

    // And a change on the *device* the machine left, the same way: two interfaces were subscribed on that path.
    daemon_->announce(QString::fromLatin1(kDevicePath), QString::fromLatin1(DeviceInterface),
                      {{QStringLiteral("Interface"), QStringLiteral("stale0")}});
    QTest::qWait(100);
    QCOMPARE(service_->interfaceName(), QStringLiteral("wlan1"));
    QCOMPARE(readings.count(), 0);
}

void NetworkTest::aDaemonThatLeavesWithdrawsTheReadingAndOneThatArrivesIsFollowed()
{
    publishChain(*daemon_);
    QVERIFY(daemon_->own());
    service_->start(connection_, QString::fromLatin1(NetworkManagerService));
    QVERIFY(waitForReading(QStringLiteral("connected")));

    // The daemon leaves the bus. The name going away is the bus's own announcement, and every client watching
    // the name is told — which is why this is a signal and not something the shell checks for.
    QSignalSpy readings(service_.get(), &quantum::dbus::NetworkService::readingChanged);
    daemon_->release();
    QVERIFY(QTest::qWaitFor([this] { return !service_->available(); }, 5000));
    QCOMPARE(readings.count(), 1);
    // The reading is withdrawn rather than left standing: a name, a strength and a state the shell can no longer
    // keep current are values that would be true only until the next time they changed.
    QVERIFY(service_->connectionName().isEmpty());
    QVERIFY(service_->interfaceName().isEmpty());
    QVERIFY(!service_->hasStrength());

    // The daemon comes back, and the shell finds it without anything asking: the watcher's registration is what
    // starts the walk, so a NetworkManager restarted under a running shell recovers on its own.
    QVERIFY(daemon_->own());
    QVERIFY(QTest::qWaitFor([this] { return service_->available(); }, 5000));
    QCOMPARE(service_->connectionName(), QStringLiteral("Victor Salin 5 GHz"));
    QCOMPARE(service_->strength(), 74);

    // And a stop at the end, which is the destructor's own path: nothing is left subscribed, so a change after it
    // reaches nothing rather than a service that believes it is still following the daemon.
    service_->stop();
    QVERIFY(!service_->available());
    QSignalSpy afterStop(service_.get(), &quantum::dbus::NetworkService::readingChanged);
    daemon_->announce(QString::fromLatin1(kAccessPointPath), QString::fromLatin1(AccessPointInterface),
                      {{QStringLiteral("Strength"), quint8(5)}});
    QTest::qWait(100);
    QCOMPARE(afterStop.count(), 0);
}

void NetworkTest::aReplyFromADaemonThatIsGoneIsNotApplied()
{
    publishChain(*daemon_);
    QVERIFY(daemon_->own());
    // Every reply the daemon sends takes 150 ms — an in-process round trip is under two milliseconds, so this
    // is two orders of magnitude of headroom rather than a race — and the walk is therefore certainly in flight
    // when the name goes away.
    daemon_->setDelayMs(150);

    QSignalSpy readings(service_.get(), &quantum::dbus::NetworkService::readingChanged);
    service_->start(connection_, QString::fromLatin1(NetworkManagerService));
    // The name leaves the bus while the first `GetAll` is still on its way. This is the shape of a daemon
    // restart, and of a stopping shell: a reply that arrives afterwards belongs to a daemon nobody is following.
    daemon_->release();
    QTest::qWait(600);

    QVERIFY2(!service_->available(), "a reply from a daemon that had gone was applied as a reading");
    QCOMPARE(readings.count(), 0);

    // And the service recovers: a new daemon on the name is followed, with the same delay in place, which is what
    // says the walk works at all rather than this slot passing because nothing ever arrives.
    QVERIFY(daemon_->own());
    QVERIFY(QTest::qWaitFor([this] { return service_->available(); }, 5000));
    QCOMPARE(service_->connectionName(), QStringLiteral("Victor Salin 5 GHz"));
}

void NetworkTest::anObjectThatRefusesLeavesTheRestOfTheReadingStanding()
{
    publishChain(*daemon_);
    QVERIFY(daemon_->own());
    service_->start(connection_, QString::fromLatin1(NetworkManagerService));
    QVERIFY(waitForReading(QStringLiteral("connected")));

    // The daemon moves to a connection it will not describe: the manager names it, and `GetAll` on it is an
    // error. What the shell has already read stands — the manager's own state — rather than the whole reading
    // being thrown away for one object that refused, and the refusal is a record rather than a silent nothing.
    RecordCapture capture;
    daemon_->announce(QString::fromLatin1(ManagerPath), QString::fromLatin1(ManagerInterface),
                      {{QStringLiteral("PrimaryConnection"),
                        QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/freedesktop/NetworkManager/ActiveConnection/404")))}});
    QTest::qWait(200);

    QVERIFY(service_->available());
    QCOMPARE(service_->state(), QStringLiteral("connected"));
    QVERIFY(service_->connectionName().isEmpty());
    QVERIFY(!service_->hasStrength());

    bool refused = false;
    for (const RecordCapture::Record& record : capture.records()) {
        if (record.category == quantum::app::networkLog().categoryName()
            && record.message.contains(QStringLiteral("ActiveConnection/404"))) {
            refused = true;
        }
    }
    QVERIFY2(refused, "an object the daemon refused to describe was not recorded");
}

void NetworkTest::anUnreachableBusIsRefusedWithAReason()
{
    // A connection that names no bus. `QDBusConnection` has no default constructor — every one of them names a
    // bus — and this is the value this module holds before `start()`: not connected, with every call on it
    // failing rather than reaching somewhere unintended. Starting on one has to be a refusal with a reason and
    // not a crash, because a machine with no system bus is a machine this shell still has to draw a bar on.
    RecordCapture capture;
    service_->start(QDBusConnection(QStringLiteral("qs-network-test-no-such-bus")),
                    QString::fromLatin1(NetworkManagerService));
    QTest::qWait(100);

    QVERIFY(!service_->available());
    QCOMPARE(service_->state(), QStringLiteral("disconnected"));
    bool refused = false;
    for (const RecordCapture::Record& record : capture.records()) {
        if (record.category == quantum::app::networkLog().categoryName()
            && record.message.contains(QStringLiteral("not connected"))) {
            refused = true;
        }
    }
    QVERIFY2(refused, "an unreachable bus was not recorded");
    // And every later call is refused the same way rather than reaching a connection that does not exist:
    // `refreshNow` on a service that never attached is a no-op, and the read path says why.
    service_->refreshNow();
    QTest::qWait(100);
    QVERIFY(!service_->available());
}

QTEST_MAIN(NetworkTest)
#include "network_test.moc"
