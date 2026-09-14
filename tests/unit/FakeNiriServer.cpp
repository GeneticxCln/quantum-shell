#include "FakeNiriServer.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace {

// The request name inside one line: a unit variant is a bare JSON string ("Version"), a struct
// variant is an object whose only key is the name. Both shapes are recognised so a test can add
// either, and the raw line is kept separately when a test wants to assert the bytes.
QString requestNameIn(const QByteArray& line) {
    const QString raw = QString::fromUtf8(line);
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (document.isObject()) {
        return document.object().keys().value(0);
    }
    if (raw.size() >= 2 && raw.startsWith(QLatin1Char('"')) && raw.endsWith(QLatin1Char('"'))) {
        return raw.mid(1, raw.size() - 2);
    }
    return raw;
}

}  // namespace

FakeNiriServer::FakeNiriServer(QObject* parent) : QObject(parent) {
    connect(&server_, &QLocalServer::newConnection, this, &FakeNiriServer::handleNewConnection);
}

FakeNiriServer::~FakeNiriServer() {
    // The socket file goes away with the scratch directory: nothing is left behind for the next run
    // to trip over.
    server_.close();
}

bool FakeNiriServer::listen() {
    if (!directory_.isValid()) {
        serverError_ = QStringLiteral("the test could not create a scratch directory for the socket");
        return false;
    }
    const QString socketPath = directory_.filePath(QStringLiteral("niri.sock"));
    if (!server_.listen(socketPath)) {
        serverError_ = server_.errorString();
        return false;
    }
    return true;
}

QString FakeNiriServer::path() const {
    return server_.fullServerName();
}

QString FakeNiriServer::serverError() const {
    return serverError_;
}

void FakeNiriServer::setReply(const QString& requestName, const QByteArray& reply) {
    replies_.insert(requestName, reply);
}

void FakeNiriServer::setDefaultReply(const QByteArray& reply) {
    defaultReply_ = reply;
}

void FakeNiriServer::setDeferReplies(bool deferred) {
    deferReplies_ = deferred;
}

void FakeNiriServer::flushReplies() {
    const QList<QLocalSocket*> clients = liveClients();
    if (clients.isEmpty()) {
        deferred_.clear();
        return;
    }
    QByteArray batch;
    for (const QByteArray& reply : std::as_const(deferred_)) {
        batch += reply;
        batch += '\n';
    }
    deferred_.clear();
    clients.first()->write(batch);
    clients.first()->flush();
}

void FakeNiriServer::writeRaw(const QByteArray& bytes) {
    writeRawTo(0, bytes);
}

void FakeNiriServer::writeRawTo(int connectionIndex, const QByteArray& bytes) {
    const QList<QLocalSocket*> clients = liveClients();
    if (connectionIndex < 0 || connectionIndex >= clients.size()) {
        return;
    }
    QLocalSocket* client = clients.at(connectionIndex);
    client->write(bytes);
    client->flush();
}

void FakeNiriServer::closeConnections() {
    for (QLocalSocket* client : std::as_const(clients_)) {
        if (client->state() != QLocalSocket::UnconnectedState) {
            client->close();
        }
    }
    buffers_.clear();
}

QStringList FakeNiriServer::receivedRequests() const {
    return received_;
}

int FakeNiriServer::connectionCount() const {
    return connections_;
}

void FakeNiriServer::handleNewConnection() {
    while (server_.hasPendingConnections()) {
        QLocalSocket* client = server_.nextPendingConnection();
        if (client == nullptr) {
            break;
        }
        client->setParent(this);
        clients_.append(client);
        ++connections_;
        connect(client, &QLocalSocket::readyRead, this, [this, client] { handleReadyRead(client); });
        connect(client, &QLocalSocket::disconnected, this, [this, client] { clients_.removeAll(client); });
    }
}

void FakeNiriServer::handleReadyRead(QLocalSocket* client) {
    QByteArray& buffer = buffers_[client];
    buffer += client->readAll();

    int newline = buffer.indexOf('\n');
    while (newline >= 0) {
        const QByteArray line = buffer.left(newline);
        buffer.remove(0, newline + 1);
        if (!line.trimmed().isEmpty()) {
            received_.append(QString::fromUtf8(line));
            writeReply(client, replyFor(requestNameIn(line)));
        }
        newline = buffer.indexOf('\n');
    }
}

QByteArray FakeNiriServer::replyFor(const QString& requestName) const {
    return replies_.value(requestName, defaultReply_);
}

void FakeNiriServer::writeReply(QLocalSocket* client, const QByteArray& reply) {
    if (deferReplies_) {
        deferred_.append(reply);
        return;
    }
    client->write(reply + '\n');
    client->flush();
}

QList<QLocalSocket*> FakeNiriServer::liveClients() const {
    QList<QLocalSocket*> live;
    for (QLocalSocket* client : std::as_const(clients_)) {
        if (client->state() == QLocalSocket::ConnectedState) {
            live.append(client);
        }
    }
    return live;
}
