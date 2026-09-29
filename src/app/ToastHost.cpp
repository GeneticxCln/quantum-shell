#include "app/ToastHost.h"

#include "app/Logging.h"
#include "config/Config.h"
#include "dbus/NotificationService.h"
#include "wayland/LayerShellWindow.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QScreen>
#include <QTimer>

namespace quantum::app {

ToastHost::ToastHost(quantum::dbus::NotificationService& service, quantum::config::Config& config,
                     QQmlEngine& engine, const QUrl& toastUrl, QObject* parent)
    : QObject(parent)
    , service_(&service)
    , config_(&config)
{
    component_ = new QQmlComponent(&engine, toastUrl, this);
    if (component_->isError()) {
        // Held rather than logged and dropped: whether a daemon answering notifications that nothing draws
        // runs at all is the composition root's decision, and it cannot make it without the reason.
        componentError_ = component_->errorString();
        qCCritical(quantum::app::waylandLog) << "the toast component did not load:" << componentError_;
        return;
    }

    // The reading is the daemon's, so the toast is created for what it publishes rather than for a flag of
    // ours to keep in step with it. Qt's own screen list is what a toast is created against, and a monitor
    // going away is a toast destroyed with it rather than one drawing nothing for the rest of its expiry.
    connect(&service, &quantum::dbus::NotificationService::notificationChanged, this,
            &ToastHost::handleNotificationChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &ToastHost::handleScreenRemoved);

    // One timer for the whole set rather than one per window: a single notification is one message on every
    // output, so their expiries are the same length and end together. It is armed when a notification arrives
    // and stopped when the toasts are withdrawn.
    expiryTimer_ = new QTimer(this);
    expiryTimer_->setSingleShot(true);
    connect(expiryTimer_, &QTimer::timeout, this, &ToastHost::dismissToasts);
}

QList<QWindow*> ToastHost::toasts() const
{
    QList<QWindow*> windows;
    windows.reserve(toasts_.size());
    for (const Toast& toast : toasts_) {
        if (toast.window != nullptr)
            windows.append(toast.window.data());
    }
    return windows;
}

void ToastHost::handleNotificationChanged()
{
    if (!service_->notificationAvailable()) {
        // The name was released. Withdrawn rather than left on screen: a toast standing after its message is
        // gone is a claim about a life the daemon has ended, and it is the other half of the reading — a
        // notification the daemon no longer holds is not one on screen.
        dismissToasts();
        return;
    }

    showToasts();
}

void ToastHost::showToasts()
{
    const QString application = service_->notificationApplication();
    const QString summary = service_->notificationSummary();
    const QString body = service_->notificationBody();

    bool created = false;
    for (QScreen* screen : QGuiApplication::screens()) {
        Toast* existing = nullptr;
        for (Toast& toast : toasts_) {
            if (toast.screen == screen) {
                existing = &toast;
                break;
            }
        }

        if (existing != nullptr && existing->window != nullptr) {
            // The newest wins, and the toast is redrawn for it rather than destroyed and rebuilt: a
            // notification updating an earlier one is a new message on a surface that is already up, and a
            // surface rebuilt for every update would be a configure round trip per notification.
            existing->window->setProperty("toastApplication", application);
            existing->window->setProperty("toastSummary", summary);
            existing->window->setProperty("toastBody", body);
            continue;
        }

        // The sender's own facts, handed over as initial properties rather than read from anywhere: a toast is
        // created when the notification arrives, and the component has nothing else to read them from.
        QObject* object = component_->createWithInitialProperties(
            {{QStringLiteral("toastApplication"), application},
             {QStringLiteral("toastSummary"), summary},
             {QStringLiteral("toastBody"), body}});
        auto* window = qobject_cast<QuantumShell::LayerShellWindow*>(object);
        if (window == nullptr) {
            // The component loaded but did not produce a layer-shell window, which is a defect in the QML
            // rather than a missing output: reported with the screen that exposed it, and the other outputs
            // still get theirs.
            qCWarning(quantum::app::waylandLog)
                << "the toast component did not create a layer-shell window for" << screen->name() << ":"
                << component_->errorString();
            delete object;
            continue;
        }

        // The screen is assigned here and the surface is presented after it, for the reason the bar's host
        // does: a layer surface is created against an output when the window is mapped and the role is
        // assigned once, so this has to happen first.
        window->setScreen(screen);
        window->present();
        toasts_.append(Toast{screen, window});
        created = true;
    }

    // The expiry, resolved here: the daemon publishes what the sender sent and the configuration says what a
    // `-1` means, so this is the one place that has both. Zero is the spec's never-expire, and a toast that
    // never expires is withdrawn when its notification is closed or the daemon's name is released, which is
    // what the reading's other half does above.
    const int sent = service_->notificationExpireTimeout();
    const int expiry = sent < 0 ? config_->bar()->notifications()->timeoutMs() : sent;
    if (expiry > 0)
        expiryTimer_->start(expiry);

    if (created)
        recheckVisibility();
}

void ToastHost::dismissToasts()
{
    if (toasts_.isEmpty())
        return;

    // Deleting the window is what destroys the surface, the way the bar's host does: a toast kept for an expiry
    // that has run out is one drawing nothing for the rest of its life.
    for (const Toast& toast : toasts_)
        delete toast.window.data();
    toasts_.clear();
    expiryTimer_->stop();
    recheckVisibility();
}

void ToastHost::handleScreenRemoved(QScreen* screen)
{
    for (int i = 0; i < toasts_.size(); ++i) {
        if (toasts_.at(i).screen != screen)
            continue;
        delete toasts_.at(i).window.data();
        toasts_.removeAt(i);
        qCInfo(quantum::app::waylandLog) << "toast withdrawn with output"
                                         << (screen != nullptr ? screen->name() : QString());
        recheckVisibility();
        return;
    }
}

void ToastHost::recheckVisibility()
{
    bool any = false;
    for (const Toast& toast : toasts_) {
        if (toast.window != nullptr && toast.window->isVisible()) {
            any = true;
            break;
        }
    }

    if (any == anyVisible_)
        return;
    anyVisible_ = any;
    Q_EMIT anyVisibleChanged();
}

}  // namespace quantum::app
