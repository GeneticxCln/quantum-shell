#include "dbus/DbusProperties.h"

#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusVariant>

#include <cmath>
#include <limits>

namespace quantum::dbus {
namespace {

// The value inside a `v`, however it was wrapped. Both routes a property can arrive by put one there — the
// `a{sv}` of a `GetAll` reply and the `a{sv}` of a `PropertiesChanged` payload — and Qt hands it over as a
// `QDBusVariant` rather than unwrapping it. A reader that skipped this step would find no converter from
// `QDBusVariant` to a number and read nothing, which looks exactly like a property the daemon never sent; that
// is the failure this one function exists to prevent, and it is why every accessor goes through it.
QVariant unwrapped(const QVariant& value)
{
    if (value.metaType() == QMetaType::fromType<QDBusVariant>()) {
        return value.value<QDBusVariant>().variant();
    }
    return value;
}

// Whether a value is an integer, by the type it carries rather than by what it can be converted to. The
// difference matters: `QVariant(QString("70")).toUInt()` succeeds, so a state that arrived as text would be
// read as a state — and no property this shell reads is sent as text and meant as a number. A conversion is a
// guess about what a sender meant; the type is what it said.
bool isInteger(const QVariant& value)
{
    switch (value.typeId()) {
    case QMetaType::Char:
    case QMetaType::SChar:
    case QMetaType::UChar:
    case QMetaType::Short:
    case QMetaType::UShort:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::Long:
    case QMetaType::ULong:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return true;
    default:
        return false;
    }
}

// The signed half of the same question, because the width a value holds and its sign are two different refusals:
// `State` is 32-bit unsigned, so a negative number is not a state and a wrapped conversion of one would be a
// state nothing sent.
bool isSignedInteger(const QVariant& value)
{
    switch (value.typeId()) {
    case QMetaType::Char:
    case QMetaType::SChar:
    case QMetaType::Short:
    case QMetaType::Int:
    case QMetaType::Long:
    case QMetaType::LongLong:
        return true;
    default:
        return false;
    }
}

}  // namespace

bool isObjectPath(const std::optional<QString>& path)
{
    return path && *path != QLatin1String(kNullPath) && path->startsWith(QLatin1Char('/'));
}

std::optional<quint32> ObjectProperties::uint32Value(QLatin1StringView name) const
{
    const auto it = values_.constFind(name.toString());
    if (it == values_.constEnd()) {
        return std::nullopt;
    }
    const QVariant value = unwrapped(*it);
    if (!isInteger(value)) {
        return std::nullopt;
    }
    // Both bounds are checked on the value as it arrived rather than left to the conversion, because a
    // conversion from a negative or oversized number does not fail — it wraps, and a wrapped state is a state
    // nothing sent. `State` is 32-bit unsigned; that is the whole of the range.
    if (isSignedInteger(value) && value.toLongLong() < 0) {
        return std::nullopt;
    }
    const qulonglong wide = value.toULongLong();
    if (wide > std::numeric_limits<quint32>::max()) {
        return std::nullopt;
    }
    return static_cast<quint32>(wide);
}

std::optional<int> ObjectProperties::byteValue(QLatin1StringView name) const
{
    const auto it = values_.constFind(name.toString());
    if (it == values_.constEnd()) {
        return std::nullopt;
    }
    const QVariant value = unwrapped(*it);
    if (!isInteger(value)) {
        return std::nullopt;
    }
    bool ok = false;
    const uint converted = value.toUInt(&ok);
    // A byte is 0-255 on the wire, so anything outside that is not this property at all — a reader that has read
    // something else, or a daemon that has changed what the property is. Refused rather than clamped: a clamped
    // value would be a number nothing sent.
    if (!ok || converted > 255) {
        return std::nullopt;
    }
    return static_cast<int>(converted);
}

std::optional<bool> ObjectProperties::boolValue(QLatin1StringView name) const
{
    const auto it = values_.constFind(name.toString());
    if (it == values_.constEnd()) {
        return std::nullopt;
    }
    const QVariant value = unwrapped(*it);
    // A boolean and nothing else. `QVariant(1).toBool()` succeeds and gives true, so a reader that converted
    // would read a number as an answer — and the two properties this is used for are both answers a reader must
    // not invent: `IsPresent` false is "the daemon says there is no battery", which is a reading, and a
    // conversion from an absent-looking zero would produce the same word from a number nothing sent.
    if (value.typeId() != QMetaType::Bool) {
        return std::nullopt;
    }
    return value.toBool();
}

std::optional<double> ObjectProperties::doubleValue(QLatin1StringView name) const
{
    const auto it = values_.constFind(name.toString());
    if (it == values_.constEnd()) {
        return std::nullopt;
    }
    const QVariant value = unwrapped(*it);
    // Only the two types a `d` can arrive as. An integer is refused rather than widened, for the reason above:
    // no property this shell reads is published as one type and meant as another, and accepting both would hide
    // a daemon that had changed what a property is.
    if (value.typeId() != QMetaType::Double && value.typeId() != QMetaType::Float) {
        return std::nullopt;
    }
    const double number = value.toDouble();
    // Neither of the two shapes a broken computation takes is a level. A NaN would compare false against every
    // bound and could be drawn as anything; an infinity would pass a `> 100` check and be clamped into a full
    // battery. Both are refused here, where the reason can be stated once, rather than at three call sites.
    if (!std::isfinite(number)) {
        return std::nullopt;
    }
    return number;
}

std::optional<qint64> ObjectProperties::int64Value(QLatin1StringView name) const
{
    const auto it = values_.constFind(name.toString());
    if (it == values_.constEnd()) {
        return std::nullopt;
    }
    const QVariant value = unwrapped(*it);
    if (!isInteger(value)) {
        return std::nullopt;
    }
    if (!isSignedInteger(value)) {
        // An unsigned value fits a signed 64-bit number as long as it is not above its maximum, which is the
        // bound checked here: `x` is signed on the wire, so a value that arrived unsigned is a number that
        // cannot be one — where the alternative, letting the conversion wrap it, is a clock that reads negative.
        const qulonglong wide = value.toULongLong();
        if (wide > static_cast<qulonglong>(std::numeric_limits<qint64>::max())) {
            return std::nullopt;
        }
        return static_cast<qint64>(wide);
    }
    return value.toLongLong();
}

std::optional<QString> ObjectProperties::textValue(QLatin1StringView name) const
{
    const auto it = values_.constFind(name.toString());
    if (it == values_.constEnd()) {
        return std::nullopt;
    }
    const QVariant value = unwrapped(*it);
    // Text is read only where text is what the property is: a number in it is a reader asking the wrong
    // question, and `toString()` would answer one anyway.
    if (value.typeId() != QMetaType::QString) {
        return std::nullopt;
    }
    return value.toString();
}

std::optional<QString> ObjectProperties::pathValue(QLatin1StringView name) const
{
    const auto it = values_.constFind(name.toString());
    if (it == values_.constEnd()) {
        return std::nullopt;
    }
    // An object path arrives as `QDBusObjectPath` inside the variant; a plain string is accepted too, because
    // a hand-built `QVariantMap` — a test's, or a daemon that thinks in strings — is the same reading.
    const QVariant value = unwrapped(*it);
    if (value.metaType() == QMetaType::fromType<QDBusObjectPath>()) {
        return value.value<QDBusObjectPath>().path();
    }
    if (value.canConvert<QString>()) {
        return value.toString();
    }
    return std::nullopt;
}

QStringList ObjectProperties::pathList(QLatin1StringView name) const
{
    const auto it = values_.constFind(name.toString());
    if (it == values_.constEnd()) {
        return {};
    }
    QVariant value = unwrapped(*it);
    // An array of object paths does **not** arrive as a list, and this was measured rather than assumed: Qt
    // demarshals a property map one level deep, so a variant holding a simple value comes back as that value
    // (`Id` is a string, `PrimaryConnection` an object path) and one holding a container comes back as the
    // `QDBusArgument` the container is read from — `Devices` has the signature `ao`. A reader that handled only
    // the list shape would see no devices at all, which is the failure that looks exactly like a machine with no
    // network hardware, and it is why the live test against the real daemon and this one agree on the shape.
    if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
        const QDBusArgument argument = value.value<QDBusArgument>();
        // `currentSignature` is the wire signature of what the argument holds, and it is what says whether this is
        // an array of paths (`ao`, which is what NetworkManager's `Devices` is), an array of variants (`av`), or
        // something this build has no business reading as a list of objects.
        const QString signature = argument.currentSignature();
        if (signature == QLatin1String("ao")) {
            QStringList paths;
            argument.beginArray();
            while (!argument.atEnd()) {
                QDBusObjectPath path;
                argument >> path;
                paths.append(path.path());
            }
            argument.endArray();
            return paths;
        }
        if (signature != QLatin1String("av")) {
            // An array of something that is not a path is not a list of paths, and the interesting case — a
            // signature this build has never seen — is refused rather than guessed at.
            return {};
        }
        value = qdbus_cast<QVariantList>(argument);
    }
    // A list is a list, and one path is not an array of them: `toList()` on a bare string succeeds and gives a
    // one-element list, so a reader that only called it would read a single path where the property is an array
    // — and a device list is walked through, so the difference is a walk that goes somewhere nothing named.
    if (value.metaType() != QMetaType::fromType<QVariantList>()
        && value.metaType() != QMetaType::fromType<QStringList>()) {
        return {};
    }
    const QVariantList elements = value.toList();
    QStringList paths;
    paths.reserve(elements.size());
    for (const QVariant& element : elements) {
        // Each element is a variant in its own right — `ao` is an array of object paths and D-Bus wraps each
        // one — so the same unwrapping applies one level down.
        const QVariant path = unwrapped(element);
        if (path.metaType() == QMetaType::fromType<QDBusObjectPath>()) {
            paths.append(path.value<QDBusObjectPath>().path());
        } else if (path.canConvert<QString>()) {
            paths.append(path.toString());
        }
    }
    return paths;
}

void ObjectProperties::beginRead()
{
    reading_ = true;
}

void ObjectProperties::adoptAll(const QVariantMap& all)
{
    values_ = all;
    reading_ = false;
    // The held changes go on last and in arrival order, because each of them is newer than the reply and a
    // later one supersedes an earlier one: replaying them backwards would end on the oldest value the daemon
    // sent after the read, which is the same class of mistake as assigning the reply over them.
    for (const auto& [changed, invalidated] : held_) {
        for (auto it = changed.constBegin(); it != changed.constEnd(); ++it) {
            values_.insert(it.key(), it.value());
        }
        for (const QString& name : invalidated) {
            values_.remove(name);
        }
    }
    held_.clear();
}

void ObjectProperties::abortRead()
{
    reading_ = false;
    for (const auto& [changed, invalidated] : held_) {
        for (auto it = changed.constBegin(); it != changed.constEnd(); ++it) {
            values_.insert(it.key(), it.value());
        }
        for (const QString& name : invalidated) {
            values_.remove(name);
        }
    }
    held_.clear();
}

void ObjectProperties::applyChange(const QVariantMap& changed, const QStringList& invalidated)
{
    if (reading_) {
        // Newer than the reply still on its way, so it is held rather than merged: merging it now would be
        // undone by `adoptAll`, and that is the bug this class exists to make impossible.
        held_.append({changed, invalidated});
        return;
    }
    for (auto it = changed.constBegin(); it != changed.constEnd(); ++it) {
        values_.insert(it.key(), it.value());
    }
    for (const QString& name : invalidated) {
        values_.remove(name);
    }
}

void ObjectProperties::clear()
{
    values_.clear();
    held_.clear();
    reading_ = false;
}

}  // namespace quantum::dbus
