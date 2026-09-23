// The desktop's notification daemon, which the shell *is* rather than talks to.
//
// The freedesktop notification spec is a D-Bus service: a process that owns the well-known name
// `org.freedesktop.Notifications` and answers `Notify` on the object `/org/freedesktop/Notifications`.
// `notify-send`, `kdialog`, every library that calls `libnotify` — all of them send a method call to
// that name and expect an id back. So the shell being the daemon is what makes its notifications real,
// and it is the shape every other reading in this shell already has: nothing polls, the desktop's own
// client delivers a call when a notification exists, and the shell's state changes because of it.
//
// The name is not unconditionally the shell's. Another daemon may already hold it (a desktop that runs
// one alongside the shell, or a session where the shell is not the chosen notifier), and D-Bus hands the
// name to whoever asks first without an opinion about which one that should be. So the request is made
// with `QueueService`: the shell joins the queue rather than stealing the name, and whether it is the
// daemon is decided by the bus's three-state reply rather than by a local flag. A shell that is not the
// daemon publishes that honestly rather than recording notifications it will never be sent.
//
// The id `Notify` returns is the caller's handle for the notification it just made (it sends the same
// id as `replaces_id` to update one, and `CloseNotification` takes it), and an id that repeats or goes
// backwards would collide two notifications. Zero is the spec's "no id yet" and is never returned.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDBusConnection>
#include <QDBusServiceWatcher>

namespace quantum::dbus {

// The freedesktop names, declared once because they are interface: a `notify-send` that works is a
// `notify-send` that resolved them, and a second spelling in a test is a second spelling that can drift.
inline constexpr auto NotificationsServiceName = "org.freedesktop.Notifications";
inline constexpr auto NotificationsObjectPath = "/org/freedesktop/Notifications";
inline constexpr auto NotificationsInterface = "org.freedesktop.Notifications";

class NotificationService : public QObject {
    Q_OBJECT
    // The interface name a sender addresses. `ExportScriptableSlots` publishes the slots under this
    // name and under no other, so a call to `org.freedesktop.Notifications` is the only way in.
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")

    // Whether this shell is the desktop's notification daemon. False while the name is held by another
    // process or the bus is unreachable, and the readout's honest empty state draws on it.
    Q_PROPERTY(bool notificationAvailable READ notificationAvailable NOTIFY notificationChanged)

    // The summary of the most recent notification — the spec's `summary` argument, the line a person
    // wrote as the subject of the message. Read directly rather than derived so a widget shows the
    // sender's own text.
    Q_PROPERTY(QString notificationSummary READ notificationSummary NOTIFY notificationChanged)

    // The body of the most recent notification, the spec's `body` argument. Empty is a real state: the
    // spec allows a notification with a summary and no body, and a daemon that invented one would be
    // putting words in a sender's mouth.
    Q_PROPERTY(QString notificationBody READ notificationBody NOTIFY notificationChanged)

    // The application name the sender gave itself, the spec's `app_name` argument. A sender may omit it,
    // and the daemon's answer is to publish what it was given.
    Q_PROPERTY(QString notificationApplication READ notificationApplication NOTIFY notificationChanged)

    // How many notifications this daemon has received, over the life of the service. A count rather than a
    // list because nothing draws a list, and it is deliberately not withdrawn with the rest of the reading
    // when the name is released: it counts what this process was sent, which a demotion does not un-send.
    // No widget draws it today.
    Q_PROPERTY(int notificationCount READ notificationCount NOTIFY notificationChanged)

public:
    // The bus to register on, defaulting to the session's because that is where the desktop's senders
    // are. A test passes its own `dbus-daemon` so its claims are about this shell rather than about
    // whatever the desktop happens to run — the same rule `network-test`'s private bus follows.
    explicit NotificationService(const QDBusConnection& bus, QObject* parent = nullptr);
    explicit NotificationService(QObject* parent = nullptr);
    ~NotificationService() override;

    bool notificationAvailable() const { return notificationAvailable_; }
    QString notificationSummary() const { return notificationSummary_; }
    QString notificationBody() const { return notificationBody_; }
    QString notificationApplication() const { return notificationApplication_; }
    int notificationCount() const { return notificationCount_; }

    // Owns `org.freedesktop.Notifications` on the bus it was constructed with, or joins the queue for
    // it. Called by the composition root once, *before* the QML engine loads rather than after, because
    // this service owns a bus name instead of subscribing to one: a desktop with no other notifier has
    // nothing sending notifications until the shell is listening, so the name is taken before the bar
    // exists. Nothing here is on a timer — the watcher below is what asks again, and it asks when the bus
    // reports that the name changed hands.
    //
    // An ask that comes back `ServiceQueued` does not publish availability, because another daemon is
    // answering `Notify` right then — but it is not a dead end. The queue is what gets this shell the name
    // when that holder leaves: the bus grants it, reports the registration, and the registration publishes
    // then, which is why a second entry is safe and why this is idempotent. `QUANTUM_SHELL.md` describes the
    // handover and the test that pins it.
    // After `stop()`, the stood-down flag is cleared before the ask, so the shell can be the daemon again.
    void start();

    // Releases the name and the object and withdraws the reading. The inverse of `start()`, and the path a
    // shutdown takes on the bus; without it a shell that stood down would keep drawing a notification it
    // is no longer the daemon for. It stays stood down until `start()`: `stopped_` is set before the name
    // is released, including when the bus is already disconnected, and `onServiceRegistered` does not call
    // `registerService()` while that flag is set, so a report of this shell's own earlier registration
    // cannot take the name back. `notificationAvailable_` cannot carry that: it is also false while the
    // shell is queued behind another daemon, which is the handover that must still ask.
    void stop();

    // The registered type name, mirror of the module constants the other services use.
    static void registerQmlSingleton(NotificationService& service);

signals:
    // One signal for every property, because every property moves together: a notification replaces the
    // summary, the body and the count in one step.
    void notificationChanged();

public slots:
    // The spec's methods, answered on the bus. `Notify` is the one a sender uses to deliver a
    // notification; the other three are the spec's contract for what a daemon must also answer.
    // `Q_SCRIPTABLE` on each one is what makes it reachable: a slot without it is a C++ slot only, and
    // Qt's bus dispatch refuses to call it, answering "No such method" to a sender that did nothing
    // wrong. The interface name comes from `Q_CLASSINFO` above, so a caller that addresses
    // `org.freedesktop.Notifications` is the only way in.
    Q_SCRIPTABLE quint32 Notify(const QString& appName, quint32 replacesId, const QString& appIcon,
                                const QString& summary, const QString& body, const QStringList& actions,
                                const QVariantMap& hints, qint32 expireTimeout);
    Q_SCRIPTABLE void CloseNotification(quint32 id);
    Q_SCRIPTABLE QStringList GetCapabilities();
    Q_SCRIPTABLE QString GetServerInformation(QString& vendor, QString& version, QString& specVersion);

private slots:
    void onServiceRegistered(const QString& serviceName);
    void onServiceUnregistered(const QString& serviceName);

private:
    bool registerService();
    void clearReading();

    QDBusConnection bus_ = QDBusConnection::sessionBus();
    QDBusServiceWatcher* serviceWatcher_ = nullptr;
    quint32 nextId_ = 0;
    bool notificationAvailable_ = false;
    // Set by `stop()`, cleared by `start()`. Separate from `notificationAvailable_`: that one is also false
    // while this shell is queued behind another daemon and must still ask when the bus reports the name.
    bool stopped_ = false;
    QString notificationSummary_;
    QString notificationBody_;
    QString notificationApplication_;
    int notificationCount_ = 0;
};

}  // namespace quantum::dbus
