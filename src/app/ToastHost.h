#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QQmlComponent;
class QQmlEngine;
class QTimer;
class QScreen;
class QWindow;

namespace quantum::config {
class Config;
}
namespace quantum::dbus {
class NotificationService;
}

namespace quantum::app {

// The shell's toasts: one per output, created when a notification arrives and destroyed when their expiry
// runs out.
//
// It is the one surface in the shell that comes and goes, and this class is where that comes and goes lives:
// it watches the daemon's reading, creates a toast for each output the first time a notification is sent,
// updates the ones already up when another arrives — the newest wins, which is what a client updating a
// notification means — and destroys them all when the expiry is reached.
//
// The expiry is resolved here rather than in the daemon or in QML, because the daemon publishes what the
// sender sent and the configuration says what a `-1` means, so the resolution belongs to the one object that
// has both. And it is a class rather than QML because the lifetime is the business: a surface created on
// demand, per output, with its own clock, is more than a binding.
class ToastHost : public QObject
{
    Q_OBJECT

public:
    // Both must outlive this object, and neither is owned: the service and the configuration are the
    // composition root's, and the engine's are the windows it creates.
    explicit ToastHost(quantum::dbus::NotificationService& service, quantum::config::Config& config,
                       QQmlEngine& engine, const QUrl& toastUrl, QObject* parent = nullptr);

    // Whether the component was readable at all. A toast component that does not load means the shell has no
    // toast to draw, which the composition root turns into a failed process rather than a daemon answering
    // notifications that nothing draws.
    bool ready() const { return componentError_.isEmpty(); }
    QString componentError() const { return componentError_; }

    // The toasts that are up, in the order their screens arrived. A toast whose surface the compositor has
    // unmapped is still a window here, which is what the expiry ends; visibility is the window's own, not a
    // second copy kept alongside it.
    QList<QWindow*> toasts() const;

Q_SIGNALS:
    void anyVisibleChanged();

private Q_SLOTS:
    // The toast's own request, from a click: the notification is closed with the spec's "dismissed by the
    // user" and the toasts are withdrawn.
    void dismissFromToast();

private:
    struct Toast
    {
        QPointer<QScreen> screen;
        // A window rather than a QObject, because what the expiry is about is a surface being on screen —
        // `isVisible()` is a window's, and a toast the compositor has unmapped is one the expiry ends.
        QPointer<QWindow> window;
    };

    void handleNotificationChanged();
    void handleNotificationClosed(quint32 id);
    void expire();
    void showToasts();
    void dismissToasts();
    void handleScreenRemoved(QScreen* screen);
    void recheckVisibility();

    QQmlComponent* component_ = nullptr;
    QString componentError_;
    QList<Toast> toasts_;
    QTimer* expiryTimer_ = nullptr;
    bool anyVisible_ = false;
    // The id of the notification the toasts are showing, zero when none are: what a `NotificationClosed`
    // is compared with, and what the expiry reports as expired.
    quint32 shownId_ = 0;

    // Not owned, and not weak pointers either: the service and the configuration are created by the
    // composition root and live as long as the shell does, unlike the windows this class holds. Those are its
    // own business — it follows them going away — which is why what is kept here is the two objects rather
    // than copies of the facts they publish.
    quantum::dbus::NotificationService* service_ = nullptr;
    quantum::config::Config* config_ = nullptr;
};

}  // namespace quantum::app
