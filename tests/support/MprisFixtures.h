// The private session bus and the MPRIS player double the media service is driven against, shared by every
// binary that needs a player to exist without touching the desktop's: `media-test` (the service's own
// behaviour) and `control-center-test` (a click on the panel's transport reaching a player).
#pragma once

#include "dbus/MediaStatus.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QProcess>
#include <QStringList>
#include <QVariantMap>

#include <optional>

namespace mprisfix {

namespace constants = quantum::dbus::mpris::constants;

class PrivateMediaBus {
public:
    bool start() {
        process_.start(QStringLiteral("dbus-daemon"),
                       {QStringLiteral("--session"), QStringLiteral("--print-address=1"),
                        QStringLiteral("--nofork")});
        if (!process_.waitForStarted(5000) || !process_.waitForReadyRead(5000)) return false;
        address_ = QString::fromUtf8(process_.readLine()).trimmed();
        return !address_.isEmpty();
    }
    QDBusConnection connect(const QString& name) {
        connections_.append(name);
        return QDBusConnection::connectToBus(address_, name);
    }
    // Disconnects one of this bus's connections, which releases every name it owned: the player double on it
    // disappears from the bus, and a service following the bus sees the name go. The double holding the
    // connection has to be destroyed first, since a connection outlives none of its users.
    void drop(const QString& name) {
        connections_.removeAll(name);
        QDBusConnection::disconnectFromBus(name);
    }
    ~PrivateMediaBus() {
        for (const auto& name : connections_) QDBusConnection::disconnectFromBus(name);
        process_.terminate();
        if (!process_.waitForFinished(3000)) {
            process_.kill();
            process_.waitForFinished(3000);
        }
    }
private:
    QProcess process_;
    QString address_;
    QStringList connections_;
};

class FakeMprisPlayer : public QDBusVirtualObject {
public:
    explicit FakeMprisPlayer(QDBusConnection bus) : bus_(std::move(bus)) {}
    bool own(const QString& name) {
        return bus_.registerVirtualObject(QString::fromLatin1(constants::mprisPath), this)
            && bus_.registerService(name);
    }
    QString introspect(const QString&) const override { return {}; }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection&) override {
        if (message.interface() == QString::fromLatin1(constants::mprisPlayerInterface)) {
            // The transport methods, recorded by name: which player was asked, and what for.
            transportCalls.append(message.member());
            return bus_.send(refuseTransport
                                 ? message.createErrorReply(QStringLiteral("org.mpris.MediaPlayer2.Error.Refused"),
                                                            QStringLiteral("not now"))
                                 : message.createReply());
        }
        if (message.interface() != QStringLiteral("org.freedesktop.DBus.Properties")
            || message.member() != QStringLiteral("GetAll")) return false;
        ++calls;
        const bool identity = message.arguments().first().toString()
            == QString::fromLatin1(constants::mprisInterface);
        auto reply = message.createReply();
        reply << (identity ? QVariantMap{{QStringLiteral("Identity"), QStringLiteral("Initial player")}}
                           : properties);
        if (!identity && holdInitial) {
            held = reply;
            return true;
        }
        return bus_.send(reply);
    }
    bool announce(const QString& interface, const QVariantMap& changed) {
        auto signal = QDBusMessage::createSignal(QString::fromLatin1(constants::mprisPath),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"));
        signal << interface << changed << QStringList{};
        return bus_.send(signal);
    }
    bool release() {
        if (!held) return false;
        const bool sent = bus_.send(*held);
        held.reset();
        return sent;
    }
    QVariantMap properties;
    QStringList transportCalls;
    bool refuseTransport = false;
    std::optional<QDBusMessage> held;
    bool holdInitial = false;
    int calls = 0;
private:
    QDBusConnection bus_;
};

}  // namespace mprisfix
