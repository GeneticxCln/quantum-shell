#include "app/ControlCenterHost.h"

#include "app/Logging.h"
#include "app/ControlCenterService.h"
#include "wayland/LayerShellWindow.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QScreen>

namespace quantum::app {

ControlCenterHost::ControlCenterHost(ControlCenterService& service, QQmlEngine& engine, const QUrl& panelUrl,
                           QObject* parent)
    : QObject(parent)
    , service_(&service)
{
    component_ = new QQmlComponent(&engine, panelUrl, this);
    if (component_->isError()) {
        componentError_ = component_->errorString();
        qCCritical(quantum::app::waylandLog) << "the control centre component did not load:" << componentError_;
        return;
    }

    connect(&service, &ControlCenterService::openChanged, this, &ControlCenterHost::handleOpenChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &ControlCenterHost::handleScreenRemoved);
}

QWindow* ControlCenterHost::window() const
{
    return window_.data();
}

void ControlCenterHost::handleOpenChanged()
{
    if (service_->isOpen())
        open();
    else
        close();
}

void ControlCenterHost::open()
{
    if (window_ != nullptr)
        return;

    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        qCWarning(quantum::app::waylandLog) << "no output for the control centre";
        service_->setOpen(false);
        return;
    }

    QObject* object = component_->create();
    auto* window = qobject_cast<QuantumShell::LayerShellWindow*>(object);
    if (window == nullptr) {
        qCWarning(quantum::app::waylandLog)
            << "the control centre component did not create a layer-shell window:" << component_->errorString();
        delete object;
        service_->setOpen(false);
        return;
    }

    // The output is assigned before the surface is presented: the layer role is assigned once, against the output
    // the window is on when it is first mapped.
    window->setScreen(screen);
    connect(window, &QWindow::visibleChanged, this, [this, window](bool visible) {
        if (!visible && window_ == window)
            service_->setOpen(false);
    });
    window->present();
    window_ = window;
    screen_ = screen;
    qCInfo(quantum::app::waylandLog) << "control centre opened on" << screen->name();
}

void ControlCenterHost::close()
{
    if (window_ == nullptr)
        return;
    QWindow* window = window_.data();
    window_.clear();
    // Later than this call: a close comes from the surface's own key handler (Escape, Enter), and Qt refuses to
    // destroy an object while one of its QML handlers is running.
    window->deleteLater();
}

void ControlCenterHost::handleScreenRemoved(QScreen* screen)
{
    if (window_ == nullptr || screen_ != screen)
        return;
    service_->setOpen(false);
}

}  // namespace quantum::app
