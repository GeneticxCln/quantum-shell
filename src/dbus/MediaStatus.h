// MPRIS media player protocol as the bar reads it: the track, the playback state and the player's identity,
// unwrapped and decided as pure functions.
//
// The bar follows org.mpris.MediaPlayer2.Player on whichever player is active, subscribing to PropertiesChanged
// so a track change, a play/pause and a player switch each arrive as a signal rather than being asked for. This
// file is the protocol's names and the decisions made from them — what the daemon sent and what it means — stated
// as functions of their inputs so every shape MPRIS can produce is a case the service never has to arrange.
//
// MPRIS names verified against the running chromium player above with `busctl --user`, the spec's own description
// at https://specifications.freedesktop.org/mpris-spec/2.2/, and against what playerctl 2.4.1 shows:
//
//   * `PlaybackStatus` — string, one of "Playing", "Paused", "Stopped"
//   * `Metadata` — dict, whose `xesam:title` and `xesam:artist` (string array) are the track
//   * `Identity` on org.mpris.MediaPlayer2 — the player's name
//
// The D-Bus constants below are declared once and mirrored at compile time by media-test.
#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

namespace quantum::dbus::mpris {

// MPRIS D-Bus names, declared once. A wrong name fails by drawing nothing.
namespace constants {
constexpr auto mprisPrefix = "org.mpris.MediaPlayer2";
constexpr auto mprisPath = "/org/mpris/MediaPlayer2";
constexpr auto mprisInterface = "org.mpris.MediaPlayer2";
constexpr auto mprisPlayerInterface = "org.mpris.MediaPlayer2.Player";

constexpr auto propertyPlaybackStatus = "PlaybackStatus";
constexpr auto propertyMetadata = "Metadata";
constexpr auto propertyIdentity = "Identity";

constexpr auto metadataTitle = "xesam:title";
constexpr auto metadataArtist = "xesam:artist";
constexpr auto metadataAlbum = "xesam:album";

// PlaybackStatus tokens MPRIS defines
constexpr auto statusPlaying = "Playing";
constexpr auto statusPaused = "Paused";
constexpr auto statusStopped = "Stopped";
}  // namespace constants

// What the bar draws: track metadata and playback state.
struct MediaReading {
    QString title;
    QString artist;
    QString playerName;
    QString playbackStatus;  // "playing", "paused", "stopped", or empty for unknown
    bool hasTrack{false};    // Whether there is a track to show

    bool operator==(const MediaReading& other) const = default;
};

// Unwrap MPRIS Metadata dict to title and artist.
// Returns empty strings if fields are missing or wrong type.
MediaReading parseMetadata(const QVariantMap& metadata);

// Map MPRIS PlaybackStatus string to lowercase token.
// "Playing" -> "playing", "Paused" -> "paused", "Stopped" -> "stopped"
// Anything else (including empty) -> empty string
QString normalizePlaybackStatus(const QString& status);

// Extract string from D-Bus variant, empty if wrong type.
QString extractString(const QVariant& variant);

// Extract string array from D-Bus variant (for xesam:artist), empty if wrong type.
QStringList extractStringList(const QVariant& variant);

}  // namespace quantum::dbus::mpris
