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
    connect(&service, &quantum::dbus::NotificationService::notificationDoNotDisturbChanged, this,
            &ToastHost::handleDoNotDisturbChanged);
    connect(&service, &quantum::dbus::NotificationService::NotificationClosed, this,
            &ToastHost::handleNotificationClosed);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &ToastHost::handleScreenRemoved);

    // One timer for the whole set rather than one per window: a single notification is one message on every
    // output, so their expiries are the same length and end together. It is armed when a notification arrives
    // and stopped when the toasts are withdrawn.
    expiryTimer_ = new QTimer(this);
    expiryTimer_->setSingleShot(true);
    connect(expiryTimer_, &QTimer::timeout, this, &ToastHost::expire);
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

    // The service also announces a change when the shell *becomes* the daemon — at a handover from another
    // notifier that exited, say — and that change carries no notification: there is no id showing and the
    // reading is empty. A toast created for it would be a blank message on every output for as long as the
    // default expiry, so a change is a toast only when a notification is what changed.
    if (service_->currentNotificationId() == 0)
        return;

    // Do Not Disturb: the notification was received, answered and recorded — that is the daemon's part — and
    // nothing shows it. It is over as far as the shell is concerned, so its sender is told rather than left
    // waiting on a notification that will never be closed by anyone (the spec's "undefined" reason, the same one
    // a displaced notification gets).
    if (service_->notificationDoNotDisturb()) {
        service_->closeNotification(service_->currentNotificationId(), quantum::dbus::CloseReason::Undefined);
        return;
    }

    showToasts();
}

void ToastHost::handleDoNotDisturbChanged()
{
    // Switching it on takes down what is up: a person who asked not to be disturbed is not helped by a toast
    // that is on screen until its clock runs out. Switching it off shows nothing — what was suppressed is in the
    // history, and replaying it would be a burst of stale toasts.
    if (!service_->notificationDoNotDisturb())
        return;
    const quint32 id = shownId_;
    dismissToasts();
    service_->closeNotification(id, quantum::dbus::CloseReason::Undefined);
}

void ToastHost::handleNotificationClosed(quint32 id)
{
    // The sender closed the notification the toasts are showing, so what they show is gone. A close for a
    // notification an earlier toast showed is not this one's business: the newest took the surface.
    if (id != 0 && id == shownId_)
        dismissToasts();
}

void ToastHost::dismissFromToast()
{
    // Every output's toast shows the same message, so a click on any of them is a click on the notification.
    const quint32 id = shownId_;
    dismissToasts();
    service_->closeNotification(id, quantum::dbus::CloseReason::Dismissed);
}

void ToastHost::expire()
{
    // Dismissed first and reported after, so the daemon's own `NotificationClosed` finds nothing left to
    // dismiss. The sender hears that its notification expired — the spec's reason 1 — rather than nothing.
    const quint32 id = shownId_;
    dismissToasts();
    service_->closeNotification(id, quantum::dbus::CloseReason::Expired);
}

void ToastHost::showToasts()
{
    // An entry whose window or screen is gone would make the loop below build a second window for an output
    // that still has an entry, so it is dropped before the outputs are walked.
    toasts_.removeIf([](const Toast& toast) { return toast.window == nullptr || toast.screen == nullptr; });
    shownId_ = service_->notificationAvailable() ? service_->currentNotificationId() : 0;

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
        // The window's own signal, connected by name because the signal is declared in the QML component, and
        // queued: the slot deletes the window, and Qt refuses (fatally) to destroy an object while one of its
        // own QML handlers is still running — which a direct connection from the click handler would be.
        connect(window, SIGNAL(dismissRequested()), this, SLOT(dismissFromToast()), Qt::QueuedConnection);
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
    // A notification that never expires must also cancel the clock an earlier one armed: left running, it
    // would withdraw a toast the sender asked to keep.
    if (expiry > 0)
        expiryTimer_->start(expiry);
    else
        expiryTimer_->stop();

    if (created)
        recheckVisibility();
}

void ToastHost::dismissToasts()
{
    shownId_ = 0;
    expiryTimer_->stop();
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
