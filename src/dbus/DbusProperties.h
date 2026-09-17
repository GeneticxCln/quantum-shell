// One D-Bus object's properties as they arrived, shared by every module that follows a daemon.
//
// This file exists because a second D-Bus module was about to need it. `src/dbus/` holds two clients now —
// `NetworkService` following NetworkManager and `BatteryService` following UPower — and both read the same two
// shapes: the `a{sv}` of a `GetAll` reply and the `a{sv}` of a `PropertiesChanged` payload. Three of the things
// below are traps that a second copy would have re-copied rather than re-derived:
//
//   * **The variant inside the variant.** Both routes put a `v` around every value, and Qt hands it over as a
//     `QDBusVariant` rather than unwrapping it. A reader that converted the `QVariant` directly finds no
//     converter to a number and reads *nothing*, which is indistinguishable from a property the daemon never
//     sent. Every accessor here goes through one unwrapping function so the trap has one home.
//   * **The type, not the conversion.** `QVariant(QString("70")).toUInt()` succeeds, so a state that arrived as
//     text would be read as a state. Every accessor checks the type the value *carries* and refuses the rest:
//     text where a number belongs is refused rather than coerced, and the reverse too.
//   * **The change that overtakes the read.** A service subscribes to an object and asks for its properties in
//     the same breath, so a `PropertiesChanged` can arrive while the first reply is in flight — and the reply,
//     being older, must not overwrite it. `ObjectProperties` holds such a change rather than merging it and
//     replays it over the reply when the reply arrives; a reader that simply assigned the reply would show the
//     value the daemon had already replaced, which on a moving signal is the common case and not a rare one.
//     The interleaving is a pure function of two payloads, so it is tested as one.
//
// Nothing here names a daemon, an interface or a property: those belong to the module that speaks them, which
// is why the names in `NetworkStatus.h` and `BatteryStatus.h` are declared beside the accessors that read them.
#pragma once

#include <QLatin1StringView>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include <optional>

namespace quantum::dbus {

// The object path that means "no object" on the bus. A daemon that has an absent reference spells it this way
// rather than omitting the property — NetworkManager names it for a machine with no primary connection, UPower
// for a device path it cannot give — so `/` is a reference to nothing rather than to an object at the root of
// the object tree. It is declared here because both readers of the bus must treat it the same way.
inline constexpr auto kNullPath = "/";

// Whether a path a daemon named is one this shell can ask about. A path from the daemon is trusted; this is not
// that check. It exists because the alternative to refusing a wrong value is building a method call to it, and a
// method call is a thing these services must not make to an object they were not told about.
bool isObjectPath(const std::optional<QString>& path);

// One object's properties, as they arrived: a `PropertiesChanged` payload, or the reply to `GetAll`. Kept as the
// map the daemon sent rather than unpacked into a struct per interface, because the objects these modules read
// have overlapping names — `State` is a different property on NetworkManager's manager and on a device, and
// UPower's `State` is a third thing again — and a single struct would have to invent a name for each one.
class ObjectProperties {
public:
    ObjectProperties() = default;
    explicit ObjectProperties(const QVariantMap& values) : values_(values) {}
    explicit ObjectProperties(QVariantMap&& values) : values_(std::move(values)) {}

    const QVariantMap& values() const { return values_; }
    bool isEmpty() const { return values_.isEmpty(); }
    // Whether a first read of this object is still in flight, which is also what says whether `values()` is yet
    // a reading at all: before the reply it is nothing but the changes that overtook it.
    bool isReading() const { return reading_; }

    // A property that should be a 32-bit unsigned number. `nullopt` for absent, for a value of another type, and
    // for one of the wrong width: `State` and `DeviceType` are 32-bit unsigned on NetworkManager's manager, and
    // a reader that took whatever it found would silently accept a string as a state.
    std::optional<quint32> uint32Value(QLatin1StringView name) const;
    // A property that should be a byte, which is 0-255 on the wire and is what NetworkManager's `Strength` is.
    // Refused rather than clamped outside that range: a clamped value is a number nothing sent.
    std::optional<int> byteValue(QLatin1StringView name) const;
    // A property that should be text, or an object path (which arrives as `QDBusObjectPath`, not as a string —
    // the one place a D-Bus type is not the C++ type a person would expect).
    std::optional<QString> textValue(QLatin1StringView name) const;
    std::optional<QString> pathValue(QLatin1StringView name) const;
    // An array of object paths (`ao`), which is how a connection names its devices. Empty for anything that is
    // not such an array, so a reader can treat "no devices" and "not this shape" as the same absence — the reply
    // to a request that has no answer is not a reason to invent one.
    QStringList pathList(QLatin1StringView name) const;
    // A property that should be a boolean, which is what UPower's `OnBattery` and `IsPresent` are. Both are
    // answers to a question rather than quantities: `false` is a reading — a machine on mains, a device that is
    // not there — and it is why a reader must not fall back on a default when the property is absent.
    std::optional<bool> boolValue(QLatin1StringView name) const;
    // A property that should be a floating-point number, which is what UPower publishes its charge level in
    // (`Percentage`, a `d`). An integer is refused rather than converted, by the rule the accessors above follow
    // and for the same reason: no property this shell reads is sent as one type and meant as another. A NaN or
    // an infinity is refused too, because neither is a level: they are the shapes a division by zero takes, and
    // a percentage drawn from one would be a number nothing sent.
    std::optional<double> doubleValue(QLatin1StringView name) const;
    // A property that should be a 64-bit signed number, which is what UPower's two clocks are (`x`, seconds, with
    // zero meaning "not known"). Refused above what a signed 64-bit number holds. A negative value is *read* and
    // not refused here: what a negative duration means is a judgement about the property rather than about its
    // width, so it belongs with the code that knows which clock it asked for — see `durationFromSeconds`.
    std::optional<qint64> int64Value(QLatin1StringView name) const;

    // A first read is in flight. Anything `applyChange` is handed before the matching `adoptAll` is held rather
    // than merged, because it is newer than the reply the daemon is about to send.
    void beginRead();
    // Adopts the reply to `GetAll` and replays the held changes over it, oldest first, so the object ends up
    // describing the newest thing the daemon has said rather than the newest thing it said *before* the read.
    void adoptAll(const QVariantMap& all);
    // The read did not arrive — the daemon refused the object, or is gone. What the object holds is what it had,
    // and the changes that overtook the read are merged now rather than held for ever: a value held behind a
    // reply that never comes is a readout that stops moving, which is the quieter half of this race.
    void abortRead();
    // Merges what a `PropertiesChanged` announced into what the object holds, and removes what it invalidated —
    // or holds the whole payload while a read is in flight. The signal carries only what moved, so a reader that
    // replaced its map with it would forget every property that did not, which is how a bar ends up with a name
    // and no strength.
    void applyChange(const QVariantMap& changed, const QStringList& invalidated);
    // Forgets everything, including a read in flight: what a daemon that left the bus told us is not part of what
    // the daemon that replaced it says.
    void clear();

private:
    QVariantMap values_;
    // Changes that arrived while a read was in flight, in arrival order.
    QVector<QPair<QVariantMap, QStringList>> held_;
    bool reading_ = false;
};

}  // namespace quantum::dbus
