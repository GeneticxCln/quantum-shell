// The battery readout's pure functions: UPower's D-Bus protocol as this shell reads it, with no daemon, no bus
// and no display.
//
// Everything here is a pure function of bytes — the property maps the daemon publishes on `PropertiesChanged` and
// answers to `GetAll` — so every shape a real daemon can send, and the shapes a healthy daemon never sends, is a
// case in this file with no system bus, no permissions and no session. What is left in the service is plumbing: a
// connection, two subscriptions and the re-resolution that follows the daemon's own device-set signals.
//
// What is deliberately *not* here: anything that needs a daemon. The subscription, the device-set signals, the
// daemon leaving and arriving, and the refresh path are `battery-live-test`'s, behind its own opt-in, because
// the only honest way to check them is against UPower — and on a bus this test started itself, so that it never
// reads or changes the battery state of whoever is running it.
#include "dbus/BatteryService.h"
#include "dbus/BatteryStatus.h"
#include "dbus/DbusProperties.h"

#include <QTest>

using quantum::dbus::BatteryReading;
using quantum::dbus::BatteryState;
using quantum::dbus::BatteryWarning;
using quantum::dbus::batteryStateFromUpower;
using quantum::dbus::batteryStateToken;
using quantum::dbus::batteryWarningFromUpower;
using quantum::dbus::batteryWarningToken;
using quantum::dbus::composeReading;
using quantum::dbus::durationFromSeconds;
using quantum::dbus::formatDuration;
using quantum::dbus::ObjectProperties;
using quantum::dbus::percentFromPercentage;
using quantum::dbus::UPowerDeviceInterface;
using quantum::dbus::UPowerManagerInterface;
using quantum::dbus::UPowerManagerPath;
using quantum::dbus::UPowerMethodGetDisplayDevice;
using quantum::dbus::UPowerPropertyIsPresent;
using quantum::dbus::UPowerPropertyOnBattery;
using quantum::dbus::UPowerPropertyPercentage;
using quantum::dbus::UPowerPropertyState;
using quantum::dbus::UPowerPropertyTimeToEmpty;
using quantum::dbus::UPowerPropertyTimeToFull;
using quantum::dbus::UPowerPropertyWarningLevel;
using quantum::dbus::UPowerService;
using quantum::dbus::UPowerSignalDeviceAdded;
using quantum::dbus::UPowerSignalDeviceRemoved;

namespace {

// The QML name this service is registered under, mirrored here rather than read out of the header, for the
// reason `sysmon_test.cpp` and `audio_test.cpp` mirror theirs: a rename in `src/dbus/` does not build until this
// file agrees, and `qml/Battery.qml` is the other place the name is written.
constexpr auto coveredServiceTypeName = "BatteryService";
static_assert(std::string_view(quantum::dbus::BatteryService::QmlTypeName) ==
                  std::string_view(coveredServiceTypeName),
              "the QML type name changed: update the mirror above, and every QML file that binds to it");

// The D-Bus names this module speaks, mirrored here for the reason the QML type name is above: they are declared
// once in `BatteryStatus.h` and checked here at compile time, so a wrong name fails a build rather than drawing
// nothing. The module's constants are the authority; these are the check.
constexpr std::string_view coveredService = "org.freedesktop.UPower";
constexpr std::string_view coveredManagerPath = "/org/freedesktop/UPower";
constexpr std::string_view coveredManagerInterface = "org.freedesktop.UPower";
constexpr std::string_view coveredDeviceInterface = "org.freedesktop.UPower.Device";
constexpr std::string_view coveredMethodGetDisplayDevice = "GetDisplayDevice";
constexpr std::string_view coveredSignalDeviceAdded = "DeviceAdded";
constexpr std::string_view coveredSignalDeviceRemoved = "DeviceRemoved";
constexpr std::string_view coveredPropertyOnBattery = "OnBattery";
constexpr std::string_view coveredPropertyIsPresent = "IsPresent";
constexpr std::string_view coveredPropertyPercentage = "Percentage";
constexpr std::string_view coveredPropertyState = "State";
constexpr std::string_view coveredPropertyTimeToEmpty = "TimeToEmpty";
constexpr std::string_view coveredPropertyTimeToFull = "TimeToFull";
constexpr std::string_view coveredPropertyWarningLevel = "WarningLevel";

// The state and warning tokens, mirrored here for the reason the D-Bus names are above: the widget switches on
// them, this module resolves them, and the agreement is checked at runtime in the test slots below. The token
// functions return QString for QML, so they cannot be checked at compile time.
constexpr std::string_view coveredStateUnknown = "unknown";
constexpr std::string_view coveredStateCharging = "charging";
constexpr std::string_view coveredStateDischarging = "discharging";
constexpr std::string_view coveredStateEmpty = "empty";
constexpr std::string_view coveredStateFull = "fully-charged";
constexpr std::string_view coveredStatePendingCharge = "pending-charge";
constexpr std::string_view coveredStatePendingDischarge = "pending-discharge";
constexpr std::string_view coveredWarningUnknown = "unknown";
constexpr std::string_view coveredWarningNone = "none";
constexpr std::string_view coveredWarningDischarging = "discharging";
constexpr std::string_view coveredWarningLow = "low";
constexpr std::string_view coveredWarningCritical = "critical";
constexpr std::string_view coveredWarningAction = "action";

}  // namespace

class BatteryTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void upowerStatesAreRecognised();
    void dbusNamesMatchCoveredValues();
    void unknownStateIsRefused();
    void stateTokensMatchCoveredValues();
    void upowerWarningLevelsAreRecognised();
    void unknownWarningLevelIsRefused();
    void warningTokensMatchCoveredValues();
    void percentageIsRounded();
    void percentageOutOfRangeIsRefused();
    void durationIsAccepted();
    void zeroDurationIsRefused();
    void negativeDurationIsRefused();
    void durationIsFormatted();
    void durationUnderOneMinuteIsDrawnAsOneMinute();
    void readingIsComposedFromBothObjects();
    void absentDeviceIsNotRead();
    void timeToEmptyIsUsedWhenDischarging();
    void timeToFullIsUsedWhenCharging();
    void firstNonZeroTimeIsUsedWhenStateIsUnknown();
};

// The names this module speaks, against the mirror above. A wrong D-Bus name cannot fail a build — it fails by
// drawing nothing, which is the one failure a readout cannot report to itself — so the mirror is compared here.
// It is a runtime slot rather than a `static_assert` because comparing `QLatin1StringView` with
// `std::string_view` is an ordinary function call and not a constant expression.
void BatteryTest::dbusNamesMatchCoveredValues()
{
    QCOMPARE(UPowerService, QLatin1StringView(coveredService.data(), qsizetype(coveredService.size())));
    QCOMPARE(UPowerManagerPath,
             QLatin1StringView(coveredManagerPath.data(), qsizetype(coveredManagerPath.size())));
    QCOMPARE(UPowerManagerInterface,
             QLatin1StringView(coveredManagerInterface.data(), qsizetype(coveredManagerInterface.size())));
    QCOMPARE(UPowerDeviceInterface,
             QLatin1StringView(coveredDeviceInterface.data(), qsizetype(coveredDeviceInterface.size())));
    QCOMPARE(UPowerMethodGetDisplayDevice,
             QLatin1StringView(coveredMethodGetDisplayDevice.data(),
                               qsizetype(coveredMethodGetDisplayDevice.size())));
    QCOMPARE(UPowerSignalDeviceAdded,
             QLatin1StringView(coveredSignalDeviceAdded.data(), qsizetype(coveredSignalDeviceAdded.size())));
    QCOMPARE(UPowerSignalDeviceRemoved,
             QLatin1StringView(coveredSignalDeviceRemoved.data(), qsizetype(coveredSignalDeviceRemoved.size())));
    QCOMPARE(UPowerPropertyOnBattery,
             QLatin1StringView(coveredPropertyOnBattery.data(), qsizetype(coveredPropertyOnBattery.size())));
    QCOMPARE(UPowerPropertyIsPresent,
             QLatin1StringView(coveredPropertyIsPresent.data(), qsizetype(coveredPropertyIsPresent.size())));
    QCOMPARE(UPowerPropertyPercentage,
             QLatin1StringView(coveredPropertyPercentage.data(), qsizetype(coveredPropertyPercentage.size())));
    QCOMPARE(UPowerPropertyState,
             QLatin1StringView(coveredPropertyState.data(), qsizetype(coveredPropertyState.size())));
    QCOMPARE(UPowerPropertyTimeToEmpty,
             QLatin1StringView(coveredPropertyTimeToEmpty.data(), qsizetype(coveredPropertyTimeToEmpty.size())));
    QCOMPARE(UPowerPropertyTimeToFull,
             QLatin1StringView(coveredPropertyTimeToFull.data(), qsizetype(coveredPropertyTimeToFull.size())));
    QCOMPARE(UPowerPropertyWarningLevel,
             QLatin1StringView(coveredPropertyWarningLevel.data(),
                               qsizetype(coveredPropertyWarningLevel.size())));
}

void BatteryTest::upowerStatesAreRecognised()
{
    // UPower's own numbers, from the installed up-types.h.
    QCOMPARE(batteryStateFromUpower(0), BatteryState::Unknown);
    QCOMPARE(batteryStateFromUpower(1), BatteryState::Charging);
    QCOMPARE(batteryStateFromUpower(2), BatteryState::Discharging);
    QCOMPARE(batteryStateFromUpower(3), BatteryState::Empty);
    QCOMPARE(batteryStateFromUpower(4), BatteryState::Full);
    QCOMPARE(batteryStateFromUpower(5), BatteryState::PendingCharge);
    QCOMPARE(batteryStateFromUpower(6), BatteryState::PendingDischarge);
}

void BatteryTest::unknownStateIsRefused()
{
    // A state this build has never heard of is refused rather than folded into `Unknown`, because `Unknown` is a
    // state UPower itself publishes: a daemon that has not decided and a daemon that has invented a seventh state
    // are different facts.
    QCOMPARE(batteryStateFromUpower(7), std::nullopt);
    QCOMPARE(batteryStateFromUpower(999), std::nullopt);
}

void BatteryTest::stateTokensMatchCoveredValues()
{
    QCOMPARE(batteryStateToken(BatteryState::Unknown), QString::fromStdString(std::string(coveredStateUnknown)));
    QCOMPARE(batteryStateToken(BatteryState::Charging), QString::fromStdString(std::string(coveredStateCharging)));
    QCOMPARE(batteryStateToken(BatteryState::Discharging),
             QString::fromStdString(std::string(coveredStateDischarging)));
    QCOMPARE(batteryStateToken(BatteryState::Empty), QString::fromStdString(std::string(coveredStateEmpty)));
    QCOMPARE(batteryStateToken(BatteryState::Full), QString::fromStdString(std::string(coveredStateFull)));
    QCOMPARE(batteryStateToken(BatteryState::PendingCharge),
             QString::fromStdString(std::string(coveredStatePendingCharge)));
    QCOMPARE(batteryStateToken(BatteryState::PendingDischarge),
             QString::fromStdString(std::string(coveredStatePendingDischarge)));
}

void BatteryTest::upowerWarningLevelsAreRecognised()
{
    // UPower's own numbers, from the installed up-types.h.
    QCOMPARE(batteryWarningFromUpower(0), BatteryWarning::Unknown);
    QCOMPARE(batteryWarningFromUpower(1), BatteryWarning::None);
    QCOMPARE(batteryWarningFromUpower(2), BatteryWarning::Discharging);
    QCOMPARE(batteryWarningFromUpower(3), BatteryWarning::Low);
    QCOMPARE(batteryWarningFromUpower(4), BatteryWarning::Critical);
    QCOMPARE(batteryWarningFromUpower(5), BatteryWarning::Action);
}

void BatteryTest::unknownWarningLevelIsRefused()
{
    // The values above `Action` belong to `BatteryLevel` and are not warning levels at all. A daemon that sent
    // `Normal` for a warning level would be telling us something this property cannot mean.
    QCOMPARE(batteryWarningFromUpower(6), std::nullopt);
    QCOMPARE(batteryWarningFromUpower(999), std::nullopt);
}

void BatteryTest::warningTokensMatchCoveredValues()
{
    QCOMPARE(batteryWarningToken(BatteryWarning::Unknown),
             QString::fromStdString(std::string(coveredWarningUnknown)));
    QCOMPARE(batteryWarningToken(BatteryWarning::None), QString::fromStdString(std::string(coveredWarningNone)));
    QCOMPARE(batteryWarningToken(BatteryWarning::Discharging),
             QString::fromStdString(std::string(coveredWarningDischarging)));
    QCOMPARE(batteryWarningToken(BatteryWarning::Low), QString::fromStdString(std::string(coveredWarningLow)));
    QCOMPARE(batteryWarningToken(BatteryWarning::Critical),
             QString::fromStdString(std::string(coveredWarningCritical)));
    QCOMPARE(batteryWarningToken(BatteryWarning::Action),
             QString::fromStdString(std::string(coveredWarningAction)));
}

void BatteryTest::percentageIsRounded()
{
    // The nearest whole point: `std::lround` rather than a cast. Both ends of the range are exact.
    QCOMPARE(percentFromPercentage(0.0), 0);
    QCOMPARE(percentFromPercentage(0.4), 0);
    QCOMPARE(percentFromPercentage(0.5), 1);
    QCOMPARE(percentFromPercentage(50.4), 50);
    QCOMPARE(percentFromPercentage(50.5), 51);
    QCOMPARE(percentFromPercentage(87.9), 88);
    QCOMPARE(percentFromPercentage(99.5), 100);
    QCOMPARE(percentFromPercentage(100.0), 100);
}

void BatteryTest::percentageOutOfRangeIsRefused()
{
    // Outside the range UPower documents, and it is refused rather than clamped: a level of 141% clamped to 100%
    // would draw a full battery where the daemon said something else entirely.
    QCOMPARE(percentFromPercentage(-1.0), std::nullopt);
    QCOMPARE(percentFromPercentage(100.1), std::nullopt);
    QCOMPARE(percentFromPercentage(141.0), std::nullopt);
}

void BatteryTest::durationIsAccepted()
{
    QCOMPARE(durationFromSeconds(1), 1);
    QCOMPARE(durationFromSeconds(60), 60);
    QCOMPARE(durationFromSeconds(3600), 3600);
    QCOMPARE(durationFromSeconds(7200), 7200);
}

void BatteryTest::zeroDurationIsRefused()
{
    // Zero is UPower's own spelling of "not known" for both clocks, and it is not a duration. Measured on the
    // machine this was written against: a wireless mouse discharging, `TimeToEmpty` of exactly 0.
    QCOMPARE(durationFromSeconds(0), std::nullopt);
}

void BatteryTest::negativeDurationIsRefused()
{
    // A negative value is refused for the same reason: no clock is negative.
    QCOMPARE(durationFromSeconds(-1), std::nullopt);
    QCOMPARE(durationFromSeconds(-3600), std::nullopt);
}

void BatteryTest::durationIsFormatted()
{
    // Hours and minutes, with the seconds dropped rather than rounded up, so the text never promises more time
    // than the daemon reported.
    QCOMPARE(formatDuration(60), QStringLiteral("0:01"));
    QCOMPARE(formatDuration(119), QStringLiteral("0:01"));
    QCOMPARE(formatDuration(120), QStringLiteral("0:02"));
    QCOMPARE(formatDuration(3599), QStringLiteral("0:59"));
    QCOMPARE(formatDuration(3600), QStringLiteral("1:00"));
    QCOMPARE(formatDuration(3660), QStringLiteral("1:01"));
    QCOMPARE(formatDuration(7200), QStringLiteral("2:00"));
    QCOMPARE(formatDuration(8135), QStringLiteral("2:15"));
}

void BatteryTest::durationUnderOneMinuteIsDrawnAsOneMinute()
{
    // A duration under a minute is drawn as one minute rather than as `0:00`: the seconds are what a readout in
    // hours and minutes cannot show, and zero minutes is the text for no time left at all.
    QCOMPARE(formatDuration(1), QStringLiteral("0:01"));
    QCOMPARE(formatDuration(30), QStringLiteral("0:01"));
    QCOMPARE(formatDuration(59), QStringLiteral("0:01"));
}

void BatteryTest::readingIsComposedFromBothObjects()
{
    ObjectProperties manager(QVariantMap{{UPowerPropertyOnBattery, true}});
    ObjectProperties device(QVariantMap{
        {UPowerPropertyIsPresent, true},
        {UPowerPropertyPercentage, 87.0},
        {UPowerPropertyState, quint32(2)},  // Discharging
        {UPowerPropertyWarningLevel, quint32(1)},  // None
        {UPowerPropertyTimeToEmpty, qint64(8100)},
    });

    const auto reading = composeReading(manager, device);

    QVERIFY(reading.onBattery);
    QVERIFY(reading.present);
    QCOMPARE(reading.percentage, 87);
    QCOMPARE(reading.state, BatteryState::Discharging);
    QCOMPARE(reading.warning, BatteryWarning::None);
    QVERIFY(reading.timeRemaining.has_value());
    QCOMPARE(*reading.timeRemaining, 8100);
}

void BatteryTest::absentDeviceIsNotRead()
{
    // Every property of an absent display device is zero, and publishing those as a reading would draw a machine
    // with a flat battery rather than one with no battery at all. So the daemon's `IsPresent` gates the rest.
    ObjectProperties manager(QVariantMap{{UPowerPropertyOnBattery, false}});
    ObjectProperties device(QVariantMap{
        {UPowerPropertyIsPresent, false},
        {UPowerPropertyPercentage, 0.0},
        {UPowerPropertyState, quint32(0)},  // Unknown
    });

    const auto reading = composeReading(manager, device);

    QVERIFY(!reading.onBattery);
    QVERIFY(!reading.present);
    QVERIFY(!reading.percentage.has_value());
    QCOMPARE(reading.state, BatteryState::Unknown);
    QVERIFY(!reading.timeRemaining.has_value());
}

void BatteryTest::timeToEmptyIsUsedWhenDischarging()
{
    ObjectProperties manager;
    ObjectProperties device(QVariantMap{
        {UPowerPropertyIsPresent, true},
        {UPowerPropertyState, quint32(2)},  // Discharging
        {UPowerPropertyTimeToEmpty, qint64(7200)},
        {UPowerPropertyTimeToFull, qint64(0)},
    });

    const auto reading = composeReading(manager, device);

    QVERIFY(reading.timeRemaining.has_value());
    QCOMPARE(*reading.timeRemaining, 7200);
}

void BatteryTest::timeToFullIsUsedWhenCharging()
{
    ObjectProperties manager;
    ObjectProperties device(QVariantMap{
        {UPowerPropertyIsPresent, true},
        {UPowerPropertyState, quint32(1)},  // Charging
        {UPowerPropertyTimeToEmpty, qint64(0)},
        {UPowerPropertyTimeToFull, qint64(3600)},
    });

    const auto reading = composeReading(manager, device);

    QVERIFY(reading.timeRemaining.has_value());
    QCOMPARE(*reading.timeRemaining, 3600);
}

void BatteryTest::firstNonZeroTimeIsUsedWhenStateIsUnknown()
{
    // The daemon has not said which way the charge is going. Both clocks are then read and the one the daemon
    // filled in is drawn, `TimeToEmpty` first.
    ObjectProperties manager;
    ObjectProperties device(QVariantMap{
        {UPowerPropertyIsPresent, true},
        {UPowerPropertyState, quint32(0)},  // Unknown
        {UPowerPropertyTimeToEmpty, qint64(0)},
        {UPowerPropertyTimeToFull, qint64(1800)},
    });

    const auto reading = composeReading(manager, device);

    QVERIFY(reading.timeRemaining.has_value());
    QCOMPARE(*reading.timeRemaining, 1800);
}

QTEST_MAIN(BatteryTest)
#include "battery_test.moc"
