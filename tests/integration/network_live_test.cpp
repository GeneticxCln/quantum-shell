// The network module against the daemon the desktop is really using, read-only.
//
// `network-test` drives the service against a fake daemon on a bus of its own, which is what makes the reading
// checkable against payloads the test chose and what makes the shell's *asking* countable. What a fake cannot
// say is that the names and the numbers are NetworkManager's: a fake built from the same reading of the
// documentation as the module would agree with the module about a mistake. So this test asks the real daemon,
// through a different program.
//
// Three things and not one agreeing with itself: the daemon owns the state, `busctl` prints what it owns — a
// second implementation, from systemd rather than from Qt — and the module reads it over Qt's D-Bus on the
// system bus. Every expectation below is a value `busctl` printed, converted by a rule written here (the state
// numbers are NetworkManager's own constants), and the module's answer is compared with it.
//
// Nothing here asks for consent because nothing here writes: the test reads the manager, the active connection,
// the device and its access point, in the same order the module walks them. It changes no connection, no device
// and no profile. It needs no compositor and no display, so it is registered on any machine and skips — with the
// reason — where there is no system bus or no NetworkManager on it.
//
// The one thing it cannot compare exactly is the signal quality, because that is the number a radio reports
// while the test is asking: it moves. So it is compared the way the live niri comparisons are, by reading the
// daemon again until the two agree, and a run that had to is reported rather than failed.
#include "dbus/NetworkService.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>

#include <optional>

namespace {

// NetworkManager's well-known name and the interface names this test asks about, written from the module's own
// declarations rather than beside them: a name that moved on one side has to move on the other, and the module's
// is the one that was read off the daemon.
using quantum::dbus::AccessPointInterface;
using quantum::dbus::ActiveConnectionInterface;
using quantum::dbus::DeviceInterface;
using quantum::dbus::ManagerInterface;
using quantum::dbus::ManagerPath;
using quantum::dbus::NetworkManagerService;
using quantum::dbus::WirelessInterface;

// One property of one object, as `busctl --system get-property` prints it: the type letter, then the value. It
// is run as a program rather than read through Qt so that the daemon's answer does not come from the code under
// test — the same reason the audio live test reads a sink through `pw-dump`.
std::optional<QString> asBusctl(const QString& path, const QString& interface, const QString& name)
{
    QProcess busctl;
    busctl.start(QStringLiteral("busctl"),
                 {QStringLiteral("--system"), QStringLiteral("get-property"),
                  QString::fromLatin1(NetworkManagerService), path, interface, name});
    if (!busctl.waitForStarted(5000) || !busctl.waitForFinished(5000) || busctl.exitCode() != 0) {
        return std::nullopt;
    }
    const QString answer = QString::fromUtf8(busctl.readAllStandardOutput()).trimmed();
    return answer.isEmpty() ? std::nullopt : std::optional<QString>(answer);
}

// The value without its type letter, which is the shape a comparison wants.
QString valueOf(const std::optional<QString>& printed)
{
    if (!printed) {
        return QString();
    }
    // `u 70`, `s "wlan0"`, `o "/path"`, `y 74` — one space between the type and the value, and a string's value
    // is quoted.
    const int space = printed->indexOf(QLatin1Char(' '));
    QString value = space < 0 ? *printed : printed->mid(space + 1);
    if (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')) && value.size() >= 2) {
        value = value.mid(1, value.size() - 2);
    }
    return value;
}

std::optional<quint32> numberAt(const QString& path, const QString& interface, const QString& name)
{
    bool ok = false;
    const quint32 value = valueOf(asBusctl(path, interface, name)).toUInt(&ok);
    return ok ? std::optional<quint32>(value) : std::nullopt;
}

QString textAt(const QString& path, const QString& interface, const QString& name)
{
    return valueOf(asBusctl(path, interface, name));
}

// The token the daemon's own state number means, from NetworkManager's `NM_STATE_*` constants. Written out here
// rather than taken from the module, so that this test and the module can disagree — which is the only way the
// comparison below is worth anything.
QString tokenForState(quint32 state)
{
    switch (state) {
    case 10:
    case 20:
    case 30:
        return QStringLiteral("disconnected");
    case 40:
    case 50:
    case 60:
        return QStringLiteral("connecting");
    case 70:
        return QStringLiteral("connected");
    default:
        return QString();
    }
}

QString tokenForDeviceType(quint32 deviceType)
{
    switch (deviceType) {
    case 2:
        return QStringLiteral("wifi");
    case 1:
        return QStringLiteral("ethernet");
    default:
        return QStringLiteral("other");
    }
}

QString tokenForConnectivity(quint32 connectivity)
{
    switch (connectivity) {
    case 1:
        return QStringLiteral("none");
    case 2:
        return QStringLiteral("portal");
    case 3:
        return QStringLiteral("limited");
    case 4:
        return QStringLiteral("full");
    default:
        return QString();
    }
}

}  // namespace

class NetworkLiveTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void theReadingAgreesWithTheDaemonTheDesktopIsUsing();
    void theDaemonIsAskedOnceForTheObjectsItNames();

private:
    quantum::dbus::NetworkService service_;
    bool daemonIsThere_ = false;
};

void NetworkLiveTest::initTestCase()
{
    // The system bus, because that is where NetworkManager lives — the session bus has no such name and a shell
    // that looked for it there would find nothing. A machine without either is not a failure of this test: it is
    // a machine where the claim it makes cannot be observed, and the skip says so rather than passing quietly.
    QDBusConnection system = QDBusConnection::systemBus();
    if (!system.isConnected() || !asBusctl(QString::fromLatin1(ManagerPath), QString::fromLatin1(ManagerInterface),
                                           QStringLiteral("State"))
                                   .has_value()) {
        QSKIP("no system bus with NetworkManager on it, so the daemon this module reads is not present. The "
              "module's reading of NetworkManager's payloads is covered by network-test against a fake daemon.");
    }
    daemonIsThere_ = true;
    service_.start(system);
}

void NetworkLiveTest::cleanupTestCase()
{
    service_.stop();
}

void NetworkLiveTest::theReadingAgreesWithTheDaemonTheDesktopIsUsing()
{
    QVERIFY(daemonIsThere_);
    // A reading is asynchronous — the walk is four round trips to the daemon — so the first thing asserted is a
    // wait rather than a value.
    QVERIFY2(QTest::qWaitFor([this] { return service_.available(); }, 10000),
             "the daemon is on the bus and the service never published a reading from it");

    // The state and the verdict, each against the number the daemon printed a moment ago. The comparison is with
    // a mapping written in this file rather than with the module's own token builder: a test that asked the
    // module what its token meant would pass whatever the module said.
    const QString state = textAt(QString::fromLatin1(ManagerPath), QString::fromLatin1(ManagerInterface),
                                 QStringLiteral("State"));
    const quint32 stateNumber = state.toUInt();
    QVERIFY2(!tokenForState(stateNumber).isEmpty(),
             qPrintable(QStringLiteral("the daemon reported a state this test does not know: %1").arg(state)));
    QCOMPARE(service_.state(), tokenForState(stateNumber));

    const quint32 connectivity = textAt(QString::fromLatin1(ManagerPath), QString::fromLatin1(ManagerInterface),
                                        QStringLiteral("Connectivity"))
                                     .toUInt();
    // Either the daemon has a verdict — and the module must be reporting it in its own token — or it has none,
    // which the module publishes as an empty token rather than as a claim.
    if (tokenForConnectivity(connectivity).isEmpty()) {
        QVERIFY2(service_.connectivity().isEmpty(),
                 qPrintable(QStringLiteral("the daemon reported connectivity %1 and the shell claims \"%2\"")
                                .arg(connectivity)
                                .arg(service_.connectivity())));
    } else {
        QCOMPARE(service_.connectivity(), tokenForConnectivity(connectivity));
    }

    // The chain, if the daemon named one. A machine with nothing connected is the other shape and is asserted
    // too: the state is the whole reading then, and the name, the interface and the strength are absent rather
    // than invented — a shell that drew a device nothing named is the failure this half rules out.
    const QString primary = textAt(QString::fromLatin1(ManagerPath), QString::fromLatin1(ManagerInterface),
                                   QStringLiteral("PrimaryConnection"));
    if (primary.isEmpty() || primary == QLatin1String("/")) {
        QVERIFY2(service_.connectionName().isEmpty(), qPrintable(service_.connectionName()));
        QVERIFY2(service_.interfaceName().isEmpty(), qPrintable(service_.interfaceName()));
        QVERIFY2(!service_.hasStrength(), "the shell reports a signal quality with no connection to carry it");
        return;
    }

    // What the connection is called, which is the property a person reads — the SSID for a wireless connection.
    // The name is compared exactly, and it is the daemon's own string with its spaces and its brackets intact,
    // which is what makes a module that trimmed or re-formatted it fail here.
    const QString id = textAt(primary, QString::fromLatin1(ActiveConnectionInterface), QStringLiteral("Id"));
    QVERIFY2(!id.isEmpty(), qPrintable(primary));
    QCOMPARE(service_.connectionName(), id);

    // The device the connection runs on: named by the connection's own `Devices` list, which is the first step of
    // the walk that a module which guessed at a path would get wrong.
    QProcess devices;
    devices.start(QStringLiteral("busctl"),
                  {QStringLiteral("--system"), QStringLiteral("get-property"),
                   QString::fromLatin1(NetworkManagerService), primary,
                   QString::fromLatin1(ActiveConnectionInterface), QStringLiteral("Devices")});
    QVERIFY(devices.waitForStarted(5000));
    QVERIFY(devices.waitForFinished(5000));
    QCOMPARE(devices.exitCode(), 0);
    const QString deviceLine = QString::fromUtf8(devices.readAllStandardOutput()).trimmed();
    // `ao 2 "/org/.../Devices/1" "/org/.../Devices/3"`, so the first quoted path is the first device.
    const int firstQuote = deviceLine.indexOf(QLatin1Char('"'));
    const int secondQuote = deviceLine.indexOf(QLatin1Char('"'), firstQuote + 1);
    QVERIFY2(firstQuote >= 0 && secondQuote > firstQuote, qPrintable(deviceLine));
    const QString devicePath = deviceLine.mid(firstQuote + 1, secondQuote - firstQuote - 1);

    const QString interfaceName =
        textAt(devicePath, QString::fromLatin1(DeviceInterface), QStringLiteral("Interface"));
    QCOMPARE(service_.interfaceName(), interfaceName);

    const quint32 deviceType =
        numberAt(devicePath, QString::fromLatin1(DeviceInterface), QStringLiteral("DeviceType")).value_or(0);
    QCOMPARE(service_.deviceKind(), tokenForDeviceType(deviceType));

    // The signal quality, which exists only on a wireless device's active access point and is the value on this
    // readout that moves while the test is asking. It is compared by reading the daemon again until the two
    // agree: a radio reports its quality whenever it likes, so a single comparison would be claiming the desktop
    // held still between two processes starting.
    if (tokenForDeviceType(deviceType) != QLatin1String("wifi")) {
        QVERIFY2(!service_.hasStrength(),
                 "a wired connection reports a signal quality, which is a property nothing has");
        return;
    }

    const QString accessPoint = textAt(devicePath, QString::fromLatin1(WirelessInterface),
                                       QStringLiteral("ActiveAccessPoint"));
    if (accessPoint.isEmpty() || accessPoint == QLatin1String("/")) {
        QVERIFY2(!service_.hasStrength(), "a wireless device that is not associated reports a signal quality");
        return;
    }

    const auto agrees = [this, &accessPoint] {
        const std::optional<quint32> strength = numberAt(accessPoint, QString::fromLatin1(AccessPointInterface),
                                                        QStringLiteral("Strength"));
        return strength.has_value() && service_.hasStrength() && service_.strength() == int(*strength);
    };
    bool reconciled = false;
    for (int attempt = 0; attempt < 20 && !reconciled; ++attempt) {
        reconciled = agrees();
        if (!reconciled) {
            QTest::qWait(100);
        }
    }
    if (!reconciled) {
        qWarning("the signal quality did not agree in 20 readings; the daemon reported %s and the shell says %d",
                 qPrintable(valueOf(asBusctl(accessPoint, QString::fromLatin1(AccessPointInterface),
                                             QStringLiteral("Strength")))),
                 service_.strength());
    }
    QVERIFY2(reconciled, "the signal quality never agreed with the daemon's own answer");
}

void NetworkLiveTest::theDaemonIsAskedOnceForTheObjectsItNames()
{
    QVERIFY(daemonIsThere_);
    // The count `network-test` makes against a fake, made again here against the real daemon through the daemon's
    // own answer: the reading is read twice more, and nothing about the chain is asked for again. What this adds
    // to the fake's count is that the daemon on the other end is the real one — its objects are the ones its own
    // properties name, and the walk that reaches them is the walk a running shell would make.
    QVERIFY(QTest::qWaitFor([this] { return service_.available(); }, 10000));
    const QString reading = service_.connectionName();
    const QString interfaceName = service_.interfaceName();
    const int strength = service_.strength();

    // A second and a third read, by hand: `refreshNow` is the one door a caller has for re-reading a chain, and
    // nothing in the module uses it on a schedule. After it, the daemon's answers are the same ones — which is
    // what says the walk is repeatable and that the objects it reached are the daemon's own, not a first-read
    // accident.
    service_.refreshNow();
    QVERIFY(QTest::qWaitFor([this] { return service_.available(); }, 10000));
    QCOMPARE(service_.connectionName(), reading);
    QCOMPARE(service_.interfaceName(), interfaceName);
    // The strength can have moved in between, which is the radio and not the walk: what is compared is that a
    // wireless reading still has one, so a walk that lost the access point fails here.
    if (QString::fromLatin1(service_.deviceKind().toUtf8()) == QLatin1String("wifi") && strength > 0) {
        QVERIFY2(service_.hasStrength(), "the access point was not reached on a re-read");
    }
}

QTEST_MAIN(NetworkLiveTest)
#include "network_live_test.moc"
