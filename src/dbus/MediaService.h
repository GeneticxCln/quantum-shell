// The media player readout's connection to MPRIS: track, playback state and player identity from whichever
// player is active, kept current by PropertiesChanged events.
//
// MPRIS players announce themselves on the session bus with names like `org.mpris.MediaPlayer2.playerName`.
// This service watches for players appearing/disappearing and follows the most recently active one — the one
// that last changed its PlaybackStatus or Metadata. A track change, play/pause, or player switch arrives as
// a signal, so nothing here polls.
//
// Every discovered player is subscribed. The most recently active reading is published; before any
// activity, and when equally active players remain, service-name order breaks the tie.
//
// **Event-driven, no polling.** NameOwnerChanged signals player arrivals/departures, PropertiesChanged delivers
// metadata and playback status updates. Nothing is asked on a schedule.
//
// **Nothing blocks the GUI thread.** All D-Bus calls are asynchronous, properties arrive via queued signals.
#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <memory>

#include "dbus/MediaStatus.h"
#include "dbus/DbusProperties.h"

namespace quantum::dbus {

class MediaService : public QObject {
    Q_OBJECT

public:
    explicit MediaService(QObject* parent = nullptr);
    ~MediaService() override;

    // Whether the shell has a player and track to show: false when no MPRIS player exists or when
    // the active player has no track loaded.
    Q_PROPERTY(bool available READ available NOTIFY trackChanged)
    // Track title, artist, and which player it's from
    Q_PROPERTY(QString title READ title NOTIFY trackChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY trackChanged)
    Q_PROPERTY(QString playerName READ playerName NOTIFY trackChanged)
    // Playback state: "playing", "paused", "stopped", or empty if unknown
    Q_PROPERTY(QString playbackStatus READ playbackStatus NOTIFY trackChanged)

    bool available() const { return available_; }
    QString title() const { return reading_.title; }
    QString artist() const { return reading_.artist; }
    QString playerName() const { return reading_.playerName; }
    QString playbackStatus() const { return reading_.playbackStatus; }

    // Register this service as a QML singleton named "MediaService"
    // The name QML binds to, declared once and used by the registration, so a test can hold the spelling in
    // `qml/Media.qml` against it — the way the other services' names are held.
    inline static constexpr auto QmlTypeName = "MediaService";

    static void registerQmlSingleton(MediaService& service);

    // Transport for the player being followed — the one whose track and status `title` and `playbackStatus`
    // describe — as the MPRIS2 `org.mpris.MediaPlayer2.Player` methods `PlayPause`, `Next` and `Previous`. Each is
    // a request: the call is sent to that player's own bus name and what is drawn afterwards is the player's
    // answer arriving as a property change, never a state this service assumed. Each returns whether a request
    // was sent, which is false while there is no player (a control with nothing behind it does nothing rather than
    // pretending); a player that refuses or fails to answer is a record on `quantum.shell.media`.
    Q_INVOKABLE bool playPause();
    Q_INVOKABLE bool next();
    Q_INVOKABLE bool previous();

    // Start monitoring MPRIS players on the given bus. Called after the QML engine loads.
    void start(const QDBusConnection& bus);

signals:
    void trackChanged();

private slots:
    void onOwnerChanged(const QString& name, const QString& oldOwner, const QString& newOwner);
    void onPropertiesChanged(const QString& interface, const QVariantMap& changed,
                             const QStringList& invalidated, const QDBusMessage& message);
    void onBusDisconnected();

private:
    struct Player {
        QString owner;
        ObjectProperties properties;
        ObjectProperties identity;
        quint64 serial = 0;
        quint64 activity = 0;
        bool refreshProperties = false;
        bool refreshIdentity = false;
    };
    bool sendTransport(const char* method);
    QMap<QString, Player>::const_iterator followedPlayer() const;
    void detach();
    void adoptPlayer(const QString& name, const QString& owner);
    void fetchProperties(const QString& name, bool identity);
    void publish();
    void updateReading(const mpris::MediaReading& reading);

    QDBusConnection bus_{QString{}};
    QMap<QString, Player> players_;
    QMap<QString, quint64> ownerRevisions_;
    quint64 generation_ = 0;
    quint64 serial_ = 0;
    quint64 activity_ = 0;
    bool subscribed_ = false;
    mpris::MediaReading reading_;
    bool available_ = false;
};

}  // namespace quantum::dbus
