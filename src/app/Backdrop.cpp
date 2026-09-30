#include "app/Backdrop.h"

#include "app/Logging.h"
#include "wayland/LayerShellWindow.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QScreen>

namespace quantum::app {

Backdrop::Backdrop(QQmlEngine& engine, const QUrl& url, QObject* parent)
    : QObject(parent)
{
    if (url.isEmpty())
        return;
    component_ = new QQmlComponent(&engine, url, this);
    if (component_->isError()) {
        componentError_ = component_->errorString();
        qCCritical(quantum::app::waylandLog) << "the backdrop component did not load:" << componentError_;
    }
}

QWindow* Backdrop::window() const
{
    return window_.data();
}

bool Backdrop::show(QScreen* screen)
{
    if (component_ == nullptr || !ready() || window_ != nullptr)
        return true;

    QObject* object = component_->create();
    auto* window = qobject_cast<QuantumShell::LayerShellWindow*>(object);
    if (window == nullptr) {
        qCWarning(quantum::app::waylandLog)
            << "the backdrop component did not create a layer-shell window:" << component_->errorString();
        delete object;
        return false;
    }
    // The output is assigned before the surface is presented, like every other surface: the role is assigned once.
    window->setScreen(screen);
    connect(window, SIGNAL(dismissed()), this, SIGNAL(dismissed()));
    window->present();
    window_ = window;
    return true;
}

void Backdrop::hide()
{
    if (window_ == nullptr)
        return;
    QWindow* window = window_.data();
    window_.clear();
    window->deleteLater();
}

}  // namespace quantum::app
