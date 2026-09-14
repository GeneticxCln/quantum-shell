#include "ipc/IPCServer.h"

#include "app/Logging.h"

#include <QCoreApplication>
#include <QDebug>
#include <QLocalSocket>
#include <QStringList>

namespace quantum::ipc {
namespace {

Response refusal(const QString& reason)
{
    Response response;
    response.ok = false;
    response.error = reason;
    return response;
}

QString implementedVerbs()
{
    QStringList names;
    for (const char* name : verb::All)
        names.append(QString::fromLatin1(name));
    return names.join(QStringLiteral(", "));
}

}  // namespace

Capabilities::~Capabilities() = default;

// One client connection: a buffer, and the frames taken out of it.
//
// A nested class rather than a file-local one so it can call `dispatch`, and parented to its socket so
// the two live and die together: nothing has to remember to clean up a connection.
class IPCServer::Connection : public QObject
{
public:
    Connection(QLocalSocket* socket, IPCServer& server)
        : QObject(socket)
        , socket_(socket)
        , server_(server)
    {
        connect(socket_, &QLocalSocket::readyRead, this, &Connection::onReadyRead);
        connect(socket_, &QLocalSocket::disconnected, this, &QObject::deleteLater);

        // No initial read of whatever may already be in the buffer, and that is a measured decision rather
        // than an omission: a client can put its first frame on the wire before this side has processed the
        // accept — a raw socket certainly can — and `readyRead` still delivers it. Qt emits the signal for a
        // socket whose buffer already holds data when the notifier is watched, so an initial drain would be
        // code no test could make a difference to. `ipc-server-test` has a slot that writes before anything
        // processes events precisely so that this stays true rather than being assumed: it passes today, and
        // it is what would fail if a later Qt stopped delivering that first frame.
    }

private:
    void onReadyRead();
    void answer(const Response& response, bool closeAfter);

    QLocalSocket* socket_;
    IPCServer& server_;
    QByteArray buffer_;
};

void IPCServer::Connection::onReadyRead()
{
    buffer_.append(socket_->readAll());

    QString framingError;
    const QList<QByteArray> lines = takeLines(buffer_, &framingError);

    for (const QByteArray& line : lines) {
        Request request;
        const QString decodeError = decodeRequest(line, &request);
        if (!decodeError.isEmpty()) {
            // The client is not speaking this protocol. Closing is what tells it that: there is no later
            // line that would make this one mean something.
            answer(refusal(decodeError), true);
            return;
        }

        if (request.version != ProtocolVersion) {
            // Both versions are named because the response carries this build's version too, so a client
            // can report "the shell speaks 2, I speak 1" instead of retrying forever.
            answer(refusal(QStringLiteral("this shell speaks protocol %1; that request says %2")
                               .arg(ProtocolVersion)
                               .arg(request.version)),
                   true);
            return;
        }

        answer(server_.dispatch(request), false);
    }

    if (!framingError.isEmpty())
        answer(refusal(framingError), true);
}

void IPCServer::Connection::answer(const Response& response, bool closeAfter)
{
    QByteArray line = encodeResponse(response);
    line.append('\n');
    socket_->write(line);
    // Flushed rather than left to the event loop: this connection may be closed on the next line, and a
    // refusal that never arrived because its socket was torn down first is worse than no refusal at all.
    socket_->flush();
    // Refusals are recorded, and answers are not: the client already has the refusal, and a person trying to
    // work out why a script did not do what they expected has only half the story without the shell's side of
    // it. An answer is not news.
    if (!response.ok)
        qCWarning(quantum::app::ipcLog) << "refused:" << response.error;

    if (closeAfter)
        socket_->disconnectFromServer();
}

IPCServer::IPCServer(Capabilities& capabilities, const QString& socketName, QObject* parent)
    : QObject(parent)
    , capabilities_(capabilities)
    , socketName_(socketName)
{
    // Drained in a loop: `newConnection` is emitted once per connection Qt made pending, and a client that
    // connects while a previous one is being answered leaves more than one waiting.
    connect(&server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = server_.nextPendingConnection())
            handleConnection(socket);
    });
}

bool IPCServer::listen(QString* error)
{
    // The abstract namespace option is what makes this a name rather than a path, and it is on both ends:
    // see the note at the top of IPCProtocol.h for what happens when only one end has it.
    server_.setSocketOptions(QLocalServer::AbstractNamespaceOption);
    if (!server_.listen(socketName_)) {
        if (error != nullptr)
            *error = server_.errorString();
        return false;
    }
    return true;
}

bool IPCServer::isListening() const
{
    return server_.isListening();
}

QString IPCServer::socketName() const
{
    return socketName_;
}

Response IPCServer::dispatch(const Request& request)
{
    Response response;
    response.ok = true;

    if (request.verb == QLatin1StringView(verb::Version)) {
        response.data.insert(QStringLiteral("name"), QCoreApplication::applicationName());
        response.data.insert(QStringLiteral("shell"), QCoreApplication::applicationVersion());
        response.data.insert(QStringLiteral("protocol"), ProtocolVersion);
        return response;
    }

    if (request.verb == QLatin1StringView(verb::State)) {
        response.data = capabilities_.state();
        return response;
    }

    if (request.verb == QLatin1StringView(verb::ConfigGet)) {
        if (request.path.isEmpty())
            return refusal(QStringLiteral("config get needs a key path, as in `qsctl config get bar.height`"));
        const std::optional<QJsonValue> value = capabilities_.configValue(request.path);
        if (!value.has_value())
            return refusal(QStringLiteral("no configuration key \"%1\"").arg(request.path));
        response.data.insert(QStringLiteral("path"), request.path);
        response.data.insert(QStringLiteral("value"), *value);
        return response;
    }

    if (request.verb == QLatin1StringView(verb::BarToggle)) {
        response.data.insert(QStringLiteral("visible"), capabilities_.toggleBar());
        return response;
    }

    // Named, never guessed at: a client asking for a verb this build does not implement is told which ones
    // it does, so a typo and a missing feature are distinguishable from the answer alone.
    return refusal(QStringLiteral("this shell does not implement the verb \"%1\"; it implements %2")
                       .arg(request.verb, implementedVerbs()));
}

void IPCServer::handleConnection(QLocalSocket* socket)
{
    // The connection parents itself to the socket, so dropping the returned pointer is not a leak.
    new Connection(socket, *this);
    qCDebug(quantum::app::ipcLog) << "a client connected";
}

}  // namespace quantum::ipc
