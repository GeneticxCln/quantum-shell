// A test double for the compositor's end of the niri IPC socket.
//
// It speaks what a running niri 26.04 was verified to speak: one JSON request per line, one
// `{"Ok": ...}` or `{"Err": "..."}` reply per line, in request order. The replies are supplied by the
// test, which is the point — protocol, framing and detection logic are unit-tested without a
// compositor, and the real thing is covered by tests/integration/niri_live_test.cpp.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

class FakeNiriServer : public QObject {
    Q_OBJECT

public:
    explicit FakeNiriServer(QObject* parent = nullptr);
    ~FakeNiriServer() override;

    // Creates the socket in a scratch directory that disappears with this object. False means the test
    // cannot run; the reason is in serverError().
    bool listen();
    QString path() const;
    QString serverError() const;

    // The reply for `requestName` (the bare name niri receives, e.g. "Version"), without a newline.
    void setReply(const QString& requestName, const QByteArray& reply);
    // Used for any request without a reply of its own: niri's answer to a request it does not know.
    void setDefaultReply(const QByteArray& reply);

    // Replies are normally written as their requests arrive. While deferred, flushReplies() writes
    // them together, so a test can make several replies arrive in a single read.
    void setDeferReplies(bool deferred);
    void flushReplies();

    // Sends `bytes` verbatim, with no newline and no reply bookkeeping.
    void writeRaw(const QByteArray& bytes);

    // The same, to the connection accepted at `connectionIndex`, counting from zero in acceptance
    // order. A test that has both of the shell's connections open addresses the event stream with
    // this instead of depending on which socket happens to be first.
    void writeRawTo(int connectionIndex, const QByteArray& bytes);

    // Drops every connection, the way niri would when it exits or the session ends.
    void closeConnections();

    // Every request line received, exactly as it arrived on the wire — JSON quoting included, so a
    // test can assert the bytes rather than the intent.
    QStringList receivedRequests() const;
    int connectionCount() const;

private:
    void handleNewConnection();
    void handleReadyRead(QLocalSocket* client);
    void writeReply(QLocalSocket* client, const QByteArray& reply);
    QByteArray replyFor(const QString& requestName) const;
    QList<QLocalSocket*> liveClients() const;

    QTemporaryDir directory_;
    QLocalServer server_;
    QString serverError_;
    QList<QLocalSocket*> clients_;
    QHash<QLocalSocket*, QByteArray> buffers_;
    QHash<QString, QByteArray> replies_;
    QStringList received_;
    QList<QByteArray> deferred_;
    QByteArray defaultReply_{"{\"Err\":\"error parsing request\"}"};
    int connections_ = 0;
    bool deferReplies_ = false;
};
