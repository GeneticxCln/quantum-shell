// The network readout's logic, as pure functions of what NetworkManager sends over D-Bus.
//
// Split out of `NetworkService` for the reason `src/system/`'s /proc parsers are split out of `SysMonService`
// and `AudioVolume.h` out of `PipeWireService`: everything here is a function of bytes — the property maps the
// daemon publishes on `PropertiesChanged` and answers to `GetAll` — so every shape a real daemon can send, and
// the shapes a healthy one never sends, is a case in `network-test` with no bus, no bus address and no session.
// What is left in the service is plumbing: a connection, four subscriptions and the re-subscription that
// follows the primary connection when it changes.
//
// **Every name and number here was read off the running daemon**, not recalled, which is this project's rule
// for anything external: the service name, the object paths and the interface names come from
// `busctl introspect --xml-interface`, which is the daemon's *own* description of what it serves, and the enum
// values come from the installed `nm-dbus-interface.h` — `NM_STATE_CONNECTED_GLOBAL = 70`,
// `NM_CONNECTIVITY_FULL = 4`, `NM_DEVICE_TYPE_WIFI = 2`, `NM_DEVICE_TYPE_ETHERNET = 1` — and are confirmed
// against the live daemon wherever a value can be observed, because a header and a daemon can disagree:
// on this session the manager's `State` is 70 and `nmcli general` prints `connected`, its `Connectivity` is 4
// and nmcli prints `full`, and the device `wlan0` reports `DeviceType` 2 with `nmcli` calling it wifi.
//
// A name that is wrong in anyone else's code is a build error; a D-Bus name that is wrong here is a readout
// that never appears, which is the one failure a bar cannot report to itself. That is why the names are
// declared once below, mirrored at compile time by `network-test`, and checked against a real bus by
// `network-live-test`.
#pragma once

#include <QMap>
#include <QString>
#include <QLatin1StringView>
#include <QStringList>
#include <QStringView>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include <optional>
#include <utility>

#include "dbus/DbusProperties.h"

namespace quantum::dbus {

// --- The names this module speaks -------------------------------------------------------------------------

// The well-known name NetworkManager owns on the system bus.
inline constexpr QLatin1StringView NetworkManagerService{"org.freedesktop.NetworkManager"};
// The manager object. Its properties are the daemon's global state: which connection is primary, what state it
// is in, and whether the daemon thinks the network works at all.
inline constexpr QLatin1StringView ManagerPath{"/org/freedesktop/NetworkManager"};
inline constexpr QLatin1StringView ManagerInterface{"org.freedesktop.NetworkManager"};
// One device per interface, under the manager's `Devices`. The device carries the interface name, its type and
// the path of the active connection using it.
inline constexpr QLatin1StringView DeviceInterface{"org.freedesktop.NetworkManager.Device"};
// The wireless half of a device, on the *same object path* as its device — `ActiveAccessPoint` is how the
// signal strength is reached, and there is no such interface on a wired device.
inline constexpr QLatin1StringView WirelessInterface{"org.freedesktop.NetworkManager.Device.Wireless"};
// One access point per BSS, and the only place a signal quality exists: `Strength` is a byte, 0-100, which
// NetworkManager defines as the signal quality and `nmcli` prints in its SIGNAL column.
inline constexpr QLatin1StringView AccessPointInterface{"org.freedesktop.NetworkManager.AccessPoint"};
// The connection in use, under the manager's `ActiveConnections`. Its `Id` is the name a person sees — the
// SSID for wifi — and its `Type` says how it is carried.
inline constexpr QLatin1StringView ActiveConnectionInterface{"org.freedesktop.NetworkManager.Connection.Active"};
// The standard interface every D-Bus object has, and the signal that makes this module event-driven: a
// property that changes is announced rather than asked for.
inline constexpr QLatin1StringView PropertiesInterface{"org.freedesktop.DBus.Properties"};
inline constexpr QLatin1StringView PropertiesChangedSignal{"PropertiesChanged"};

// The property names, exactly as the daemon's introspection spells them. `QLatin1StringView` rather than
// `const char*`, so a name is a value the accessors below can be handed directly: the alternative is a
// wrapper at every call site, and one missing wrapper is a property that reads as absent.
inline constexpr QLatin1StringView PropertyDevices{"Devices"};
inline constexpr QLatin1StringView PropertyState{"State"};
inline constexpr QLatin1StringView PropertyConnectivity{"Connectivity"};
inline constexpr QLatin1StringView PropertyPrimaryConnection{"PrimaryConnection"};
inline constexpr QLatin1StringView PropertyId{"Id"};
inline constexpr QLatin1StringView PropertyType{"Type"};
inline constexpr QLatin1StringView PropertyDeviceInterface{"Interface"};
inline constexpr QLatin1StringView PropertyDeviceType{"DeviceType"};
inline constexpr QLatin1StringView PropertyActiveConnection{"ActiveConnection"};
inline constexpr QLatin1StringView PropertyActiveAccessPoint{"ActiveAccessPoint"};
inline constexpr QLatin1StringView PropertyStrength{"Strength"};

// --- NetworkManager's own numbers -------------------------------------------------------------------------

// The values of the daemon's `State` property, grouped the way a bar draws them rather than one token per
// member: a person does not distinguish "connected (site only)" from "connected (global)" on a bar, but they
// do distinguish connecting from connected, which is why those two are not folded together.
enum class ConnectionState {
    // 10 asleep, 20 disconnected, 30 disconnecting. Nothing is being carried.
    Disconnected,
    // 40 connecting, 50 connected (local only), 60 connected (site only): the link is up and the daemon is
    // still working on it. A bar showing "connected" here would be claiming something that is not true yet.
    Connecting,
    // 70 connected (global): what `nmcli general` prints as `connected`.
    Connected,
};

// The daemon's `Connectivity` property — its own verdict on whether the network works, which is a different
// question from whether a link is up: a captive portal is connected and not usable.
enum class Connectivity {
    None,     // 1: no connectivity
    Portal,   // 2: a captive portal is in the way
    Limited,  // 3: a connection that cannot reach everything
    Full,     // 4: what `nmcli general connectivity` prints as `full`
};

// What a device is, as far as the bar cares. The daemon's `DeviceType` has twenty-odd members; the ones that
// change what a readout shows are wifi — which has a signal strength — and ethernet, which does not.
enum class DeviceKind {
    Wifi,
    Ethernet,
    // Everything else: a loopback, a p2p device, a bridge. Drawn as a connection with no quality, because that
    // is what it is.
    Other,
};

// The daemon's `State`, as the token a widget switches on. `nullopt` for a value this build does not know,
// which is a refusal rather than a guess: a number that means nothing to us is not "disconnected".
std::optional<ConnectionState> connectionStateFromNmState(quint32 state);
std::optional<Connectivity> connectivityFromNmValue(quint32 value);

// The kind a device is, from two of the daemon's own spellings: the `DeviceType` number, and the connection
// `Type` string ("802-11-wireless", "802-3-ethernet"). Either can be missing on an object the daemon has only
// partly published, so both are optional and `Other` is the answer for anything unrecognised.
DeviceKind deviceKindFromDeviceType(quint32 deviceType);
DeviceKind deviceKindFromConnectionType(QStringView connectionType);
// The two spellings disagreeing is possible in principle — a vpn connection on a wifi device is one case — and
// the device is the authority for what the readout can show, because the strength lives on the device's access
// point rather than on the connection. Both functions are here so the disagreement is a decision rather than an
// accident, and `network-test` pins which side wins.
DeviceKind deviceKindFor(quint32 deviceType, QStringView connectionType);

// The tokens, which are what QML switches on. `network-test` mirrors them and `bar-interaction-test` holds them
// against the widget's own branches, so a rename on either side fails a build rather than drawing a fallback.
QString connectionStateToken(ConnectionState state);
QString connectivityToken(Connectivity connectivity);
QString deviceKindToken(DeviceKind kind);

// --- What the daemon sent ---------------------------------------------------------------------------------

// One object's properties as they arrived, and the in-flight read this module's signals overtake: both are
// `ObjectProperties` in `dbus/DbusProperties.h` now, where the battery module reads them too. It moved there in
// the change that added the second D-Bus client, and for the reason the heading above claims — the class holds
// this module's one real race, and a second copy of a race is a second place to fix it. The property names it
// is asked for are still declared here, beside the module that speaks them.

// --- The reading ------------------------------------------------------------------------------------------

// What the bar shows, as one value: the four objects of the chain — the manager, the primary connection, the
// device it runs on, and that device's active access point — added up. One struct rather than four published
// groups because the widget draws one readout, and because a reading assembled in one place is a reading whose
// parts cannot disagree with each other.
struct NetworkReading {
    // The daemon's global state, as the widget's branch. `Disconnected` is a real reading — the bar draws
    // "offline" — where an absent reading is the daemon not answering at all, which is the service's `available`.
    ConnectionState state = ConnectionState::Disconnected;
    // The active connection's `Id`: the SSID for wifi, the profile name for anything else. Empty when there is
    // no primary connection.
    QString name;
    // The device's interface name, `wlan0` or `enp39s0`. Empty when there is no primary connection.
    QString interfaceName;
    DeviceKind kind = DeviceKind::Other;
    // The active access point's signal quality, 0-100. Only wifi has one; `nullopt` on everything else,
    // because the alternative is a zero the widget would draw as a signal that is not there.
    std::optional<int> strength;
    // The daemon's verdict on whether the network works. Absent while the daemon has not said, which is not the
    // same as `Full`: a bar that drew "everything is fine" before the daemon answered would be inventing it.
    std::optional<Connectivity> connectivity;

    // Comparable because that is the whole of the service's publishing rule: a signal that announced a property
    // this readout does not draw must cost no repaint, so the service compares the reading it just composed with
    // the one before it and emits only when they differ.
    bool operator==(const NetworkReading&) const = default;
};

// The reading the four objects add up to. The manager is required — a service with no manager has no reading at
// all — and the other three are what the chain named: a manager whose `PrimaryConnection` is the null path is a
// reading with a state and nothing else, which is what a bar shows when a machine is offline.
NetworkReading composeReading(const ObjectProperties& manager, const ObjectProperties& connection,
                              const ObjectProperties& device, const ObjectProperties& accessPoint);

}  // namespace quantum::dbus
