#include "dbus/NotificationService.h"
#include "QmlModule.h"
#include "app/Logging.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QQmlEngine>
#include <QVariantMap>

namespace quantum::dbus {

NotificationService::NotificationService(const QDBusConnection& bus, QObject* parent)
    : QObject(parent), bus_(bus) {
    // The service watcher is what makes a lost name come back: if another daemon holds
    // `org.freedesktop.Notifications` when the shell starts, the shell has joined the queue for it, and
    // this is what tells it when the holder went away so it can ask again. It also covers the bus
    // itself being unavailable at construction, which a constructor cannot retry on its own.
    serviceWatcher_ = new QDBusServiceWatcher(
        QString::fromLatin1(NotificationsServiceName), bus_,
        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration, this);

    connect(serviceWatcher_, &QDBusServiceWatcher::serviceRegistered, this,
            &NotificationService::onServiceRegistered);
    connect(serviceWatcher_, &QDBusServiceWatcher::serviceUnregistered, this,
            &NotificationService::onServiceUnregistered);
}

NotificationService::NotificationService(QObject* parent) : QObject(parent) {
    // The desktop's senders are on the session bus, so that is the bus the composition root's service
    // registers on. The constructor above is the one a test uses with a `dbus-daemon` of its own, for
    // the same reason `network-test` starts one.
    bus_ = QDBusConnection::sessionBus();
    serviceWatcher_ = new QDBusServiceWatcher(
        QString::fromLatin1(NotificationsServiceName), bus_,
        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration, this);

    connect(serviceWatcher_, &QDBusServiceWatcher::serviceRegistered, this,
            &NotificationService::onServiceRegistered);
    connect(serviceWatcher_, &QDBusServiceWatcher::serviceUnregistered, this,
            &NotificationService::onServiceUnregistered);
}

NotificationService::~NotificationService() = default;

void NotificationService::start() {
    // Cleared before the ask. A registration report delivered from the ask itself is this start, not a
    // late report from `stop()`, and a shell that stood down has to be able to take the name again.
    stopped_ = false;
    registerService();
}

bool NotificationService::registerService() {
    if (!bus_.isConnected()) {
        qCWarning(app::notificationLog) << "Cannot register notifications on a disconnected bus";
        return false;
    }

    // The object has to be exported before the name is asked for: a sender that resolves the name reaches
    // the object at the spec's path. The export is `ExportScriptableSlots` rather than `ExportAllSlots`
    // because `ExportAllSlots` publishes the methods under no interface name, so a caller that addresses
    // `org.freedesktop.Notifications` — every sender in the spec — is told there is no such interface.
    // `Q_CLASSINFO("D-Bus Interface", ...)` in the header names it, and this export honours that name. The
    // scriptable *signals* are exported with it, because `NotificationClosed` is how a sender learns that its
    // notification is gone.
    //
    // Already exported by *this* connection is not a failure, and the first, queued ask is what leaves it
    // that way: the name was somebody else's, so only the export happened. The later moment this runs again
    // is the bus handing the shell the name it queued for, and stopping on that export would leave the
    // shell answering `Notify` while publishing that it is not the daemon. `objectRegisteredAt` is the
    // connection's own answer about what is exported at that path, so what is compared here is the bus
    // connection's state rather than a flag of this class that could drift from it.
    const QString objectPath = QString::fromLatin1(NotificationsObjectPath);
    if (!bus_.registerObject(objectPath, this, QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals)
        && bus_.objectRegisteredAt(objectPath) != this) {
        qCWarning(app::notificationLog) << "Could not register the notifications object on the bus";
        return false;
    }

    // `QueueService` rather than `DontQueueService`: the name is taken rather than stolen, so a desktop
    // running another notifier keeps it and the shell waits as a queue position instead of failing.
    //
    // Which reply means what was measured on this Qt against a `dbus-daemon` a test started, one case at a
    // time: a free name answers `ServiceRegistered`; a name another connection holds answers
    // `ServiceQueued`; a name this connection already owns answers `ServiceRegistered` again. So only
    // `ServiceRegistered` is "the shell is the daemon", and the test for it is not an inference from the
    // spec. `ServiceQueued` is the shell waiting behind a holder that has not gone away, which means another
    // process answers `Notify` right now — publishing `notificationAvailable` for it would be a claim that
    // notifications arrive here when they arrive there, and a readout drawing a summary the shell was never
    // sent. (Measured too: `DontQueueService` against a name held elsewhere answers `ServiceNotRegistered`,
    // which is why a refusal and "somebody else has it" cannot be told apart in that spelling, and why this
    // asks with the queue.)
    //
    // The queue is what gets the shell the name later, when the holder releases it: the bus grants it to the
    // queued request, its report of that registration brings this method back here, and the ask below then
    // answers `ServiceRegistered` because the name is this connection's by then — the path the export above
    // must not stop on, which `notification-test`'s
    // `aQueuedShellBecomesTheDaemonWhenTheHolderLeaves` pins.
    const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> registration =
        bus_.interface()->registerService(QString::fromLatin1(NotificationsServiceName),
                                          QDBusConnectionInterface::QueueService);
    if (!registration.isValid()) {
        qCWarning(app::notificationLog) << "Could not request the notifications name:"
                                        << registration.error().message();
        return false;
    }

    if (registration.value() != QDBusConnectionInterface::ServiceRegistered) {
        qCInfo(app::notificationLog) << "Queued for the notifications name; another daemon holds it";
        return false;
    }

    qCInfo(app::notificationLog) << "Registered as the desktop's notification daemon";
    // Published only on a real change. The bus's report of a registration is what can bring this method
    // back here, and by then the name is this connection's, so the ask above answers `ServiceRegistered`
    // again — measured, as is the same answer for a name this connection already owns. Setting and
    // emitting the same state a second time would be the signal saying something moved when nothing did,
    // which is the one thing a widget binding on it cannot tell apart from a notification arriving.
    if (!notificationAvailable_) {
        notificationAvailable_ = true;
        emit notificationChanged();
    }
    return true;
}

quint32 NotificationService::Notify(const QString& appName, quint32 replacesId, const QString& appIcon,
                                    const QString& summary, const QString& body,
                                    const QStringList& actions, const QVariantMap& hints,
                                    qint32 expireTimeout) {
    Q_UNUSED(appIcon)
    Q_UNUSED(actions)
    Q_UNUSED(hints)

    // `replaces_id` is the caller's own handle for a notification it is updating, so the spec says to
    // answer with the same id rather than a new one — a client that updates a notification twice and
    // gets two ids has two notifications where it asked for one. Zero is the spec's "no id", and a
    // client cannot be updating a notification it never made, so a zero replaces_id is treated as new.
    //
    // A new id is one nobody is holding. The counter alone is not that: a sender may hand back any
    // `replaces_id` it likes and the daemon echoes it, so the counter can arrive at the id that is showing,
    // and two notifications with one id are a `CloseNotification` that closes the wrong one. It also wraps
    // to zero, which is the spec's "no id" and is never returned.
    quint32 id = replacesId;
    if (id == 0) {
        do {
            id = ++nextId_;
        } while (id == 0 || id == currentId_);
    }
    const quint32 displaced = (currentId_ != 0 && currentId_ != id) ? currentId_ : 0;
    currentId_ = id;

    notificationApplication_ = appName;
    notificationSummary_ = summary;
    notificationBody_ = body;
    notificationExpireTimeout_ = expireTimeout;
    ++notificationCount_;

    qCInfo(app::notificationLog) << "Notification" << id << "from"
                                 << (appName.isEmpty() ? QStringLiteral("(unnamed)") : appName) << ":"
                                 << summary;

    emit notificationChanged();

    // The notification this one took the place of is gone from the screen and from the readout, and its
    // sender is told so rather than left waiting on a notification that will never be closed by anyone.
    if (displaced != 0)
        emit NotificationClosed(displaced, static_cast<quint32>(CloseReason::Undefined));
    return id;
}

void NotificationService::CloseNotification(quint32 id) {
    // The spec's contract: a daemon must accept this call, and a client may send it for a notification
    // whose id the daemon no longer holds — which closes nothing and is not an error. When the id is the
    // one showing, the notification is closed and the sender hears `NotificationClosed` with the reason
    // the spec gives for a close it asked for.
    const bool closed = closeNotification(id, CloseReason::Closed);
    qCInfo(app::notificationLog) << "CloseNotification for id" << id
                                 << (closed ? "closed it" : "was not showing");
}

bool NotificationService::closeNotification(quint32 id, CloseReason reason) {
    if (id == 0 || id != currentId_)
        return false;
    currentId_ = 0;
    emit NotificationClosed(id, static_cast<quint32>(reason));
    return true;
}

QStringList NotificationService::GetCapabilities() {
    // The capabilities this daemon actually has. `body` is listed because the toast draws the sender's body.
    // `body-markup` is not: the toast draws every text as plain text, so a `<b>` in a body would be shown
    // as the characters the sender typed, and a capability a daemon claims but does not honour is one a
    // sender plans around — libnotify strips markup for a daemon that does not claim it, which is what
    // makes plain text the honest answer. `actions` and `icons` are not listed either, because nothing in
    // the shell acts on either one.
    return {QStringLiteral("body")};
}

QString NotificationService::GetServerInformation(QString& vendor, QString& version,
                                                  QString& specVersion) {
    // What a sender asks to know who it is talking to. The spec version is the one this daemon
    // implements — stated rather than discovered, because a daemon that reports a spec it has not read
    // is the same kind of lie as a capability it does not honour.
    vendor = QStringLiteral("Quantum Shell");
    version = QStringLiteral("0.1.0");
    specVersion = QStringLiteral("1.2");
    return QStringLiteral("quantum-shell");
}

void NotificationService::onServiceRegistered(const QString& serviceName) {
    if (serviceName != QString::fromLatin1(NotificationsServiceName))
        return;
    // The signal says a name was registered; it does not say by whom, and it fires for this shell's own
    // registration as well as for another process taking the name. Whose it is matters, because the two
    // call for opposite things — this is the moment the shell that queued for the name asks again — so the
    // question is what the ask comes back with. Measured, asking for a name this connection already owns
    // answers `ServiceRegistered`, not a queue position, so the second ask reaches the same line as the
    // first and publishes the same state (once: the publish above is guarded by the value it sets).
    //
    // The guard below is about that second ask being unnecessary work and a second line in the record, not
    // about the state: the shell is already the daemon and there is nothing for the report to tell it.
    //
    // `notificationAvailable_` is also false while the shell is queued behind another daemon, and that
    // report is when this method must ask again. `stop()` is a different false. The shell stood down on
    // purpose, and a report of its own earlier registration — queued on the bus, so it can arrive after
    // the name was released — must not take the name back. `stopped_` is that case, set by `stop()` and
    // cleared only by `start()`. `aRegistrationReportFromBeforeStopDoesNotReacquireTheName` pins it.
    if (stopped_)
        return;
    if (!notificationAvailable_)
        registerService();
}

void NotificationService::onServiceUnregistered(const QString& serviceName) {
    if (serviceName != QString::fromLatin1(NotificationsServiceName))
        return;
    // `stop()` releases the name itself, and the bus reports that release as this signal — but not
    // synchronously inside `unregisterService()`. The signal is queued on the bus, so it can arrive
    // after a later `start()` has taken the name back: measured in the order check, where `stop()`,
    // `start()` and then the bus's delayed `NameLost` for the old registration arrived in that order,
    // and clearing the reading unconditionally withdrew a reading the shell owned. The same lie in the
    // other direction — a daemon that is the daemon reporting it is not.
    //
    // The bus is the arbiter of who owns the name, and this signal is not it: the signal is a
    // one-time report of a change, and the question is what the answer is *now*. So the reading follows
    // the owner the bus names, which is this connection or nobody. A name owned by another process is
    // the shell standing down, whether the release was this shell's `stop()` or another daemon's exit.
    const QDBusReply<QString> owner = bus_.interface()->serviceOwner(
        QString::fromLatin1(NotificationsServiceName));
    if (owner.isValid() && owner.value() == bus_.baseService())
        return;
    qCInfo(app::notificationLog) << "The notifications name was released";
    clearReading();
}

// Releasing the name is the shell stepping down as the daemon, which is the same event the watcher
// reports when another daemon's exit releases it. The reading has to go with it, and the name has to
// go before the object, because a sender resolving the name between the two would reach an object
// that no longer answers it. `stopped_` is set first, before the disconnected-bus return and before the
// name is released, so a registration report already queued cannot re-enter `registerService()`.
void NotificationService::stop() {
    stopped_ = true;
    if (!bus_.isConnected())
        return;
    bus_.unregisterService(QString::fromLatin1(NotificationsServiceName));
    bus_.unregisterObject(QString::fromLatin1(NotificationsObjectPath));
    clearReading();
}

void NotificationService::clearReading() {
    currentId_ = 0;
    notificationAvailable_ = false;
    notificationSummary_.clear();
    notificationBody_.clear();
    notificationApplication_.clear();
    emit notificationChanged();
}

void NotificationService::registerQmlSingleton(NotificationService& service) {
    qmlRegisterSingletonInstance(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion,
                                 quantum::qml::ModuleMinorVersion, "NotificationService", &service);
}

}  // namespace quantum::dbus
