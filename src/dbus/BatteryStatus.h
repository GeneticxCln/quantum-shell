// The battery readout's logic, as pure functions of what UPower sends over D-Bus.
//
// Split out of `BatteryService` for the reason `src/system/`'s /proc parsers are split out of `SysMonService`,
// `AudioVolume.h` out of `PipeWireService` and `NetworkStatus.h` out of `NetworkService`: everything here is a
// function of bytes — the property maps the daemon publishes on `PropertiesChanged` and answers to `GetAll` —
// so every shape a real daemon can send, and the shapes a healthy one never sends, is a case in `battery-test`
// with no bus, no bus address and no session. What is left in the service is plumbing: a connection, two
// subscriptions and the re-resolution that follows the daemon's own device-set signals.
//
// **Every name and number here was read off the running daemon or the installed library**, not recalled, which
// is this project's rule for anything external:
//
//   * the service name, the object paths, the interface name, the property names, the method name and the two
//     signals come from `busctl --system introspect org.freedesktop.UPower`, which is the daemon's *own*
//     description of what it serves;
//   * the enum values come from the installed `up-types.h` — `UP_DEVICE_STATE_CHARGING = 1`,
//     `UP_DEVICE_STATE_DISCHARGING = 2`, `UP_DEVICE_STATE_EMPTY = 3`, `UP_DEVICE_STATE_FULLY_CHARGED = 4`,
//     `UP_DEVICE_STATE_PENDING_CHARGE = 5`, `UP_DEVICE_STATE_PENDING_DISCHARGE = 6`, and the levels
//     `UP_DEVICE_LEVEL_UNKNOWN = 0` … `UP_DEVICE_LEVEL_ACTION = 5` — which is the library's own C++ spelling of
//     the daemon's numbers;
//   * the *tokens* are UPower's own spellings of those enums, read out of the installed
//     `libupower-glib.so`'s string table (`strings -n 3` gives `unknown`, `discharging`, `fully-charged`,
//     `pending-charge`, `pending-discharge`, `none`, `low`, `critical` as whole strings, and `charging`, `empty`
//     and `action` are pooled as suffixes of longer literals in the same table — of `discharging`,
//     `energy-empty` and `get_critical_action` — which is why a grep for them finds nothing and the offsets do).
//     A token is what QML switches on, so it is checked against the library rather than invented here.
//
// A name that is wrong in anyone else's code is a build error; a D-Bus name that is wrong here is a readout
// that never appears, which is the one failure a bar cannot report to itself. That is why the names are declared
// once below, mirrored at compile time by `battery-test`, and checked against a real bus by `battery-live-test`.
//
// **What is deliberately not read.** UPower's device interface carries twenty-odd properties, and this module
// reads six of them. `Energy`, `EnergyRate`, `Capacity` and `Voltage` are the daemon's own working values and
// would each need a decided unit and a rounding rule to become a readout; `IconName` is a name in a theme this
// bar has no icon font for; `Technology`, `ChargeCycles` and `Temperature` are facts about a battery's health
// rather than about the charge it has left. A bar shows the charge, whether it is going in or out, how long the
// daemon says that will take, and whether the daemon is worried — which is what the reading below is.
#pragma once

#include <QLatin1StringView>
#include <QString>

#include <optional>

#include "dbus/DbusProperties.h"

namespace quantum::dbus {

// --- The names this module speaks -------------------------------------------------------------------------
//
// Every name below is prefixed with the daemon it belongs to, and that is not decoration: both D-Bus modules
// live in `quantum::dbus` and both daemons have a manager object, a device interface and a property spelled
// `State`, so the unprefixed spellings would be a redefinition of NetworkManager's in the same namespace. The
// prefix says whose vocabulary each name is, which is also the thing that makes a name from one module unusable
// in the other by accident.

// The well-known name UPower owns on the system bus. Confirmed on this session: `busctl --system list` reports
// `org.freedesktop.UPower` owned by `upowerd`, and `upower -d` prints the same daemon's version.
inline constexpr QLatin1StringView UPowerService{"org.freedesktop.UPower"};
// The manager object. Its properties are the daemon's global state — whether this machine is running on its
// battery and whether the lid is shut — and it is the object the display device is asked for by.
inline constexpr QLatin1StringView UPowerManagerPath{"/org/freedesktop/UPower"};
inline constexpr QLatin1StringView UPowerManagerInterface{"org.freedesktop.UPower"};
// One object per device, including the one the daemon synthesises as the thing a bar should draw. Its
// properties are that device's charge, its state and the daemon's own warning level for it.
inline constexpr QLatin1StringView UPowerDeviceInterface{"org.freedesktop.UPower.Device"};

// The method that names the device a bar should draw: the daemon's aggregate of the batteries it has, which on
// a laptop is the pack and on a desktop with a wireless mouse is a device that is *not present*. It returns an
// object path, and it is asked rather than assumed — the path is conventionally
// `/org/freedesktop/UPower/devices/DisplayDevice` and that is not a promise, so the daemon is the one that
// names it, exactly as NetworkManager's own answers name the objects `NetworkService` walks.
inline constexpr QLatin1StringView UPowerMethodGetDisplayDevice{"GetDisplayDevice"};

// The two signals that say the set of devices moved, and therefore that the display device may be a different
// object than it was. UPower has no signal for "the display device changed path" — it has these, which is
// stronger: they say *why* it might have. They are the only reason this module ever asks the daemon for
// anything a second time.
inline constexpr QLatin1StringView UPowerSignalDeviceAdded{"DeviceAdded"};
inline constexpr QLatin1StringView UPowerSignalDeviceRemoved{"DeviceRemoved"};

// The property names, exactly as the daemon's introspection spells them.
inline constexpr QLatin1StringView UPowerPropertyOnBattery{"OnBattery"};
inline constexpr QLatin1StringView UPowerPropertyIsPresent{"IsPresent"};
inline constexpr QLatin1StringView UPowerPropertyPercentage{"Percentage"};
inline constexpr QLatin1StringView UPowerPropertyState{"State"};
inline constexpr QLatin1StringView UPowerPropertyTimeToEmpty{"TimeToEmpty"};
inline constexpr QLatin1StringView UPowerPropertyTimeToFull{"TimeToFull"};
inline constexpr QLatin1StringView UPowerPropertyWarningLevel{"WarningLevel"};

// --- UPower's own numbers ---------------------------------------------------------------------------------

// `UP_DEVICE_STATE_*`: what the device is doing. `Unknown` is the daemon's own zero — it has not decided, which
// is a reading rather than an absence — where a number this build does not know is a refusal (`nullopt` below),
// because a state a newer UPower invented is not one of these.
enum class BatteryState {
    Unknown,
    Charging,
    Discharging,
    Empty,
    Full,
    PendingCharge,
    PendingDischarge,
};

// `UP_DEVICE_LEVEL_*`, as the daemon uses them for `WarningLevel`. The values above `Action` are the ones
// `BatteryLevel` uses and are not warning levels at all, which is why the mapping below stops there: a daemon
// that sent `Normal` for a warning level would be telling us something this property cannot mean.
enum class BatteryWarning {
    Unknown,
    None,
    // The daemon's own name for "this machine is running on its battery". It is a warning level in UPower's
    // numbering, and it is *not* a warning: it is drawn as no trouble at all, which is why the widget's rule
    // names the three levels it colours rather than counting from here.
    Discharging,
    Low,
    Critical,
    Action,
};

// The daemon's `State`, as a token, and its `WarningLevel` likewise. `nullopt` for a value this build does not
// know — a refusal rather than a guess, because a number that means nothing to us is not `unknown`.
std::optional<BatteryState> batteryStateFromUpower(quint32 state);
std::optional<BatteryWarning> batteryWarningFromUpower(quint32 level);

// The tokens, which are what QML switches on and what a person reads in a record. `battery-test` mirrors them
// and `bar-interaction-test` holds them against the widget's own branches, so a rename on either side fails a
// build rather than drawing a fallback.
QString batteryStateToken(BatteryState state);
QString batteryWarningToken(BatteryWarning warning);

// --- The numbers the daemon sends, as the readout's numbers -----------------------------------------------

// The charge level, from the `Percentage` the daemon publishes as a `d`. `nullopt` for a value outside 0-100 —
// a level the daemon does not have, refused rather than clamped, because a level of 141% clamped to 100% is a
// claim about a battery that is not what the daemon said. Anything outside 0-100 is a reading this build cannot
// draw and a reason to keep the last one, not a number to fix up.
//
// Inside that range it is rounded to the nearest whole point: the daemon's number is continuous and a bar draws
// whole points, so *some* rounding is unavoidable, and the nearest point is the one at most half a point from
// what the daemon reported. Truncating instead would draw a level up to a whole point below the daemon's own
// answer, which is a systematic understatement rather than a rounding.
std::optional<int> percentFromPercentage(double percentage);

// One of the daemon's two clocks, as a duration a readout can draw. `nullopt` for zero and for a negative
// number, and both are the same statement: UPower documents `TimeToEmpty` and `TimeToFull` as "the time in
// seconds, or 0 if unknown", and it really does publish that zero on the machine this was written against — a
// wireless mouse discharging with `TimeToEmpty` of exactly 0 — so a reader that drew the raw number would show
// `0:00` for a battery with hours left. A negative value is refused for the same reason: no clock is negative.
std::optional<qint64> durationFromSeconds(qint64 seconds);

// A duration as the readout draws it: `2:15`, hours and minutes, with the seconds dropped rather than rounded
// up, so the text never promises more time than the daemon reported. A duration under a minute is drawn as one
// minute, because `0:00` is the text for no time left at all, which is a different statement — and a battery
// with forty seconds in it is not a battery with none.
QString formatDuration(qint64 seconds);

// --- The reading ------------------------------------------------------------------------------------------

// What the bar shows, as one value: the manager's global answer and the display device the daemon named for it.
// One struct rather than two published groups because the widget draws one readout, and because a reading
// assembled in one place is a reading whose parts cannot disagree with each other.
struct BatteryReading {
    // The manager's own verdict: the machine is running on its battery rather than on mains. It is the one fact
    // available even on a machine with no battery the daemon can aggregate, and it is a *reading* rather than a
    // guess — the daemon computes it from the devices it has.
    bool onBattery = false;
    // The display device's `IsPresent`, which is the daemon's answer to "is there a battery here". A desktop
    // answers no, and that is a complete reading: it is the fact this readout draws nothing for, and it is not
    // the same as a device that could not be read (the service's `available`).
    bool present = false;
    // The charge level, 0-100. Absent when the daemon did not publish one, or published one outside the range —
    // a readout with a state and no number, rather than a number nothing sent.
    std::optional<int> percentage;
    BatteryState state = BatteryState::Unknown;
    BatteryWarning warning = BatteryWarning::Unknown;
    // How long the daemon says the charge has left to move, in seconds, and which of its two clocks that came
    // from is decided by the state. Absent when the daemon published no usable number: on a full battery both
    // clocks are 0, and `0` means "not known" rather than "no time".
    std::optional<qint64> timeRemaining;

    // Comparable because that is the whole of the service's publishing rule: a signal that announced a property
    // this readout does not draw must cost no repaint, so the service compares the reading it just composed with
    // the one before it and emits only when they differ.
    bool operator==(const BatteryReading&) const = default;
};

// The reading the two objects add up to. The manager's half is what it is; the device's half is read only when
// the device says it is there, because an absent device's properties are all zero and its state is `unknown` —
// numbers that would be drawn as a battery at 0% if they were published as a reading.
BatteryReading composeReading(const ObjectProperties& manager, const ObjectProperties& device);

}  // namespace quantum::dbus
