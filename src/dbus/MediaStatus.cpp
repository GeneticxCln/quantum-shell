#include "dbus/MediaStatus.h"

#include <QDBusArgument>

namespace quantum::dbus::mpris {

MediaReading parseMetadata(const QVariantMap& metadata)
{
    MediaReading reading;

    // Extract title - xesam:title is a string
    if (metadata.contains(constants::metadataTitle)) {
        reading.title = extractString(metadata[constants::metadataTitle]);
    }

    // Extract artist - xesam:artist is a string array, join with ", "
    if (metadata.contains(constants::metadataArtist)) {
        const QStringList artists = extractStringList(metadata[constants::metadataArtist]);
        if (!artists.isEmpty()) {
            reading.artist = artists.join(", ");
        }
    }

    // A track exists if we have at least a title or artist
    reading.hasTrack = !reading.title.isEmpty() || !reading.artist.isEmpty();

    return reading;
}

QString normalizePlaybackStatus(const QString& status)
{
    if (status == constants::statusPlaying) {
        return QStringLiteral("playing");
    }
    if (status == constants::statusPaused) {
        return QStringLiteral("paused");
    }
    if (status == constants::statusStopped) {
        return QStringLiteral("stopped");
    }
    // Unknown status -> empty string
    return QString();
}

QString extractString(const QVariant& variant)
{
    // Accept only QString or QDBusVariant containing QString, not conversions from other types
    if (variant.metaType() == QMetaType::fromType<QString>()) {
        return variant.toString();
    }
    if (variant.metaType() == QMetaType::fromType<QDBusVariant>()) {
        const QDBusVariant dbusVariant = variant.value<QDBusVariant>();
        if (dbusVariant.variant().metaType() == QMetaType::fromType<QString>()) {
            return dbusVariant.variant().toString();
        }
    }
    return QString();
}

QStringList extractStringList(const QVariant& variant)
{
    // Handle QDBusArgument for array of strings
    if (variant.metaType() == QMetaType::fromType<QDBusArgument>()) {
        const QDBusArgument arg = variant.value<QDBusArgument>();
        QStringList list;
        arg.beginArray();
        while (!arg.atEnd()) {
            QString str;
            arg >> str;
            list.append(str);
        }
        arg.endArray();
        return list;
    }
    // Handle direct QStringList - check actual type, not conversion
    if (variant.metaType() == QMetaType::fromType<QStringList>()) {
        return variant.toStringList();
    }
    return QStringList();
}

}  // namespace quantum::dbus::mpris
