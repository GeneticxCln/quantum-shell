#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <functional>

class QQmlComponent;
class QQmlEngine;
class QScreen;
class QWindow;

namespace quantum::app {

class Backdrop;

// The lifetime of one panel's surface: a layer-shell window on the primary output that exists while its service says
// the panel is open, with the transparent backdrop behind it that turns a click outside into a close.
//
// The launcher, the control centre, the notification history and the calendar are the same thing wearing four
// services. The open/closed state is the service's, because whatever toggles a panel (an IPC verb, a click on the bar,
// the panel's own Escape) and this host that draws it are separate objects and the service is the one thing all of
// them see. The host follows it and also writes it: when the compositor closes the surface, when the output goes
// away, or when the backdrop is pressed, the panel is not open any more and the service is told so, so the next
// toggle opens the panel instead of doing nothing.
//
// What differs between panels is only which service holds the state, which is what `State` is: the derived classes
// say how to read and write it and connect the service's change signal to `handleOpenChanged`, and everything else —
// the surface's creation, its output, its backdrop, its teardown — is here once.
class PanelHost : public QObject
{
    Q_OBJECT

public:
    struct State {
        std::function<bool()> isOpen;
        std::function<void(bool)> setOpen;
    };

    // `label` is the panel's name as a log line says it ("the launcher"). `backdropUrl` may be empty, which makes a
    // host with no backdrop. Both urls are loaded against `engine`, which must outlive this object.
    PanelHost(State state, QQmlEngine& engine, const QUrl& panelUrl, const QUrl& backdropUrl, QString label,
              QObject* parent = nullptr);

    // Whether both components were readable, and the reason when not, for the composition root's decision about a
    // shell whose panel cannot be drawn.
    bool ready() const { return componentError_.isEmpty(); }
    QString componentError() const { return componentError_; }

    // The panel, or null while it is not open.
    QWindow* window() const;

    // The transparent surface behind the panel that a click outside it lands on, or null while the panel is not
    // open (or the host was built without a backdrop).
    QWindow* backdropWindow() const;

protected:
    // Called by the derived class when its service's state changed.
    void handleOpenChanged();

private:
    void handleScreenRemoved(QScreen* screen);
    void open();
    void close();

    State state_;
    QString label_;
    QQmlComponent* component_ = nullptr;
    QString componentError_;
    QPointer<QWindow> window_;
    Backdrop* backdrop_ = nullptr;
    QPointer<QScreen> screen_;
};

}  // namespace quantum::app
