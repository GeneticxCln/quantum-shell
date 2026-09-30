#include "app/PanelHost.h"

#include "app/Backdrop.h"
#include "app/Logging.h"
#include "wayland/LayerShellWindow.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QScreen>

namespace quantum::app {

PanelHost::PanelHost(State state, QQmlEngine& engine, const QUrl& panelUrl, const QUrl& backdropUrl, QString label,
                     QObject* parent)
    : QObject(parent)
    , state_(std::move(state))
    , label_(std::move(label))
{
    backdrop_ = new Backdrop(engine, backdropUrl, this);
    componentError_ = backdrop_->componentError();
    connect(backdrop_, &Backdrop::dismissed, this, [this] { state_.setOpen(false); });

    component_ = new QQmlComponent(&engine, panelUrl, this);
    if (component_->isError()) {
        componentError_ = component_->errorString();
        qCCritical(quantum::app::waylandLog) << label_ << "component did not load:" << componentError_;
        return;
    }

    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &PanelHost::handleScreenRemoved);
}

QWindow* PanelHost::window() const
{
    return window_.data();
}

QWindow* PanelHost::backdropWindow() const
{
    return backdrop_->window();
}

void PanelHost::handleOpenChanged()
{
    if (state_.isOpen())
        open();
    else
        close();
}

void PanelHost::open()
{
    if (window_ != nullptr)
        return;

    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        qCWarning(quantum::app::waylandLog) << "no output for" << label_;
        state_.setOpen(false);
        return;
    }

    // The backdrop is on the top layer and the panel on the overlay layer, so the panel is above it whichever is
    // mapped first; if the panel then cannot be created the backdrop is taken down again below.
    if (!backdrop_->show(screen)) {
        state_.setOpen(false);
        return;
    }

    QObject* object = component_->create();
    auto* window = qobject_cast<QuantumShell::LayerShellWindow*>(object);
    if (window == nullptr) {
        qCWarning(quantum::app::waylandLog)
            << label_ << "component did not create a layer-shell window:" << component_->errorString();
        delete object;
        backdrop_->hide();
        state_.setOpen(false);
        return;
    }

    // The output is assigned before the surface is presented: the layer role is assigned once, against the output
    // the window is on when it is first mapped.
    window->setScreen(screen);
    // The compositor closing the surface (or the window being hidden any other way) is the panel no longer being
    // open, which the service has to hear or the next toggle would try to close what is gone.
    connect(window, &QWindow::visibleChanged, this, [this, window](bool visible) {
        if (!visible && window_ == window)
            state_.setOpen(false);
    });
    window->present();
    window_ = window;
    screen_ = screen;
    qCInfo(quantum::app::waylandLog).noquote() << label_ << "opened on" << screen->name();
}

void PanelHost::close()
{
    if (window_ == nullptr)
        return;
    backdrop_->hide();
    QWindow* window = window_.data();
    window_.clear();
    // Deleted rather than hidden, and later than this call: deleting the window is what destroys the layer surface,
    // and a close can come from the surface's own key or click handler, which Qt will not let an object be
    // destroyed under.
    window->deleteLater();
}

void PanelHost::handleScreenRemoved(QScreen* screen)
{
    if (window_ == nullptr || screen_ != screen)
        return;
    state_.setOpen(false);
}

}  // namespace quantum::app
