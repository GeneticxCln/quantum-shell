#include "dbus/MediaService.h"
#include "QmlModule.h"
#include "app/Logging.h"

#include <QDBusArgument>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QQmlEngine>

namespace quantum::dbus {
namespace {
namespace c = mpris::constants;
const QString propertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");
const QString busService = QStringLiteral("org.freedesktop.DBus");
const QString busPath = QStringLiteral("/org/freedesktop/DBus");
bool isPlayer(const QString& name) {
    return name.startsWith(QLatin1String(c::mprisPrefix) + QLatin1Char('.'));
}
QVariantMap metadataMap(QVariant value) {
    if (value.metaType() == QMetaType::fromType<QDBusVariant>())
        value = value.value<QDBusVariant>().variant();
    if (value.metaType() == QMetaType::fromType<QVariantMap>())
        return value.toMap();
    if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
        const auto argument = value.value<QDBusArgument>();
        if (argument.currentSignature() == QLatin1String("a{sv}"))
            return qdbus_cast<QVariantMap>(argument);
    }
    if (value.isValid())
        qCWarning(quantum::app::mediaLog) << "Refused non-dictionary MPRIS Metadata";
    return {};
}
}

MediaService::MediaService(QObject* parent) : QObject(parent) {}
MediaService::~MediaService() { detach(); }

void MediaService::registerQmlSingleton(MediaService& service) {
    qmlRegisterSingletonInstance(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion,
                                quantum::qml::ModuleMinorVersion, "MediaService", &service);
}

void MediaService::detach() {
    ++generation_;
    if (subscribed_) {
        const bool owners = bus_.disconnect(busService, busPath, busService, QStringLiteral("NameOwnerChanged"),
                                            this, SLOT(onOwnerChanged(QString,QString,QString)));
        const bool properties = bus_.disconnect(QString(), QLatin1String(c::mprisPath), propertiesInterface,
            QStringLiteral("PropertiesChanged"), this,
            SLOT(onPropertiesChanged(QString,QVariantMap,QStringList,QDBusMessage)));
        const bool disconnected = bus_.disconnect(QString(), QStringLiteral("/org/freedesktop/DBus/Local"),
            QStringLiteral("org.freedesktop.DBus.Local"), QStringLiteral("Disconnected"), this,
            SLOT(onBusDisconnected()));
        if ((!owners || !properties || !disconnected) && bus_.isConnected())
            qCWarning(quantum::app::mediaLog) << "Failed to remove media bus subscriptions";
    }
    subscribed_ = false;
    players_.clear();
    ownerRevisions_.clear();
    activity_ = 0;
}

void MediaService::start(const QDBusConnection& bus) {
    detach();
    updateReading({});
    bus_ = bus;
    if (!bus_.isConnected()) {
        qCWarning(quantum::app::mediaLog) << "Cannot monitor media on a disconnected bus";
        return;
    }
    // Subscribe before ListNames/GetNameOwner; owner revisions prevent a late discovery reply
    // from resurrecting a departed or replaced player. The sender of each property signal is
    // checked against these unique owners, never against a mutable well-known name.
    const bool owners = bus_.connect(busService, busPath, busService, QStringLiteral("NameOwnerChanged"),
                                    this, SLOT(onOwnerChanged(QString,QString,QString)));
    const bool properties = bus_.connect(QString(), QLatin1String(c::mprisPath), propertiesInterface,
        QStringLiteral("PropertiesChanged"), this,
        SLOT(onPropertiesChanged(QString,QVariantMap,QStringList,QDBusMessage)));
    const bool disconnected = bus_.connect(QString(), QStringLiteral("/org/freedesktop/DBus/Local"),
        QStringLiteral("org.freedesktop.DBus.Local"), QStringLiteral("Disconnected"), this,
        SLOT(onBusDisconnected()));
    subscribed_ = true;
    if (!owners || !properties || !disconnected) {
        qCWarning(quantum::app::mediaLog) << "Failed to subscribe to media bus events";
        detach();
        return;
    }
    auto request = QDBusMessage::createMethodCall(busService, busPath, busService, QStringLiteral("ListNames"));
    auto* pending = new QDBusPendingCallWatcher(bus_.asyncCall(request), this);
    const auto generation = generation_;
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher* call) {
        QDBusPendingReply<QStringList> reply = *call;
        call->deleteLater();
        if (generation != generation_) return;
        if (reply.isError()) {
            qCWarning(quantum::app::mediaLog) << "Media discovery failed:" << reply.error().message();
            return;
        }
        for (const auto& name : reply.value()) {
            if (!isPlayer(name) || ownerRevisions_.contains(name)) continue;
            auto query = QDBusMessage::createMethodCall(busService, busPath, busService, QStringLiteral("GetNameOwner"));
            query << name;
            auto* ownerCall = new QDBusPendingCallWatcher(bus_.asyncCall(query), this);
            connect(ownerCall, &QDBusPendingCallWatcher::finished, this,
                [this, generation, name](QDBusPendingCallWatcher* ownerPending) {
                    QDBusPendingReply<QString> ownerReply = *ownerPending;
                    ownerPending->deleteLater();
                    if (generation != generation_ || ownerRevisions_.contains(name)) return;
                    if (ownerReply.isError()) {
                        qCWarning(quantum::app::mediaLog) << "Media owner lookup failed:" << name << ownerReply.error().message();
                        return;
                    }
                    adoptPlayer(name, ownerReply.value());
                });
        }
    });
}

void MediaService::onBusDisconnected() {
    qCWarning(quantum::app::mediaLog) << "Media session bus disconnected";
    detach();
    updateReading({});
}

void MediaService::onOwnerChanged(const QString& name, const QString&, const QString& owner) {
    if (!isPlayer(name)) return;
    ownerRevisions_[name] = ++serial_;
    players_.remove(name);
    if (!owner.isEmpty()) adoptPlayer(name, owner);
    publish();
}

void MediaService::adoptPlayer(const QString& name, const QString& owner) {
    if (players_.contains(name) && players_.value(name).owner == owner) return;
    Player player;
    player.owner = owner;
    player.serial = ++serial_;
    players_.insert(name, std::move(player));
    fetchProperties(name, false);
    fetchProperties(name, true);
}

void MediaService::fetchProperties(const QString& name, bool identity) {
    auto it = players_.find(name);
    if (it == players_.end()) return;
    auto& properties = identity ? it->identity : it->properties;
    auto& refresh = identity ? it->refreshIdentity : it->refreshProperties;
    if (properties.isReading()) {
        refresh = true;
        return;
    }
    refresh = false;
    properties.beginRead();
    auto query = QDBusMessage::createMethodCall(it->owner, QLatin1String(c::mprisPath), propertiesInterface,
                                               QStringLiteral("GetAll"));
    query << QString::fromLatin1(identity ? c::mprisInterface : c::mprisPlayerInterface);
    const auto serial = it->serial;
    const auto generation = generation_;
    auto* pending = new QDBusPendingCallWatcher(bus_.asyncCall(query), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this,
        [this, name, serial, generation, identity](QDBusPendingCallWatcher* call) {
            QDBusPendingReply<QVariantMap> reply = *call;
            call->deleteLater();
            auto player = players_.find(name);
            if (generation != generation_ || player == players_.end() || player->serial != serial) return;
            auto& result = identity ? player->identity : player->properties;
            if (reply.isError()) {
                result.abortRead();
                qCWarning(quantum::app::mediaLog) << "Media GetAll failed:" << name << reply.error().message();
            } else {
                result.adoptAll(reply.value());
            }
            const bool refreshAgain = identity ? player->refreshIdentity : player->refreshProperties;
            publish();
            if (refreshAgain) fetchProperties(name, identity);
        });
}

void MediaService::onPropertiesChanged(const QString& interface, const QVariantMap& changed,
                                      const QStringList& invalidated, const QDBusMessage& message) {
    const bool identity = interface == QLatin1String(c::mprisInterface);
    if (!identity && interface != QLatin1String(c::mprisPlayerInterface)) return;
    for (auto it = players_.begin(); it != players_.end(); ++it) {
        if (it->owner != message.service()) continue;
        const auto metadata = QLatin1String(c::propertyMetadata);
        const auto status = QLatin1String(c::propertyPlaybackStatus);
        const bool relevant = !identity && (changed.contains(metadata) || changed.contains(status)
            || invalidated.contains(metadata) || invalidated.contains(status));
        if (relevant) it->activity = ++activity_;
        auto& properties = identity ? it->identity : it->properties;
        properties.applyChange(changed, invalidated);
        if (!invalidated.isEmpty()) fetchProperties(it.key(), identity);
    }
    publish();
}

void MediaService::publish() {
    auto selected = players_.cend();
    for (auto it = players_.cbegin(); it != players_.cend(); ++it) {
        if (selected == players_.cend() || it->activity > selected->activity) selected = it;
    }
    mpris::MediaReading reading;
    if (selected != players_.cend()) {
        reading = mpris::parseMetadata(metadataMap(selected->properties.values().value(QLatin1String(c::propertyMetadata))));
        reading.playbackStatus = mpris::normalizePlaybackStatus(mpris::extractString(
            selected->properties.values().value(QLatin1String(c::propertyPlaybackStatus))));
        reading.playerName = mpris::extractString(selected->identity.values().value(QLatin1String(c::propertyIdentity)));
    }
    updateReading(reading);
}

void MediaService::updateReading(const mpris::MediaReading& reading) {
    if (reading == reading_) return;
    reading_ = reading;
    available_ = reading.hasTrack;
    emit trackChanged();
}
} // namespace quantum::dbus
