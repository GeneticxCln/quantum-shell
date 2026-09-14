// The niri IPC client: one connection to the compositor, one JSON value per line.
//
// Every fact below was verified against the running niri 26.04 on this machine, whose IPC types are
// defined by niri-ipc v26.04:
//
//   - the socket path comes from `$NIRI_SOCKET`. niri's own socket helper documents no other
//     mechanism, so this client does not guess a path;
//   - a request is one JSON value and a newline, a reply is one JSON value and a newline, and
//     replies come back in request order because niri processes requests one at a time;
//   - a reply is `{"Ok": <value>}` or `{"Err": "<message>"}`. A request the compositor does not know
//     is answered with `{"Err":"error parsing request"}`, which is how a capability probe detects a
//     build that lacks a request instead of assuming one;
//   - after `Request::EventStream` the compositor stops reading requests, so the event stream needs
//     a connection of its own. This class is the request connection.
//
// Everything here is asynchronous: QLocalSocket signals, one read buffer, no blocking read, no
// polling timer.
#pragma once

#include "niri/NiriProtocol.h"
#include "niri/NiriVersion.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringView>

#include <functional>

class QLocalSocket;

namespace quantum::niri {

// How a probe answer is classified. niri's own "unknown request" error — and only that error — means
// the running build lacks the feature; any other error means it knows the request and something else
// went wrong, which is not a missing capability.
Availability classifyProbeReply(QStringView expectedVariant, const Reply& reply);

class NiriIPC : public QObject {
    Q_OBJECT

public:
    using ReplyHandler = std::function<void(const Reply&)>;

    explicit NiriIPC(QObject* parent = nullptr);

    // `path` defaults to the socket niri names for this process. Asynchronous: `connected` or
    // `transportError` follows.
    void connectToCompositor(const QString& path = QString());
    void disconnectFromCompositor();
    bool isConnected() const;

    // Queues one request; `handler` runs when its reply arrives. Requests here are unit variants,
    // sent as the bare JSON string niri expects ("Version", "Workspaces", ...). A name niri does not
    // know still gets a reply — an error — which is what makes probing possible. A handler whose
    // request never gets an answer (connection lost, or this object destroyed) is not called.
    void send(QStringView requestName, ReplyHandler handler);

    // Sends a request that carries fields, which cannot be spelled as a bare string — an action is the
    // object `{"Action":{"OpenOverview":{}}}`. The object is serialised here in compact form, which
    // contains no line break by construction, so the one-request-per-line rule niri reads by cannot be
    // broken from this side and reply matching cannot drift. Replies are matched in order exactly as
    // for a unit request.
    void sendObject(const QJsonObject& request, ReplyHandler handler);

    // Asks for the version, then probes the read-only features. Emits exactly one of versionDetected
    // and detectionFailed, followed by capabilitiesDetected once a version was recognised.
    void detect();

signals:
    void connected();
    void disconnected();
    void transportError(const QString& reason);
    void versionDetected(const quantum::niri::NiriVersion& version);
    void capabilitiesDetected(const quantum::niri::NiriCapabilities& capabilities);
    void detectionFailed(const QString& reason);

private:
    struct PendingRequest {
        // The request exactly as it goes on the wire, without its newline: either the quoted name of a
        // unit variant or the compact JSON of one that carries fields.
        QByteArray line;
        ReplyHandler handler;
        bool written = false;
    };

    void enqueue(QByteArray line, ReplyHandler handler);
    void flushPending();
    void handleReadyRead();
    void handleLine(const QByteArray& line);
    void deliver(const Reply& reply);
    void failPending(const QString& reason);
    void startProbes();
    void finishProbes();

    QLocalSocket* socket_ = nullptr;
    QByteArray buffer_;
    QList<PendingRequest> pending_;

    bool detecting_ = false;
    int probesOutstanding_ = 0;
    NiriCapabilities capabilities_;
};

}  // namespace quantum::niri

// Detection results travel through signals so they can be queued across threads later without
// changing these signatures.
Q_DECLARE_METATYPE(quantum::niri::NiriVersion)
Q_DECLARE_METATYPE(quantum::niri::NiriCapabilities)
