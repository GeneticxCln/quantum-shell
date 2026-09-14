#include "niri/NiriIPC.h"

#include <QJsonDocument>
#include <QLocalSocket>
#include <QStringList>

#include <utility>

namespace quantum::niri {
namespace {

// A request is a unit variant of niri's `Request` enum: a bare JSON string holding its name, and niri
// spelled those names in ASCII letters. Sending only what fits that shape is what keeps quoting and
// escaping out of a stream the compositor parses — an unescaped quote or newline would otherwise
// smuggle a second line into the connection.
bool isEncodableRequestName(const QString& name) {
    if (name.isEmpty()) {
        return false;
    }
    for (const QChar character : name) {
        if (character.unicode() > 0x7f || !character.isLetter()) {
            return false;
        }
    }
    return true;
}

}  // namespace

Availability classifyProbeReply(QStringView expectedVariant, const Reply& reply) {
    switch (reply.kind) {
    case Reply::Kind::transportError:
        // Nothing was learned: the reply never arrived in a form we could read.
        return Availability::unknown;
    case Reply::Kind::error:
        return reply.text == unknownRequestError ? Availability::unsupported : Availability::supported;
    case Reply::Kind::ok:
        break;
    }
    if (reply.value.isObject() && !reply.variant(expectedVariant).isUndefined()) {
        return Availability::supported;
    }
    // Answered, but not with the response we asked for: report it as unknown rather than reading
    // agreement into a reply we do not understand.
    return Availability::unknown;
}

NiriIPC::NiriIPC(QObject* parent) : QObject(parent), socket_(new QLocalSocket(this)) {
    connect(socket_, &QLocalSocket::connected, this, [this] {
        flushPending();
        emit connected();
    });
    connect(socket_, &QLocalSocket::disconnected, this, [this] {
        failPending(QStringLiteral("niri closed the IPC connection"));
        emit disconnected();
    });
    connect(socket_, &QLocalSocket::readyRead, this, &NiriIPC::handleReadyRead);
    connect(socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        // The socket's own message names the problem (no such file, refused, closed) better than a
        // summary invented here.
        const QString reason = socket_->errorString();
        failPending(reason);
        emit transportError(reason);
    });
}

void NiriIPC::connectToCompositor(const QString& path) {
    const QString target = path.isEmpty() ? niriSocketPath() : path;
    if (target.isEmpty()) {
        emit transportError(QStringLiteral(
            "$NIRI_SOCKET is not set, so there is no niri IPC socket for this process"));
        return;
    }
    socket_->abort();
    buffer_.clear();
    socket_->connectToServer(target);
}

void NiriIPC::disconnectFromCompositor() {
    const bool wasConnected = isConnected();
    socket_->disconnectFromServer();
    buffer_.clear();
    failPending(QStringLiteral("niri IPC connection closed"));
    if (wasConnected) {
        // Deliberate or not, nothing about the compositor is known from here any more, and a consumer
        // bound to `disconnected` has to hear it. NiriReconnect re-attaches whatever drops while it is
        // running, so a shell that means to stay disconnected stops it first.
        emit disconnected();
    }
}

bool NiriIPC::isConnected() const {
    return socket_->state() == QLocalSocket::ConnectedState;
}

void NiriIPC::send(QStringView requestName, ReplyHandler handler) {
    const QString name = requestName.toString();
    if (!isEncodableRequestName(name)) {
        Reply reply;
        reply.kind = Reply::Kind::transportError;
        reply.text = QStringLiteral("\"%1\" is not a niri request name").arg(name);
        if (handler) {
            handler(reply);
        }
        return;
    }

    enqueue('"' + name.toLatin1() + '"', std::move(handler));
}

void NiriIPC::sendObject(const QJsonObject& request, ReplyHandler handler) {
    enqueue(QJsonDocument(request).toJson(QJsonDocument::Compact), std::move(handler));
}

void NiriIPC::enqueue(QByteArray line, ReplyHandler handler) {
    PendingRequest request;
    request.line = std::move(line);
    request.handler = std::move(handler);
    pending_.append(std::move(request));
    flushPending();
}

void NiriIPC::flushPending() {
    if (!isConnected()) {
        return;
    }
    for (auto& request : pending_) {
        if (request.written) {
            continue;
        }
        socket_->write(request.line);
        socket_->write("\n");
        request.written = true;
    }
    socket_->flush();
}

void NiriIPC::handleReadyRead() {
    buffer_ += socket_->readAll();
    for (const QByteArray& line : takeCompleteLines(buffer_)) {
        handleLine(line);
    }
    const QString oversized = oversizedBufferReason(buffer_);
    if (!oversized.isEmpty()) {
        buffer_.clear();
        failPending(oversized);
        emit transportError(oversized);
    }
}

void NiriIPC::handleLine(const QByteArray& line) {
    deliver(decodeReplyLine(line));
}

void NiriIPC::deliver(const Reply& reply) {
    if (pending_.isEmpty()) {
        emit transportError(
            QStringLiteral("niri sent a reply with no request outstanding: %1").arg(reply.describe()));
        return;
    }
    const PendingRequest request = pending_.takeFirst();
    if (request.handler) {
        request.handler(reply);
    }
}

void NiriIPC::failPending(const QString& reason) {
    const QList<PendingRequest> waiting = pending_;
    pending_.clear();
    for (const auto& request : waiting) {
        if (!request.handler) {
            continue;
        }
        Reply reply;
        reply.kind = Reply::Kind::transportError;
        reply.text = reason;
        request.handler(reply);
    }
}

void NiriIPC::detect() {
    if (!isConnected()) {
        const QString socket = niriSocketPath();
        emit detectionFailed(QStringLiteral("no niri IPC connection is open (socket: %1)")
                                 .arg(socket.isEmpty() ? QStringLiteral("$NIRI_SOCKET unset") : socket));
        return;
    }

    detecting_ = true;
    probesOutstanding_ = 0;
    capabilities_ = NiriCapabilities{};

    send(QStringLiteral("Version"), [this](const Reply& reply) {
        const QJsonValue value = reply.variant(QStringLiteral("Version"));
        if (!reply.isOk() || !value.isString()) {
            detecting_ = false;
            emit detectionFailed(QStringLiteral("niri did not report a version: %1").arg(reply.describe()));
            return;
        }
        const NiriVersion version = NiriVersion::parse(value.toString());
        if (!version.isValid()) {
            detecting_ = false;
            emit detectionFailed(
                QStringLiteral("niri reported a version this shell does not recognise: \"%1\"")
                    .arg(value.toString()));
            return;
        }
        capabilities_.setVersion(version);
        capabilities_.recordProbe(NiriFeature::version, Availability::supported);
        emit versionDetected(version);
        startProbes();
    });
}

void NiriIPC::startProbes() {
    const QList<NiriFeature> features = probedFeatures();
    probesOutstanding_ = static_cast<int>(features.size());
    if (features.isEmpty()) {
        finishProbes();
        return;
    }
    for (const NiriFeature feature : features) {
        const QString variant = responseVariant(feature);
        send(requestName(feature), [this, feature, variant](const Reply& reply) {
            capabilities_.recordProbe(feature, classifyProbeReply(variant, reply));
            --probesOutstanding_;
            if (probesOutstanding_ <= 0) {
                finishProbes();
            }
        });
    }
}

void NiriIPC::finishProbes() {
    if (!detecting_) {
        return;
    }
    detecting_ = false;
    emit capabilitiesDetected(capabilities_);
}

}  // namespace quantum::niri
