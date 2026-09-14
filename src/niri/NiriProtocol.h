// The wire format both niri connections speak, in one place.
//
// A request is one JSON value and a newline; a reply is one JSON value and a newline, either
// `{"Ok": <value>}` or `{"Err": "<message>"}`; an event is one object per line whose single key names
// the event and whose value carries its fields. Verified against a running niri 26.04, whose types
// come from niri-ipc v26.04.
//
// Both NiriIPC (the request connection) and NiriEventStream (the event connection) decode through
// here, so the two cannot drift apart on quoting, newline handling or error text.
#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <array>
#include <optional>

namespace quantum::niri {

// The `$NIRI_SOCKET` path niri names for this process, empty outside a niri session. niri's own
// socket helper documents no other mechanism, so nothing here guesses a path.
QString niriSocketPath();

// Where niri puts its IPC sockets. niri's `socket_dir()` (src/ipc/server.rs) is the runtime directory,
// and the temp directory when there is none; this mirrors both, so a shell looks in the same place niri
// would have written.
QString niriSocketDir();

// Every niri IPC socket in `directory` (default: niriSocketDir()), newest first.
//
// niri names its socket `niri.<wayland socket name>.<pid>.sock` (verified in niri v26.04's
// `IpcServer::start`, src/ipc/server.rs), and unlinks it when it exits cleanly. Both halves matter to a
// shell that outlives the compositor: the pid in the name means a restarted niri's socket has a
// different path from the one before it, and the whole file is gone in between. Names that are not
// that shape are left out rather than guessed at.
QStringList niriSocketFiles(const QString& directory = QString());

// The sockets for a compositor whose Wayland socket is named `waylandName` — `wayland-1` for
// `$WAYLAND_DISPLAY=wayland-1` — newest first, because niri's IPC socket is named after the Wayland
// socket the same compositor serves.
QStringList niriSocketFor(const QString& waylandName, const QString& directory = QString());

// The socket to attach to now: `$NIRI_SOCKET` while that path still exists, otherwise the compositor's
// socket rediscovered for this session's `$WAYLAND_DISPLAY`. Empty when there is none, which is the
// honest answer to "is this process inside a niri session" rather than a path to keep trying.
//
// The environment variable is trusted only while the path exists on purpose: a shell that outlives a
// compositor restart still holds the value it inherited at login, and that socket was unlinked by the
// niri that has since exited.
QString compositorSocket();

// A line larger than this means the stream is not the IPC protocol — the largest reply on this
// machine, Outputs, is about 3 KB — so the buffer is dropped instead of growing without bound.
inline constexpr int maximumLineBytes = 4 * 1024 * 1024;

// What niri 26.04 answers a request it does not know with. Verified on the wire for both a request
// name that does not exist and a line that is not JSON at all.
inline constexpr QLatin1StringView unknownRequestError{"error parsing request"};

// One line of a reply, decoded.
struct Reply {
    enum class Kind {
        ok,              // {"Ok": ...}
        error,           // {"Err": "..."} — niri refused the request, or it failed
        transportError,  // the reply could not be read or parsed at all
    };

    Kind kind = Kind::transportError;
    QJsonValue value;  // ok: the response value, e.g. {"Version": "26.04 (8ed0da4)"}
    QString text;      // error and transportError: what went wrong

    bool isOk() const;
    // The payload of the named response variant, undefined when the reply is not an Ok reply
    // carrying that variant. A variant whose payload is JSON null is present, not undefined.
    QJsonValue variant(QStringView expectedVariant) const;
    QString describe() const;
};

// Splits complete lines out of a read buffer, leaving a partial line behind, and drops empty lines so
// a stray newline cannot shift which request a reply belongs to. Replies can arrive split across
// reads, so the connections only ever parse lines this returned.
QList<QByteArray> takeCompleteLines(QByteArray& buffer);

// Decodes one reply line.
Reply decodeReplyLine(const QByteArray& line);

// The reason a read buffer has to be dropped, or an empty string while it is within bounds. A peer
// that never sends a line break is not speaking this protocol, and the buffer must not grow without
// limit while it does — one bound, in one place, for both connections.
QString oversizedBufferReason(const QByteArray& buffer);

// One event line, split into its name and its fields.
struct EventLine {
    QString name;       // the event variant name, e.g. "WorkspacesChanged"
    QJsonValue fields;  // the variant's value: an object for every event niri-ipc v26.04 defines

    bool isValid() const;
};

// Decodes one event line. An invalid result means the line was not a single-key object, so it is a
// protocol problem rather than a variant this build does not know.
EventLine decodeEventLine(const QByteArray& line);

// True for the names in niri-ipc v26.04's `Event` enum. The distinction matters: an event that is in
// this list but not handled yet is a known gap, while a name that is not is the compositor telling us
// the protocol moved — and those two must not be reported as the same thing.
bool isKnownEventName(QStringView name);

// Every name niri-ipc v26.04 defines, for the test that pins this table to the protocol.
QStringList knownEventNames();

// Reads a JSON value that should be an id. Absent, null or negative gives no value: niri documents
// that ids need not be small, need not start at 1 and may be generated at random, so no id here is
// ever treated as "none".
std::optional<quint64> idFromJson(const QJsonValue& value);

}  // namespace quantum::niri
