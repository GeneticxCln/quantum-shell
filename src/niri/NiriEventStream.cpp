#include "niri/NiriEventStream.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLocalSocket>

namespace quantum::niri {
namespace {

// Reads a list of objects into value types, dropping entries that carry no usable id: one unreadable
// window is a smaller failure than discarding the whole update.
template <typename T, typename Parse>
QList<T> parseList(const QJsonArray& array, Parse parse) {
    QList<T> items;
    items.reserve(array.size());
    for (const QJsonValue& value : array) {
        if (!value.isObject()) {
            continue;
        }
        T item = parse(value.toObject());
        if (item.isValid()) {
            items.append(item);
        }
    }
    return items;
}

}  // namespace

NiriEventStream::NiriEventStream(QObject* parent) : QObject(parent), socket_(new QLocalSocket(this)) {
    connect(socket_, &QLocalSocket::connected, this, [this] {
        // The subscription is the first and only request this connection ever sends.
        socket_->write("\"EventStream\"\n");
        socket_->flush();
        emit connected();
    });
    connect(socket_, &QLocalSocket::disconnected, this, [this] {
        streaming_ = false;
        buffer_.clear();
        emit disconnected();
    });
    connect(socket_, &QLocalSocket::readyRead, this, &NiriEventStream::handleReadyRead);
    connect(socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        const QString reason = socket_->errorString();
        fail(reason);
        emit transportError(reason);
    });
}

void NiriEventStream::connectToCompositor(const QString& path) {
    const QString target = path.isEmpty() ? niriSocketPath() : path;
    if (target.isEmpty()) {
        emit transportError(QStringLiteral(
            "$NIRI_SOCKET is not set, so there is no niri IPC socket for this process"));
        return;
    }
    socket_->abort();
    buffer_.clear();
    streaming_ = false;
    unmodelled_.clear();
    unknown_.clear();
    socket_->connectToServer(target);
}

void NiriEventStream::disconnectFromCompositor() {
    const bool wasStreaming = streaming_;
    socket_->disconnectFromServer();
    buffer_.clear();
    streaming_ = false;
    if (wasStreaming) {
        // See NiriIPC::disconnectFromCompositor: whatever the reason, the state that arrived over this
        // stream is no longer known to be current, and that is what `disconnected` tells consumers.
        emit disconnected();
    }
}

bool NiriEventStream::isConnected() const {
    return socket_->state() == QLocalSocket::ConnectedState;
}

bool NiriEventStream::isStreaming() const {
    return streaming_;
}

QStringList NiriEventStream::unmodelledEvents() const {
    return unmodelled_;
}

QStringList NiriEventStream::unknownEvents() const {
    return unknown_;
}

void NiriEventStream::handleReadyRead() {
    buffer_ += socket_->readAll();
    for (const QByteArray& line : takeCompleteLines(buffer_)) {
        handleLine(line);
    }
    const QString oversized = oversizedBufferReason(buffer_);
    if (!oversized.isEmpty()) {
        buffer_.clear();
        fail(oversized);
        emit transportError(oversized);
    }
}

void NiriEventStream::handleLine(const QByteArray& line) {
    if (!streaming_) {
        // Until the subscription is acknowledged there is exactly one line to expect: the reply.
        handleAck(decodeReplyLine(line));
        return;
    }
    const EventLine event = decodeEventLine(line);
    if (!event.isValid()) {
        emit malformedEvent(QStringLiteral("<line>"),
                            QStringLiteral("not a single-event object: %1")
                                .arg(QString::fromUtf8(line.left(120))));
        return;
    }
    handleEvent(event.name, event.fields);
}

void NiriEventStream::handleAck(const Reply& reply) {
    if (!reply.isOk()) {
        fail(QStringLiteral("niri refused the event stream: %1").arg(reply.describe()));
        return;
    }
    streaming_ = true;
    emit streaming();
}

void NiriEventStream::handleEvent(const QString& name, const QJsonValue& fields) {
    const QJsonObject payload = fields.toObject();

    if (name == QLatin1StringView("WorkspacesChanged")) {
        const QJsonArray array = payload.value(QStringLiteral("workspaces")).toArray();
        emit workspacesChanged(parseList<NiriWorkspace>(
            array, [](const QJsonObject& object) { return NiriWorkspace::fromJson(object); }));
        return;
    }
    if (name == QLatin1StringView("WindowsChanged")) {
        const QJsonArray array = payload.value(QStringLiteral("windows")).toArray();
        emit windowsChanged(parseList<NiriWindow>(
            array, [](const QJsonObject& object) { return NiriWindow::fromJson(object); }));
        return;
    }
    if (name == QLatin1StringView("WindowOpenedOrChanged")) {
        const NiriWindow window = NiriWindow::fromJson(payload.value(QStringLiteral("window")).toObject());
        if (!window.isValid()) {
            emit malformedEvent(name, QStringLiteral("window carries no id"));
            return;
        }
        emit windowOpenedOrChanged(window);
        return;
    }
    if (name == QLatin1StringView("WindowClosed")) {
        const std::optional<quint64> id = idFromJson(payload.value(QStringLiteral("id")));
        if (!id.has_value()) {
            emit malformedEvent(name, QStringLiteral("no window id"));
            return;
        }
        emit windowClosed(*id);
        return;
    }
    if (name == QLatin1StringView("WindowFocusChanged")) {
        const std::optional<quint64> id = idFromJson(payload.value(QStringLiteral("id")));
        // The id is optional here: null means no window is focused, which is a real state, not a
        // malformed event. A null is the only way to express it, so this one case is not an error.
        emit focusedWindowChanged(id.value_or(0), id.has_value());
        return;
    }
    if (name == QLatin1StringView("WindowUrgencyChanged")) {
        const std::optional<quint64> id = idFromJson(payload.value(QStringLiteral("id")));
        if (!id.has_value()) {
            emit malformedEvent(name, QStringLiteral("no window id"));
            return;
        }
        emit windowUrgencyChanged(*id, payload.value(QStringLiteral("urgent")).toBool(false));
        return;
    }
    if (name == QLatin1StringView("WorkspaceActivated")) {
        const std::optional<quint64> id = idFromJson(payload.value(QStringLiteral("id")));
        if (!id.has_value()) {
            emit malformedEvent(name, QStringLiteral("no workspace id"));
            return;
        }
        emit workspaceActivated(*id, payload.value(QStringLiteral("focused")).toBool(false));
        return;
    }
    if (name == QLatin1StringView("WorkspaceUrgencyChanged")) {
        const std::optional<quint64> id = idFromJson(payload.value(QStringLiteral("id")));
        if (!id.has_value()) {
            emit malformedEvent(name, QStringLiteral("no workspace id"));
            return;
        }
        emit workspaceUrgencyChanged(*id, payload.value(QStringLiteral("urgent")).toBool(false));
        return;
    }
    if (name == QLatin1StringView("WorkspaceActiveWindowChanged")) {
        const std::optional<quint64> workspace = idFromJson(payload.value(QStringLiteral("workspace_id")));
        if (!workspace.has_value()) {
            emit malformedEvent(name, QStringLiteral("no workspace id"));
            return;
        }
        const std::optional<quint64> active = idFromJson(payload.value(QStringLiteral("active_window_id")));
        emit workspaceActiveWindowChanged(*workspace, active.value_or(0), active.has_value());
        return;
    }

    if (name == QLatin1StringView("KeyboardLayoutsChanged")) {
        const NiriKeyboardLayouts layouts =
            NiriKeyboardLayouts::fromJson(payload.value(QStringLiteral("keyboard_layouts")).toObject());
        if (!layouts.isValid()) {
            emit malformedEvent(name, QStringLiteral("keyboard_layouts carries no names"));
            return;
        }
        emit keyboardLayoutsChanged(layouts);
        return;
    }
    if (name == QLatin1StringView("KeyboardLayoutSwitched")) {
        const QJsonValue index = payload.value(QStringLiteral("idx"));
        if (!index.isDouble() || index.toDouble() < 0.0) {
            emit malformedEvent(name, QStringLiteral("no layout index"));
            return;
        }
        emit keyboardLayoutSwitched(static_cast<int>(index.toDouble()));
        return;
    }
    if (name == QLatin1StringView("OverviewOpenedOrClosed")) {
        const QJsonValue isOpen = payload.value(QStringLiteral("is_open"));
        if (!isOpen.isBool()) {
            emit malformedEvent(name, QStringLiteral("no is_open"));
            return;
        }
        emit overviewOpenedOrClosed(isOpen.toBool());
        return;
    }
    if (name == QLatin1StringView("ConfigLoaded")) {
        const QJsonValue failed = payload.value(QStringLiteral("failed"));
        if (!failed.isBool()) {
            emit malformedEvent(name, QStringLiteral("no failed flag"));
            return;
        }
        emit configLoaded(failed.toBool());
        return;
    }

    if (isKnownEventName(name)) {
        unmodelled_.append(name);
        emit unmodelledEvent(name);
        return;
    }
    unknown_.append(name);
    emit unknownEvent(name);
}

void NiriEventStream::fail(const QString& reason) {
    streaming_ = false;
    buffer_.clear();
    emit streamFailed(reason);
}

}  // namespace quantum::niri
