#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QQmlComponent;
class QQmlEngine;
class QScreen;
class QWindow;

namespace quantum::app {

class Backdrop;

class ControlCenterService;

// The control centre's surface: one window, on the primary output, that exists while the service says the control centre is
// open.
//
// It is `HistoryHost`'s shape for the same reason: the open/closed state is the service's (`open`), because the
// IPC verb that toggles it, the surface's own Escape key and this host are three separate things and the
// service is the one they all see. The host follows the property and also writes it — when the compositor
// closes the surface, or the output goes away, the control centre is not open any more and the service is told so, so
// the next toggle opens it instead of doing nothing.
class ControlCenterHost : public QObject
{
    Q_OBJECT

public:
    // Both must outlive this object. The surface is created against `engine` from `panelUrl`.
    explicit ControlCenterHost(ControlCenterService& service, QQmlEngine& engine, const QUrl& panelUrl,
                          const QUrl& backdropUrl = {}, QObject* parent = nullptr);

    bool ready() const { return componentError_.isEmpty(); }
    QString componentError() const { return componentError_; }

    // The surface, or null while the control centre is not open.
    QWindow* window() const;

    // The transparent surface behind the panel that a click outside it lands on, or null while the panel is not
    // open (or the host was built without a backdrop).
    QWindow* backdropWindow() const;

private:
    void handleOpenChanged();
    void handleScreenRemoved(QScreen* screen);
    void open();
    void close();

    ControlCenterService* service_ = nullptr;
    QQmlComponent* component_ = nullptr;
    QString componentError_;
    QPointer<QWindow> window_;
    Backdrop* backdrop_ = nullptr;
    QPointer<QScreen> screen_;
};

}  // namespace quantum::app
