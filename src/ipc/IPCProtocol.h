// The local IPC wire format: what a client says, what the shell answers, and the names both sides use.
//
// QUANTUM_SHELL.md § IPC fixes the transport and the shape: `QLocalServer`/`QLocalSocket`, the abstract
// socket name `\0quantum-shell`, newline-delimited JSON, one request line per one response line, and a
// `{"version": N}` handshake so a client can be told the shell speaks a different protocol instead of
// guessing. This file is where those decisions live, and two of them are worth stating here because they
// are the ones that go wrong quietly.
//
// **The name handed to Qt is not the name on the socket.** The abstract namespace's leading NUL is added
// by Qt when `AbstractNamespaceOption` is set, on both ends: passing the string with a NUL already in it
// binds a *different* address, which `ss -x` shows as `@@quantum-shell` rather than `@quantum-shell`. The
// two sockets coexist happily, which is exactly why a mistake here is not obvious — the shell listens,
// nothing connects, and both sides are "correct". The frozen name in AGENTS.md is the abstract address,
// `\0quantum-shell`; `SocketName` below is Qt's spelling of it, and the live test reads the bound name
// back out of the socket list rather than trusting either string.
//
// **Every frame carries the version.** The handshake is not a separate exchange that later frames may
// skip: a request without a verb and with a version *is* the handshake, answered like a `version`
// request, and a frame whose version is not this build's is refused with both versions named. So a client
// that only knows how to send one frame still finds out it is talking to the wrong shell.
//
// Nothing here holds state and nothing here touches a socket: decoding is a pure function of one line,
// which is what lets the whole format be tested — including its refusals — without a connection.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <array>
#include <string_view>

namespace quantum::ipc {

// The protocol revision. Bumped when a frame that exists today means something else, never when a verb is
// added: an old client asking for a verb this build does not have is refused by name.
inline constexpr int ProtocolVersion = 1;

// The socket name as Qt takes it. See the note above: Qt adds the abstract namespace's leading NUL for
// both `QLocalServer` and `QLocalSocket` when `AbstractNamespaceOption` is set, so the name here is the
// bare name and the address it binds is `\0quantum-shell`.
inline constexpr auto SocketName = "quantum-shell";

// One line longer than this is refused rather than buffered. A local client that never ends a line would
// otherwise be able to grow the shell's memory without bound, and no request this shell implements comes
// close to the limit.
inline constexpr qsizetype MaxLineBytes = 64 * 1024;

// The verbs, named once. AGENTS.md § Public interface is frozen without approval makes these public names:
// a rename is a breaking change for every script and external widget, so the list lives in one place and
// `ipc-protocol-test` mirrors it and fails to build when the two disagree.
//
// These are the names on the wire and the words a `qsctl` command line is written in, which is why the
// multi-word ones keep the space the design document gives them: `qsctl config get bar.height` sends the
// verb `config get`, so the CLI needs no mapping table between what a person types and what is sent.
namespace verb {

inline constexpr auto Version = "version";
inline constexpr auto State = "state";
inline constexpr auto ConfigGet = "config get";
inline constexpr auto BarToggle = "bar toggle";

// Every verb this shell implements. A verb exists only when a handler answers it and a test drives it
// (SYSTEM_PROMPT.md § Anti-Evasion Rules), so this list is the whole public surface rather than a plan.
inline constexpr std::array<const char*, 4> All{Version, State, ConfigGet, BarToggle};

}  // namespace verb

// One decoded request line.
struct Request {
    // The protocol version the client speaks. Zero until a frame has been decoded.
    int version = 0;

    // The verb, as written above. Empty never leaves `decodeRequest`: a frame carrying a version and no
    // verb is the handshake, and is decoded as a `version` request.
    QString verb;

    // `config get`'s key path, e.g. `bar.height`. Empty for every other verb.
    QString path;
};

// One response. `ok` is the machine-readable half — a script branches on it — and `error` is the half a
// person reads, so a refusal always says what was refused and what was expected instead. Every response
// carries `version`, the protocol revision of the shell that answered, which is what lets a client tell a
// refusal apart from a shell that speaks a different protocol.
//
// A successful response always carries `data`, an empty object when the verb has nothing to report, and a
// refusal always carries `error`: a client branches on `ok` without checking which keys are present.
struct Response {
    int version = 0;
    bool ok = false;
    QJsonObject data;
    QString error;
};

// Decodes one request line. Returns an empty string on success and a message naming the problem on
// failure; the message is what the client is sent, so it is written for a person. An unknown verb is not
// an error here: it decodes, and the server refuses it by name, which is a refusal a client can read
// rather than a protocol error it has to guess at.
QString decodeRequest(const QByteArray& line, Request* request);

// Encodes one request. The result has no trailing newline; the framing adds it.
QByteArray encodeRequest(const Request& request);

// Encodes one response, always carrying this build's protocol version so a client can tell a mismatch from
// a refusal.
QByteArray encodeResponse(const Response& response);

// Decodes one response line, for a client. Returns an empty string on success.
QString decodeResponse(const QByteArray& line, Response* response);

// Pulls every complete line out of `buffer`, leaving any partial tail in it. On a line longer than
// `MaxLineBytes` the buffer is cleared, `error` is set, and the lines read so far are returned: the caller
// answers what it can and then closes, rather than keeping a client's bytes forever.
QList<QByteArray> takeLines(QByteArray& buffer, QString* error);

}  // namespace quantum::ipc
