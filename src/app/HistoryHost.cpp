#include "app/HistoryHost.h"

#include "app/Logging.h"
#include "dbus/NotificationService.h"
#include "wayland/LayerShellWindow.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QScreen>

namespace quantum::app {

HistoryHost::HistoryHost(quantum::dbus::NotificationService& service, QQmlEngine& engine, const QUrl& historyUrl,
                         QObject* parent)
    : QObject(parent)
    , service_(&service)
{
    component_ = new QQmlComponent(&engine, historyUrl, this);
    if (component_->isError()) {
        // Held rather than logged and dropped: whether a shell whose panel cannot be drawn runs at all is the
        // composition root's decision, and it cannot make it without the reason.
        componentError_ = component_->errorString();
        qCCritical(quantum::app::waylandLog) << "the history component did not load:" << componentError_;
        return;
    }

    connect(&service, &quantum::dbus::NotificationService::notificationHistoryOpenChanged, this,
            &HistoryHost::handleOpenChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &HistoryHost::handleScreenRemoved);
}

QWindow* HistoryHost::window() const
{
    return window_.data();
}

void HistoryHost::handleOpenChanged()
{
    if (service_->notificationHistoryOpen())
        open();
    else
        close();
}

void HistoryHost::open()
{
    if (window_ != nullptr)
        return;

    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        // No output to put it on: the panel is not open, and saying so keeps the next toggle meaningful.
        qCWarning(quantum::app::waylandLog) << "no output for the notification history";
        service_->setNotificationHistoryOpen(false);
        return;
    }

    QObject* object = component_->create();
    auto* window = qobject_cast<QuantumShell::LayerShellWindow*>(object);
    if (window == nullptr) {
        qCWarning(quantum::app::waylandLog)
            << "the history component did not create a layer-shell window:" << component_->errorString();
        delete object;
        service_->setNotificationHistoryOpen(false);
        return;
    }

    // The screen is assigned before the surface is presented, for the reason the bar's host does: a layer
    // surface is created against an output when it is mapped, and the role is assigned once.
    window->setScreen(screen);
    // The compositor closing the surface (or the window being hidden any other way) is the panel no longer
    // being open, which the service has to hear or the next toggle would try to close what is gone.
    connect(window, &QWindow::visibleChanged, this, [this, window](bool visible) {
        if (!visible && window_ == window)
            service_->setNotificationHistoryOpen(false);
    });
    window->present();
    window_ = window;
    screen_ = screen;
    qCInfo(quantum::app::waylandLog) << "notification history opened on" << screen->name();
}

void HistoryHost::close()
{
    if (window_ == nullptr)
        return;
    QWindow* window = window_.data();
    window_.clear();
    // Deleted rather than hidden, the way toasts are: deleting the window is what destroys the surface. Later
    // than this call, because a close can come from the window's own click handler, and Qt refuses to destroy
    // an object while one of its QML handlers is running.
    window->deleteLater();
}

void HistoryHost::handleScreenRemoved(QScreen* screen)
{
    if (window_ == nullptr || screen_ != screen)
        return;
    service_->setNotificationHistoryOpen(false);
}

}  // namespace quantum::app
