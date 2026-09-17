// MPRIS parsing and service regressions. Service cases use a private bus and explicit player doubles;
// no desktop player or session bus is touched.
#include "dbus/MediaService.h"
#include "dbus/MediaStatus.h"

#include <QTest>
#include <QDBusVirtualObject>
#include <QProcess>
#include <optional>

namespace constants = quantum::dbus::mpris::constants;
using quantum::dbus::mpris::extractString;
using quantum::dbus::mpris::extractStringList;
using quantum::dbus::mpris::MediaReading;
using quantum::dbus::mpris::normalizePlaybackStatus;
using quantum::dbus::mpris::parseMetadata;

namespace {

// The QML name this service is registered under, mirrored here for the reason battery_test mirrors its own:
// a rename in src/dbus/ does not build until this file agrees, and qml/Media.qml is the other place the name
// is written.
constexpr auto coveredServiceTypeName = "MediaService";

// The MPRIS D-Bus names this module speaks, mirrored here for the reason the QML type name is above: they are
// declared once in MediaStatus.h and checked here at compile time, so a wrong name fails a build rather than
// drawing nothing.
constexpr std::string_view coveredMprisPrefix = "org.mpris.MediaPlayer2";
constexpr std::string_view coveredMprisPath = "/org/mpris/MediaPlayer2";
constexpr std::string_view coveredMprisInterface = "org.mpris.MediaPlayer2";
constexpr std::string_view coveredMprisPlayerInterface = "org.mpris.MediaPlayer2.Player";
constexpr std::string_view coveredPropertyPlaybackStatus = "PlaybackStatus";
constexpr std::string_view coveredPropertyMetadata = "Metadata";
constexpr std::string_view coveredPropertyIdentity = "Identity";
constexpr std::string_view coveredMetadataTitle = "xesam:title";
constexpr std::string_view coveredMetadataArtist = "xesam:artist";
constexpr std::string_view coveredMetadataAlbum = "xesam:album";
constexpr std::string_view coveredStatusPlaying = "Playing";
constexpr std::string_view coveredStatusPaused = "Paused";
constexpr std::string_view coveredStatusStopped = "Stopped";

// The normalized status tokens the widget switches on, mirrored here to check at runtime.
constexpr std::string_view normalizedStatusPlaying = "playing";
constexpr std::string_view normalizedStatusPaused = "paused";
constexpr std::string_view normalizedStatusStopped = "stopped";


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
    std::optional<QDBusMessage> held;
    bool holdInitial = false;
    int calls = 0;
private:
    QDBusConnection bus_;
};
}  // namespace

class MediaTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void mprisNamesMatchCoveredValues();
    void playbackStatusIsNormalized();
    void unknownPlaybackStatusReturnsEmpty();
    void normalizedStatusesMatchCoveredValues();
    void extractsStringFromVariant();
    void extractsStringListFromVariant();
    void parsesMetadataWithTitleAndArtist();
    void parsesMetadataWithTitleOnly();
    void parsesMetadataWithArtistOnly();
    void parsesMetadataWithMultipleArtists();
    void emptyMetadataHasNoTrack();
    void metadataWithWrongTypesReturnsEmpty();
    void aSecondPlayersSignalSelectsItsReading();
    void aNewerSignalSurvivesTheInitialReply();
};

void MediaTest::mprisNamesMatchCoveredValues()
{
    // The constants declared in MediaStatus.h must match what this test covers, so a rename in the module fails
    // a build rather than silently breaking the D-Bus connection.
    QCOMPARE(std::string_view(constants::mprisPrefix), coveredMprisPrefix);
    QCOMPARE(std::string_view(constants::mprisPath), coveredMprisPath);
    QCOMPARE(std::string_view(constants::mprisInterface), coveredMprisInterface);
    QCOMPARE(std::string_view(constants::mprisPlayerInterface), coveredMprisPlayerInterface);
    QCOMPARE(std::string_view(constants::propertyPlaybackStatus), coveredPropertyPlaybackStatus);
    QCOMPARE(std::string_view(constants::propertyMetadata), coveredPropertyMetadata);
    QCOMPARE(std::string_view(constants::propertyIdentity), coveredPropertyIdentity);
    QCOMPARE(std::string_view(constants::metadataTitle), coveredMetadataTitle);
    QCOMPARE(std::string_view(constants::metadataArtist), coveredMetadataArtist);
    QCOMPARE(std::string_view(constants::metadataAlbum), coveredMetadataAlbum);
    QCOMPARE(std::string_view(constants::statusPlaying), coveredStatusPlaying);
    QCOMPARE(std::string_view(constants::statusPaused), coveredStatusPaused);
    QCOMPARE(std::string_view(constants::statusStopped), coveredStatusStopped);
}

void MediaTest::playbackStatusIsNormalized()
{
    // MPRIS defines three statuses with initial caps; the widget switches on lowercase.
    QCOMPARE(normalizePlaybackStatus(QStringLiteral("Playing")), QStringLiteral("playing"));
    QCOMPARE(normalizePlaybackStatus(QStringLiteral("Paused")), QStringLiteral("paused"));
    QCOMPARE(normalizePlaybackStatus(QStringLiteral("Stopped")), QStringLiteral("stopped"));
}

void MediaTest::unknownPlaybackStatusReturnsEmpty()
{
    // Anything not a known MPRIS status returns empty string.
    QVERIFY(normalizePlaybackStatus(QStringLiteral("Unknown")).isEmpty());
    QVERIFY(normalizePlaybackStatus(QStringLiteral("playing")).isEmpty());  // lowercase not recognized
    QVERIFY(normalizePlaybackStatus(QString()).isEmpty());
    QVERIFY(normalizePlaybackStatus(QStringLiteral("Buffering")).isEmpty());
}

void MediaTest::normalizedStatusesMatchCoveredValues()
{
    // The normalized tokens the widget switches on must match what this test covers.
    QCOMPARE(normalizePlaybackStatus(QStringLiteral("Playing")), QString::fromUtf8(normalizedStatusPlaying.data()));
    QCOMPARE(normalizePlaybackStatus(QStringLiteral("Paused")), QString::fromUtf8(normalizedStatusPaused.data()));
    QCOMPARE(normalizePlaybackStatus(QStringLiteral("Stopped")), QString::fromUtf8(normalizedStatusStopped.data()));
}

void MediaTest::extractsStringFromVariant()
{
    // QString variants are extracted directly.
    QCOMPARE(extractString(QVariant(QStringLiteral("test"))), QStringLiteral("test"));
    QCOMPARE(extractString(QVariant(QStringLiteral(""))), QStringLiteral(""));
    
    // Wrong types return empty.
    QVERIFY(extractString(QVariant(42)).isEmpty());
    QVERIFY(extractString(QVariant()).isEmpty());
}

void MediaTest::extractsStringListFromVariant()
{
    // QStringList variants are extracted directly.
    const QStringList list{QStringLiteral("Artist 1"), QStringLiteral("Artist 2")};
    QCOMPARE(extractStringList(QVariant(list)), list);
    
    // Empty list is still valid.
    QCOMPARE(extractStringList(QVariant(QStringList())), QStringList());
    
    // Wrong types return empty list.
    QVERIFY(extractStringList(QVariant(42)).isEmpty());
    QVERIFY(extractStringList(QVariant()).isEmpty());
}

void MediaTest::parsesMetadataWithTitleAndArtist()
{
    QVariantMap metadata;
    metadata[constants::metadataTitle] = QStringLiteral("Test Track");
    metadata[constants::metadataArtist] = QVariant::fromValue(QStringList{QStringLiteral("Test Artist")});
    
    const MediaReading reading = parseMetadata(metadata);
    
    QCOMPARE(reading.title, QStringLiteral("Test Track"));
    QCOMPARE(reading.artist, QStringLiteral("Test Artist"));
    QVERIFY(reading.hasTrack);
}

void MediaTest::parsesMetadataWithTitleOnly()
{
    QVariantMap metadata;
    metadata[constants::metadataTitle] = QStringLiteral("Title Only");
    
    const MediaReading reading = parseMetadata(metadata);
    
    QCOMPARE(reading.title, QStringLiteral("Title Only"));
    QVERIFY(reading.artist.isEmpty());
    QVERIFY(reading.hasTrack);  // Title alone is enough for hasTrack
}

void MediaTest::parsesMetadataWithArtistOnly()
{
    QVariantMap metadata;
    metadata[constants::metadataArtist] = QVariant::fromValue(QStringList{QStringLiteral("Artist Only")});
    
    const MediaReading reading = parseMetadata(metadata);
    
    QVERIFY(reading.title.isEmpty());
    QCOMPARE(reading.artist, QStringLiteral("Artist Only"));
    QVERIFY(reading.hasTrack);  // Artist alone is enough for hasTrack
}

void MediaTest::parsesMetadataWithMultipleArtists()
{
    QVariantMap metadata;
    metadata[constants::metadataTitle] = QStringLiteral("Collaboration");
    metadata[constants::metadataArtist] =
        QVariant::fromValue(QStringList{QStringLiteral("Artist 1"), QStringLiteral("Artist 2"),
                                         QStringLiteral("Artist 3")});
    
    const MediaReading reading = parseMetadata(metadata);
    
    QCOMPARE(reading.title, QStringLiteral("Collaboration"));
    // Multiple artists joined with ", "
    QCOMPARE(reading.artist, QStringLiteral("Artist 1, Artist 2, Artist 3"));
    QVERIFY(reading.hasTrack);
}

void MediaTest::emptyMetadataHasNoTrack()
{
    const MediaReading reading = parseMetadata(QVariantMap());
    
    QVERIFY(reading.title.isEmpty());
    QVERIFY(reading.artist.isEmpty());
    QVERIFY(!reading.hasTrack);  // No title or artist means no track
}

void MediaTest::metadataWithWrongTypesReturnsEmpty()
{
    QVariantMap metadata;
    // Title as int, artist as string (should be list)
    metadata[constants::metadataTitle] = 42;
    metadata[constants::metadataArtist] = QStringLiteral("not a list");
    
    const MediaReading reading = parseMetadata(metadata);
    
    // Both should be empty due to type mismatch
    QVERIFY(reading.title.isEmpty());
    QVERIFY(reading.artist.isEmpty());
    QVERIFY(!reading.hasTrack);
}

void MediaTest::aSecondPlayersSignalSelectsItsReading()
{
    PrivateMediaBus bus;
    QVERIFY(bus.start());
    FakeMprisPlayer first(bus.connect(QStringLiteral("media-first")));
    FakeMprisPlayer second(bus.connect(QStringLiteral("media-second")));
    first.properties = {{QStringLiteral("Metadata"), QVariantMap{{QStringLiteral("xesam:title"), QStringLiteral("First track")}}}};
    second.properties = {{QStringLiteral("Metadata"), QVariantMap{{QStringLiteral("xesam:title"), QStringLiteral("Second track")}}}};
    QVERIFY(first.own(QStringLiteral("org.mpris.MediaPlayer2.alpha")));
    QVERIFY(second.own(QStringLiteral("org.mpris.MediaPlayer2.beta")));
    quantum::dbus::MediaService service;
    service.start(bus.connect(QStringLiteral("media-reader")));
    QTRY_COMPARE(service.title(), QStringLiteral("First track"));
    QTRY_COMPARE(first.calls, 2);
    QTRY_COMPARE(second.calls, 2);
    QVERIFY(second.announce(QStringLiteral("org.mpris.MediaPlayer2.Player"),
                            {{QStringLiteral("PlaybackStatus"), QStringLiteral("Playing")}}));
    QTRY_COMPARE(service.title(), QStringLiteral("Second track"));
    QTRY_COMPARE(service.playbackStatus(), QStringLiteral("playing"));
    QVERIFY(service.available());
    QCOMPARE(first.calls, 2);
    QCOMPARE(second.calls, 2);
}

void MediaTest::aNewerSignalSurvivesTheInitialReply()
{
    PrivateMediaBus bus;
    QVERIFY(bus.start());
    FakeMprisPlayer player(bus.connect(QStringLiteral("media-delayed")));
    player.holdInitial = true;
    player.properties = {
        {QStringLiteral("Metadata"), QVariantMap{{QStringLiteral("xesam:title"), QStringLiteral("Old track")}}},
        {QStringLiteral("PlaybackStatus"), QStringLiteral("Paused")}};
    QVERIFY(player.own(QStringLiteral("org.mpris.MediaPlayer2.delayed")));
    quantum::dbus::MediaService service;
    service.start(bus.connect(QStringLiteral("media-reader")));
    QTRY_VERIFY(player.held.has_value());
    QTRY_COMPARE(service.playerName(), QStringLiteral("Initial player"));
    QVERIFY(player.announce(QStringLiteral("org.mpris.MediaPlayer2.Player"),
        {{QStringLiteral("Metadata"), QVariantMap{{QStringLiteral("xesam:title"), QStringLiteral("New track")}}}}));
    // Same sender and ordered connection: observing Identity proves the preceding metadata signal
    // reached the service before releasing its initial reply, even while that metadata is buffered.
    QVERIFY(player.announce(QStringLiteral("org.mpris.MediaPlayer2"),
                            {{QStringLiteral("Identity"), QStringLiteral("Signal barrier")}}));
    QTRY_COMPARE(service.playerName(), QStringLiteral("Signal barrier"));
    QVERIFY(player.release());
    // PlaybackStatus exists only in the held reply, so this proves it was consumed rather than ignored.
    QTRY_COMPARE(service.playbackStatus(), QStringLiteral("paused"));
    QCOMPARE(service.title(), QStringLiteral("New track"));
    QVERIFY(service.available());
    QCOMPARE(player.calls, 2);
}

QTEST_MAIN(MediaTest)
#include "media_test.moc"
