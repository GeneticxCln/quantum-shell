// The shell's local IPC server: the socket `qsctl` and every script talks to.
//
// QUANTUM_SHELL.md § IPC fixes the security property this class exists to keep: the interface exposes
// **only the `Shell.*` capabilities the QML API already exposes, never arbitrary C++ entry points**.
// That is why the server does not know what a workspace or a configuration key is. It decodes a frame,
// asks a `Capabilities` object, and encodes the answer; the object it asks is the same shell the QML
// binds to, and the verbs it can reach are the ones in `IPCProtocol.h` and nothing else. A new verb means
// a new method on that interface — a deliberate edit in two files, which is the point.
//
// Nothing here blocks: connections are read and answered from the event loop, a request is one line and a
// response is one line, and no handler waits on anything. A local client that stalls affects only its own
// connection.
//
// What is refused, and how:
//
//   * a line that is not a JSON object, or that has no version — answered and the connection closed,
//     because the client is not speaking this protocol and there is no later line that would help;
//   * a line whose version is not this build's — answered with both versions named and the connection
//     closed, so a client from a newer shell finds out instead of having its request half-understood;
//   * an unknown verb, a missing argument or an unknown configuration key — answered with a refusal that
//     names what was wrong and leaves the connection open, because a client that sends one bad request is
//     entitled to send a good one next;
//   * a line longer than `MaxLineBytes`, or a line that never ends — refused and closed rather than
//     buffered, so a client cannot grow the shell's memory by never sending a newline.
#pragma once

#include "ipc/IPCProtocol.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QObject>
#include <QString>

#include <optional>

class QLocalSocket;

namespace quantum::ipc {

// What the IPC can reach. Implemented by the shell (`ShellCapabilities`, which answers from `NiriService`,
// the configuration and the bar window) and by the tests, which is why the server takes this rather than
// references to those three: the framing and the refusals can be driven without a compositor.
class Capabilities {
public:
    virtual ~Capabilities();

    // The state a widget reads, as the same values and the same key names as `NiriService`.
    virtual QJsonObject state() const = 0;

    // One configuration value by key path, e.g. `bar.height`. No value means no such key, and the server
    // turns that into a refusal naming the path rather than into a null.
    virtual std::optional<QJsonValue> configValue(const QString& path) const = 0;

    // Hides or shows the bar and reports what it is now, so a caller does not have to toggle twice to find
    // out what it did.
    virtual bool toggleBar() = 0;
};

class IPCServer : public QObject {
    Q_OBJECT

public:
    // `capabilities` must outlive this object.
    //
    // The socket name is a parameter rather than the constant read directly, and the shell passes
    // `IPCProtocol.h`'s `SocketName` — which is the frozen name and is asserted against the bound socket by
    // the live test. The unit test passes a name of its own for a reason worth stating: the abstract
    // namespace is per user, not per process, so a test binding the shell's name would fail on the desktop
    // of anybody who has a shell running — which is exactly the person most likely to be running the tests.
    IPCServer(Capabilities& capabilities, const QString& socketName, QObject* parent = nullptr);

    // Listens on that name as an abstract socket. Returns false and sets `error` when the name is already
    // taken — which for an abstract socket means another instance is running, since there is no filesystem
    // entry to be stale — or when the platform refuses the socket.
    bool listen(QString* error = nullptr);

    bool isListening() const;

    // The name handed to Qt, which is the abstract address without its leading NUL.
    QString socketName() const;

private:
    // Defined in the .cpp: per-connection buffering and framing, which is state the header does not need
    // to describe. A nested class is a member and can reach the private members below.
    class Connection;

    Response dispatch(const Request& request);
    void handleConnection(QLocalSocket* socket);

    QLocalServer server_;
    Capabilities& capabilities_;
    QString socketName_;
};

}  // namespace quantum::ipc
