#include "app/OsdHost.h"

#include "app/Logging.h"
#include "audio/PipeWireService.h"
#include "config/Config.h"
#include "wayland/LayerShellWindow.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QScreen>
#include <QTimer>

#include <algorithm>

namespace quantum::app {

OsdHost::OsdHost(quantum::audio::PipeWireService& audio, quantum::config::Config& config, QQmlEngine& engine,
                 const QUrl& osdUrl, QObject* parent)
    : QObject(parent)
    , config_(&config)
{
    component_ = new QQmlComponent(&engine, osdUrl, this);
    if (component_->isError()) {
        componentError_ = component_->errorString();
        qCCritical(quantum::app::waylandLog) << "the on-screen display component did not load:" << componentError_;
        return;
    }

    connect(&audio, &quantum::audio::PipeWireService::volumeAdjusted, this, &OsdHost::handleAdjusted);
    // Switching the display off takes down one that is up: a person who has turned it off is not helped by
    // one that stays until its clock runs out.
    connect(config.bar()->osd(), &quantum::config::ConfigOsd::showOsdChanged, this,
            &OsdHost::handleShowChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &OsdHost::handleScreenRemoved);

    // One clock for the set: every output's display shows the same change, so their lives end together.
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &OsdHost::withdraw);
}

QList<QWindow*> OsdHost::windows() const
{
    QList<QWindow*> windows;
    for (const Osd& osd : osds_) {
        if (osd.window != nullptr)
            windows.append(osd.window.data());
    }
    return windows;
}

void OsdHost::handleAdjusted()
{
    if (!config_->bar()->osd()->showOsd())
        return;
    show();
}

void OsdHost::handleShowChanged()
{
    if (!config_->bar()->osd()->showOsd())
        withdraw();
}

void OsdHost::show()
{
    osds_.removeIf([](const Osd& osd) { return osd.window == nullptr || osd.screen == nullptr; });

    for (QScreen* screen : QGuiApplication::screens()) {
        const bool have = std::any_of(osds_.cbegin(), osds_.cend(),
                                      [screen](const Osd& osd) { return osd.screen == screen; });
        // A display that is already up redraws itself from the service, so a further change costs a restart of
        // the clock and no surface.
        if (have)
            continue;

        QObject* object = component_->create();
        auto* window = qobject_cast<QuantumShell::LayerShellWindow*>(object);
        if (window == nullptr) {
            qCWarning(quantum::app::waylandLog)
                << "the on-screen display component did not create a layer-shell window for" << screen->name()
                << ":" << component_->errorString();
            delete object;
            continue;
        }
        // The output is assigned before the surface is presented, for the reason every other host does: the
        // layer role is assigned once, against the output the window is on when it is first mapped.
        window->setScreen(screen);
        window->present();
        osds_.append(Osd{screen, window});
    }

    timer_->start(config_->bar()->osd()->timeoutMs());
}

void OsdHost::withdraw()
{
    timer_->stop();
    for (const Osd& osd : osds_)
        delete osd.window.data();
    osds_.clear();
}

void OsdHost::handleScreenRemoved(QScreen* screen)
{
    for (int i = 0; i < osds_.size(); ++i) {
        if (osds_.at(i).screen != screen)
            continue;
        delete osds_.at(i).window.data();
        osds_.removeAt(i);
        return;
    }
}

}  // namespace quantum::app
