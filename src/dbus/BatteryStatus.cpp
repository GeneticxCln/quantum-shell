#include "dbus/BatteryStatus.h"

#include <cmath>

namespace quantum::dbus {
namespace {

// UPower's `UpDeviceState`, as the daemon's own numbers. The installed `up-types.h` is the authority, and these
// are its spelling in C++.
constexpr quint32 kStateUnknown = 0;
constexpr quint32 kStateCharging = 1;
constexpr quint32 kStateDischarging = 2;
constexpr quint32 kStateEmpty = 3;
constexpr quint32 kStateFullyCharged = 4;
constexpr quint32 kStatePendingCharge = 5;
constexpr quint32 kStatePendingDischarge = 6;

// UPower's `UpDeviceLevel`, as far as `WarningLevel` uses it. The members above `Action` belong to
// `BatteryLevel` and are not warning levels, which is why this range stops here rather than at the enum's end.
constexpr quint32 kLevelUnknown = 0;
constexpr quint32 kLevelNone = 1;
constexpr quint32 kLevelDischarging = 2;
constexpr quint32 kLevelLow = 3;
constexpr quint32 kLevelCritical = 4;
constexpr quint32 kLevelAction = 5;

// The two ends of a charge level UPower documents, and the bound `percentFromPercentage` holds a reading to.
constexpr double kMinPercentage = 0.0;
constexpr double kMaxPercentage = 100.0;

constexpr qint64 kSecondsPerMinute = 60;
constexpr qint64 kMinutesPerHour = 60;

}  // namespace

std::optional<BatteryState> batteryStateFromUpower(quint32 state)
{
    switch (state) {
    case kStateUnknown:
        return BatteryState::Unknown;
    case kStateCharging:
        return BatteryState::Charging;
    case kStateDischarging:
        return BatteryState::Discharging;
    case kStateEmpty:
        return BatteryState::Empty;
    case kStateFullyCharged:
        return BatteryState::Full;
    case kStatePendingCharge:
        return BatteryState::PendingCharge;
    case kStatePendingDischarge:
        return BatteryState::PendingDischarge;
    default:
        // A state this build has never heard of. Refused rather than folded into `Unknown`, which is a state
        // UPower itself publishes: a daemon that has not decided and a daemon that has invented a seventh state
        // are different facts, and only one of them means the shell is out of date.
        return std::nullopt;
    }
}

std::optional<BatteryWarning> batteryWarningFromUpower(quint32 level)
{
    switch (level) {
    case kLevelUnknown:
        return BatteryWarning::Unknown;
    case kLevelNone:
        return BatteryWarning::None;
    case kLevelDischarging:
        return BatteryWarning::Discharging;
    case kLevelLow:
        return BatteryWarning::Low;
    case kLevelCritical:
        return BatteryWarning::Critical;
    case kLevelAction:
        return BatteryWarning::Action;
    default:
        return std::nullopt;
    }
}

QString batteryStateToken(BatteryState state)
{
    switch (state) {
    case BatteryState::Unknown:
        return QStringLiteral("unknown");
    case BatteryState::Charging:
        return QStringLiteral("charging");
    case BatteryState::Discharging:
        return QStringLiteral("discharging");
    case BatteryState::Empty:
        return QStringLiteral("empty");
    // UPower's own spelling of `UP_DEVICE_STATE_FULLY_CHARGED`, read out of the library's string table: the
    // token is the daemon's word for it rather than a second vocabulary this shell invents.
    case BatteryState::Full:
        return QStringLiteral("fully-charged");
    case BatteryState::PendingCharge:
        return QStringLiteral("pending-charge");
    case BatteryState::PendingDischarge:
        return QStringLiteral("pending-discharge");
    }
    return QStringLiteral("unknown");
}

QString batteryWarningToken(BatteryWarning warning)
{
    switch (warning) {
    case BatteryWarning::Unknown:
        return QStringLiteral("unknown");
    case BatteryWarning::None:
        return QStringLiteral("none");
    case BatteryWarning::Discharging:
        return QStringLiteral("discharging");
    case BatteryWarning::Low:
        return QStringLiteral("low");
    case BatteryWarning::Critical:
        return QStringLiteral("critical");
    case BatteryWarning::Action:
        return QStringLiteral("action");
    }
    return QStringLiteral("unknown");
}

std::optional<int> percentFromPercentage(double percentage)
{
    // Outside the range the daemon documents, and it is refused rather than clamped: this is the value a widget
    // draws as a proportion, and a level of 141% clamped to 100% would draw a full battery where the daemon said
    // something else entirely. The caller keeps the reading it had and the reason is a record.
    if (percentage < kMinPercentage || percentage > kMaxPercentage) {
        return std::nullopt;
    }
    // The nearest whole point. `std::lround` rather than a cast: a cast truncates toward zero, which for a level
    // of 87.9% would draw 87% — a systematic understatement of up to a whole point, where rounding is at most
    // half of one. Both ends of the range are exact here, so `100.0` cannot round to 101.
    return static_cast<int>(std::lround(percentage));
}

std::optional<qint64> durationFromSeconds(qint64 seconds)
{
    // Zero is UPower's own spelling of "not known" for both clocks, and it is not a duration: `upower -d` prints
    // no time at all for a device that reports zero, and a readout that drew it would say `0:00` — a battery with
    // no charge left — about a battery that has hours. Measured on the machine this was written against: a
    // wireless mouse discharging, `TimeToEmpty` of exactly 0.
    if (seconds <= 0) {
        return std::nullopt;
    }
    return seconds;
}

QString formatDuration(qint64 seconds)
{
    // A duration under a minute is drawn as one minute rather than as `0:00`: the seconds are what a readout in
    // hours and minutes cannot show, and zero minutes is the text for no time left at all. The alternative —
    // rounding the seconds up into the minute — would make `1:00` mean somewhere between 60 and 119 seconds,
    // which is a promise the daemon's number does not support.
    if (seconds < kSecondsPerMinute) {
        seconds = kSecondsPerMinute;
    }
    const qint64 minutes = seconds / kSecondsPerMinute;
    const qint64 hours = minutes / kMinutesPerHour;
    // The seconds are dropped, never rounded up: `2:15` should mean at least two hours and fifteen minutes, so
    // that a person reading it is never told they have longer than the daemon reported.
    return QStringLiteral("%1:%2").arg(hours).arg(minutes % kMinutesPerHour, 2, 10, QLatin1Char('0'));
}

BatteryReading composeReading(const ObjectProperties& manager, const ObjectProperties& device)
{
    BatteryReading reading;

    // The manager's half. `OnBattery` is read as the boolean it is; a manager that has not published it is a
    // manager that has not described itself, and the service's `available` is what says so rather than this
    // value — `false` here means "not on battery", which is what a machine on mains is.
    reading.onBattery = manager.boolValue(UPowerPropertyOnBattery).value_or(false);

    // The device's half, and it is read only when the device says it is there. Every property of an absent
    // display device is zero — on the desktop this was written against, `Percentage` is 0 and `State` is
    // `unknown` — and publishing those as a reading would draw a machine with a flat battery rather than one
    // with no battery at all. So the daemon's `IsPresent` gates the rest: not there is a complete reading.
    reading.present = device.boolValue(UPowerPropertyIsPresent).value_or(false);
    if (!reading.present) {
        return reading;
    }

    if (const auto percentage = device.doubleValue(UPowerPropertyPercentage)) {
        // A level the daemon published outside the range is refused, which leaves the reading with no level
        // rather than with a made-up one, and the service keeps the number it had.
        reading.percentage = percentFromPercentage(*percentage);
    }

    if (const auto state = device.uint32Value(UPowerPropertyState)) {
        reading.state = batteryStateFromUpower(*state).value_or(BatteryState::Unknown);
    }

    if (const auto level = device.uint32Value(UPowerPropertyWarningLevel)) {
        reading.warning = batteryWarningFromUpower(*level).value_or(BatteryWarning::Unknown);
    }

    // Which of the daemon's two clocks this readout draws: the one that matches the direction the charge is
    // going, because that is what each of them means. `TimeToFull` on a charging battery and `TimeToEmpty` on a
    // discharging one; a battery that is full has neither, and UPower publishes zero for both.
    switch (reading.state) {
    case BatteryState::Charging:
    case BatteryState::PendingCharge:
        reading.timeRemaining = durationFromSeconds(device.int64Value(UPowerPropertyTimeToFull).value_or(0));
        break;
    case BatteryState::Discharging:
    case BatteryState::PendingDischarge:
        reading.timeRemaining = durationFromSeconds(device.int64Value(UPowerPropertyTimeToEmpty).value_or(0));
        break;
    case BatteryState::Unknown:
    case BatteryState::Empty:
    case BatteryState::Full:
        // The daemon has not said which way the charge is going, or has said there is nothing left to move.
        // Both clocks are then read and the one the daemon filled in is drawn, `TimeToEmpty` first, because a
        // machine resting on its battery is what this covers in practice. This is the one place a *value*
        // decides rather than a state, and it chooses between two numbers the daemon published rather than
        // between a number and a guess.
        reading.timeRemaining = durationFromSeconds(device.int64Value(UPowerPropertyTimeToEmpty).value_or(0));
        if (!reading.timeRemaining) {
            reading.timeRemaining = durationFromSeconds(device.int64Value(UPowerPropertyTimeToFull).value_or(0));
        }
        break;
    }

    return reading;
}

}  // namespace quantum::dbus
