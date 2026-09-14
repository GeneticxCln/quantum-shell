// The event half of the niri connection: one long-lived socket subscribed to `EventStream`, whose
// lines arrive as typed signals.
//
// This has to be a socket of its own. niri stops reading requests once an `EventStream` request has
// been sent on a connection (niri-ipc v26.04), so the request connection in NiriIPC cannot double as
// this one; the shell runs the two side by side.
//
// The first line after subscribing is a reply (`{"Ok":"Handled"}`), not an event; everything after it
// is one event per line. An event this build does not model is reported by name rather than dropped, a
// name that is not an event of this protocol version is reported separately, and an event whose
// payload cannot be read is reported as malformed — three different problems that must not be
// collapsed into one silent no-op.
#pragma once

#include "niri/NiriKeyboardLayouts.h"
#include "niri/NiriProtocol.h"
#include "niri/NiriWindow.h"
#include "niri/NiriWorkspace.h"

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

class QLocalSocket;

namespace quantum::niri {

class NiriEventStream : public QObject {
    Q_OBJECT

public:
    explicit NiriEventStream(QObject* parent = nullptr);

    // `path` defaults to the socket niri names for this process.
    void connectToCompositor(const QString& path = QString());
    void disconnectFromCompositor();
    bool isConnected() const;  // the socket is up
    bool isStreaming() const;  // the compositor acknowledged the subscription

    // What arrived but is not acted on, in arrival order, so a consumer can report protocol drift
    // instead of guessing at it.
    QStringList unmodelledEvents() const;
    QStringList unknownEvents() const;

signals:
    void connected();
    void disconnected();
    void transportError(const QString& reason);
    void streamFailed(const QString& reason);
    void streaming();

    void workspacesChanged(const QList<quantum::niri::NiriWorkspace>& workspaces);
    void windowsChanged(const QList<quantum::niri::NiriWindow>& windows);
    void windowOpenedOrChanged(const quantum::niri::NiriWindow& window);
    void windowClosed(quint64 windowId);
    void windowUrgencyChanged(quint64 windowId, bool urgent);
    void focusedWindowChanged(quint64 windowId, bool hasWindow);
    void workspaceActivated(quint64 workspaceId, bool focused);
    void workspaceUrgencyChanged(quint64 workspaceId, bool urgent);
    void workspaceActiveWindowChanged(quint64 workspaceId, quint64 activeWindowId, bool hasWindow);

    // The configured keyboard layouts, then just the index whenever one is switched.
    void keyboardLayoutsChanged(const quantum::niri::NiriKeyboardLayouts& layouts);
    void keyboardLayoutSwitched(int index);

    void overviewOpenedOrClosed(bool isOpen);
    // A config reload. niri always sends this on connect, reporting the last load attempt, so a false
    // here at startup does not mean a reload happened.
    void configLoaded(bool failed);

    // A real event of this protocol version that this build does not model yet.
    void unmodelledEvent(const QString& name);
    // A name that is not an event of niri-ipc v26.04's `Event` enum: the protocol is not what this
    // build was written against.
    void unknownEvent(const QString& name);
    // An event this build does model, whose payload could not be read.
    void malformedEvent(const QString& name, const QString& reason);

private:
    void handleReadyRead();
    void handleLine(const QByteArray& line);
    void handleAck(const Reply& reply);
    void handleEvent(const QString& name, const QJsonValue& fields);
    void fail(const QString& reason);

    QLocalSocket* socket_ = nullptr;
    QByteArray buffer_;
    bool streaming_ = false;
    QStringList unmodelled_;
    QStringList unknown_;
};

}  // namespace quantum::niri

// The event payloads travel through signals, so they can be queued across threads later.
Q_DECLARE_METATYPE(quantum::niri::NiriWorkspace)
Q_DECLARE_METATYPE(quantum::niri::NiriWindow)
Q_DECLARE_METATYPE(QList<quantum::niri::NiriWorkspace>)
Q_DECLARE_METATYPE(QList<quantum::niri::NiriWindow>)
