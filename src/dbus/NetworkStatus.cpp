#include "dbus/NetworkStatus.h"

#include <QStringList>

#include <algorithm>

namespace quantum::dbus {
namespace {

// NetworkManager publishes `State` as 32-bit unsigned and `Strength` as a byte. `nm-dbus-interface.h` is the
// authority for the numbers; these two are its spelling in C++.
constexpr quint32 kNmStateAsleep = 10;
constexpr quint32 kNmStateDisconnected = 20;
constexpr quint32 kNmStateDisconnecting = 30;
constexpr quint32 kNmStateConnecting = 40;
constexpr quint32 kNmStateConnectedLocal = 50;
constexpr quint32 kNmStateConnectedSite = 60;
constexpr quint32 kNmStateConnectedGlobal = 70;

constexpr quint32 kNmConnectivityNone = 1;
constexpr quint32 kNmConnectivityPortal = 2;
constexpr quint32 kNmConnectivityLimited = 3;
constexpr quint32 kNmConnectivityFull = 4;

constexpr quint32 kNmDeviceTypeEthernet = 1;
constexpr quint32 kNmDeviceTypeWifi = 2;

// The two connection `Type` strings this build recognises. They are the settings keys NetworkManager's own
// profiles are written with ("802-11-wireless" is what `nmcli connection show` calls wifi), not invented here.
constexpr auto kWirelessConnectionType = "802-11-wireless";
constexpr auto kWiredConnectionType = "802-3-ethernet";

}  // namespace

std::optional<ConnectionState> connectionStateFromNmState(quint32 state)
{
    switch (state) {
    case kNmStateAsleep:
    case kNmStateDisconnected:
    case kNmStateDisconnecting:
        return ConnectionState::Disconnected;
    case kNmStateConnecting:
    case kNmStateConnectedLocal:
    case kNmStateConnectedSite:
        return ConnectionState::Connecting;
    case kNmStateConnectedGlobal:
        return ConnectionState::Connected;
    default:
        // A value this build does not know. NetworkManager has not added one since 70, so this is a newer daemon
        // or a reading of the wrong property — either way, refusing beats guessing: a bar that drew
        // "connected" for an unknown number would claim something nothing told it.
        return std::nullopt;
    }
}

std::optional<Connectivity> connectivityFromNmValue(quint32 value)
{
    switch (value) {
    case kNmConnectivityNone:
        return Connectivity::None;
    case kNmConnectivityPortal:
        return Connectivity::Portal;
    case kNmConnectivityLimited:
        return Connectivity::Limited;
    case kNmConnectivityFull:
        return Connectivity::Full;
    default:
        return std::nullopt;
    }
}

DeviceKind deviceKindFromDeviceType(quint32 deviceType)
{
    switch (deviceType) {
    case kNmDeviceTypeWifi:
        return DeviceKind::Wifi;
    case kNmDeviceTypeEthernet:
        return DeviceKind::Ethernet;
    default:
        return DeviceKind::Other;
    }
}

DeviceKind deviceKindFromConnectionType(QStringView connectionType)
{
    if (connectionType == QLatin1String(kWirelessConnectionType)) {
        return DeviceKind::Wifi;
    }
    if (connectionType == QLatin1String(kWiredConnectionType)) {
        return DeviceKind::Ethernet;
    }
    return DeviceKind::Other;
}

DeviceKind deviceKindFor(quint32 deviceType, QStringView connectionType)
{
    // The device wins, and that is a decision rather than a default: the strength this readout draws lives on
    // the *device's* active access point, so a reading that called a wifi device "ethernet" because a profile's
    // `Type` said so would ask for a strength it can never get. A vpn over wifi is the case this settles.
    const DeviceKind fromDevice = deviceKindFromDeviceType(deviceType);
    if (fromDevice != DeviceKind::Other) {
        return fromDevice;
    }
    // Only when the device did not say: a device the daemon has published without `DeviceType` yet, which
    // happens on the properties-changed path where only what moved arrives.
    return deviceKindFromConnectionType(connectionType);
}

QString connectionStateToken(ConnectionState state)
{
    switch (state) {
    case ConnectionState::Disconnected:
        return QStringLiteral("disconnected");
    case ConnectionState::Connecting:
        return QStringLiteral("connecting");
    case ConnectionState::Connected:
        return QStringLiteral("connected");
    }
    return QStringLiteral("disconnected");
}

QString connectivityToken(Connectivity connectivity)
{
    switch (connectivity) {
    case Connectivity::None:
        return QStringLiteral("none");
    case Connectivity::Portal:
        return QStringLiteral("portal");
    case Connectivity::Limited:
        return QStringLiteral("limited");
    case Connectivity::Full:
        return QStringLiteral("full");
    }
    return QStringLiteral("none");
}

QString deviceKindToken(DeviceKind kind)
{
    switch (kind) {
    case DeviceKind::Wifi:
        return QStringLiteral("wifi");
    case DeviceKind::Ethernet:
        return QStringLiteral("ethernet");
    case DeviceKind::Other:
        return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

NetworkReading composeReading(const ObjectProperties& manager, const ObjectProperties& connection,
                              const ObjectProperties& device, const ObjectProperties& accessPoint)
{
    NetworkReading reading;

    // The state is required to be one this build knows; an unknown one is read as disconnected, because that is
    // the reading that claims the least. `network-test` pins that this is a refusal and not a default in
    // disguise: the *service* keeps the last reading when a property it cannot read arrives, so this path is only
    // reached on the first reading of a daemon that speaks a newer vocabulary.
    if (const auto state = manager.uint32Value(PropertyState)) {
        reading.state = connectionStateFromNmState(*state).value_or(ConnectionState::Disconnected);
    }
    if (const auto connectivity = manager.uint32Value(PropertyConnectivity)) {
        reading.connectivity = connectivityFromNmValue(*connectivity);
    }

    // The chain. A manager with no primary connection is a machine that is offline, which is a complete reading:
    // the state above is the whole of it, and the name, interface, kind and strength stay absent rather than
    // becoming an empty string a widget would draw.
    const auto primary = manager.pathValue(PropertyPrimaryConnection);
    if (!primary || *primary == QLatin1String(kNullPath)) {
        return reading;
    }

    reading.name = connection.textValue(PropertyId).value_or(QString());
    reading.interfaceName = device.textValue(PropertyDeviceInterface).value_or(QString());

    const quint32 deviceType = device.uint32Value(PropertyDeviceType).value_or(0);
    const QString connectionType = connection.textValue(PropertyType).value_or(QString());
    reading.kind = deviceKindFor(deviceType, connectionType);

    // The strength exists only on a wireless device's active access point. `Strength` is read as the byte it is
    // and clamped into 0-100, because the daemon documents the range and a value outside it is a daemon telling
    // us something this readout cannot draw; the clamp is the honest bound rather than a wrong number.
    if (reading.kind == DeviceKind::Wifi) {
        if (const auto strength = accessPoint.byteValue(PropertyStrength)) {
            reading.strength = std::clamp(*strength, 0, 100);
        }
    }

    return reading;
}

}  // namespace quantum::dbus
