#include "app/LauncherHost.h"

#include "app/Backdrop.h"
#include "app/Logging.h"
#include "apps/LauncherService.h"
#include "wayland/LayerShellWindow.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QScreen>

namespace quantum::app {

LauncherHost::LauncherHost(quantum::apps::LauncherService& service, QQmlEngine& engine, const QUrl& launcherUrl,
                           const QUrl& backdropUrl, QObject* parent)
    : QObject(parent)
    , service_(&service)
{
    backdrop_ = new Backdrop(engine, backdropUrl, this);
    componentError_ = backdrop_->componentError();
    connect(backdrop_, &Backdrop::dismissed, this, [this] { service_->setOpen(false); });

    component_ = new QQmlComponent(&engine, launcherUrl, this);
    if (component_->isError()) {
        componentError_ = component_->errorString();
        qCCritical(quantum::app::waylandLog) << "the launcher component did not load:" << componentError_;
        return;
    }

    connect(&service, &quantum::apps::LauncherService::openChanged, this, &LauncherHost::handleOpenChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &LauncherHost::handleScreenRemoved);
}

QWindow* LauncherHost::window() const
{
    return window_.data();
}

QWindow* LauncherHost::backdropWindow() const
{
    return backdrop_->window();
}

void LauncherHost::handleOpenChanged()
{
    if (service_->isOpen())
        open();
    else
        close();
}

void LauncherHost::open()
{
    if (window_ != nullptr)
        return;

    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        qCWarning(quantum::app::waylandLog) << "no output for the launcher";
        service_->setOpen(false);
        return;
    }

    // The backdrop is on the top layer and the panel on the overlay layer, so the panel is above it whichever is
    // mapped first; if the panel then cannot be created the backdrop is taken down again below.
    if (!backdrop_->show(screen)) {
        service_->setOpen(false);
        return;
    }

    QObject* object = component_->create();
    auto* window = qobject_cast<QuantumShell::LayerShellWindow*>(object);
    if (window == nullptr) {
        qCWarning(quantum::app::waylandLog)
            << "the launcher component did not create a layer-shell window:" << component_->errorString();
        delete object;
        backdrop_->hide();
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
    qCInfo(quantum::app::waylandLog) << "launcher opened on" << screen->name();
}

void LauncherHost::close()
{
    if (window_ == nullptr)
        return;
    backdrop_->hide();
    QWindow* window = window_.data();
    window_.clear();
    // Later than this call: a close comes from the surface's own key handler (Escape, Enter), and Qt refuses to
    // destroy an object while one of its QML handlers is running.
    window->deleteLater();
}

void LauncherHost::handleScreenRemoved(QScreen* screen)
{
    if (window_ == nullptr || screen_ != screen)
        return;
    service_->setOpen(false);
}

}  // namespace quantum::app
