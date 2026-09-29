// The notification daemon's own contract: the shell is `org.freedesktop.Notifications`, so a
// `notify-send` on this desktop is a D-Bus call the shell answers.
//
// The pure half is tested by calling the service through a real bus rather than by calling its methods
// directly: a `Notify` that arrives over D-Bus has been marshalled, demarshalled and dispatched by Qt's
// bus layer, which is the path a real sender takes, and a slot signature that does not match the spec's
// argument types is rejected there before it ever reaches the method. That is the defect a direct call
// cannot see, and it is the one that would make the shell a daemon nothing can talk to.
//
// The bus is a `dbus-daemon` this test starts in memory of its own, so `org.freedesktop.Notifications`
// is free for the shell to take here and can never be taken from it — the desktop's own notification
// daemon holds that name on the session, and a test that shared the session bus would be queued behind
// it and exercising a daemon that is not the shell's. That is the same rule `network-test`'s private
// bus follows: a test names its own instance of the real thing.
#include "dbus/NotificationService.h"
#include "NotificationBus.h"

#include "app/ToastHost.h"
#include "config/Config.h"
#include "wayland/LayerShellWindow.h"

#include <QDBusConnection>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QDBusPendingCallWatcher>

#include "app/ToastHost.h"
#include "config/Config.h"

#include <QQmlEngine>
#include <QUrl>

#include <QGuiApplication>
#include <QQmlEngine>
#include <QScreen>
#include <QWindow>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include <memory>

namespace {
// The bus name the service registers under, as Qt's bus layer spells it. A connection named the same
// way in two places is a connection that can be disconnected from in one of them.
constexpr auto kBusName = "quantum-shell-notification-test";

// The bus of this test's own, which is `NotificationBus`: a `dbus-daemon` started here with a service
// directory holding the shell's own service file and nothing else, so the notifications name is this
// shell's to take and no other daemon can answer a sender that resolves it. The reasoning behind it —
// why the desktop's session bus will not do, and why the directory is this process's own — is written
// at the class, where both of this suite's notification binaries read it.

}  // namespace

// What a sender that waits on its notifications hears: every `NotificationClosed(id, reason)` the daemon put on
// the bus, in the order it arrived. Connected from a second connection, so it is the wire that is read and
// not the service's own C++ signal.
class ClosedRecorder : public QObject {
    Q_OBJECT
public:
    struct Closed {
        quint32 id;
        quint32 reason;
    };
    QList<Closed> seen;

public slots:
    void closed(quint32 id, quint32 reason) { seen.append({id, reason}); }
};

class NotificationTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void theShellTakesTheNotificationsName();
    void theNotifyMethodAnswersWithAnId();
    void replacesIdIsAnsweredWithTheSameId();
    void notifyPublishesTheSendersOwnText();
    void getServerInformationNamesTheDaemon();
    void getCapabilitiesReportsWhatTheDaemonHas();
    void aNotifySendReachesTheService();
    void closingTheNameWithdrawsTheReading();
    void aQueuedShellBecomesTheDaemonWhenTheHolderLeaves();
    void aDemotionSignalFromBeforeAReRegistrationDoesNotWithdrawAReadingHeldNow();
    void aRegistrationReportFromBeforeStopDoesNotReacquireTheName();
    void serviceRegistersWithQml();
    void aNotificationCreatesOneToastPerOutput();
    void theToastDrawsTheSendersOwnFacts();
    void theExpiryTheSenderAskedForIsTheOneTheToastLivesFor();
    void aCloseForTheShowingNotificationIsAnsweredWithNotificationClosed();
    void aCloseForAnIdThatIsNotShowingSaysNothing();
    void aNewerNotificationClosesTheOneItDisplaced();
    void aNewIdIsNeverTheOneThatIsShowing();
    void aSenderClosingItsNotificationWithdrawsTheToast();
    void aNeverExpiringNotificationOutlivesTheClockAnEarlierOneArmed();
    void aToastIsNotTheSizeOfTheScreen();
    void aClickOnAToastDismissesItAndTheSenderIsTold();
    void becomingTheDaemonCreatesNoToast();

private:
    void closeOverTheWire(quint32 id);
    void listenForClosed(ClosedRecorder& recorder);
    bool settle();
    // The bus this test's daemon registers on, and the one a sender is pointed at.
    qstest::NotificationBus bus_;
    QDBusConnection connection_ = QDBusConnection::sessionBus();
    std::unique_ptr<quantum::dbus::NotificationService> service_;

    // The toast host and the engine its component is created in, against the service this test starts and the
    // configuration this test's own. Declared here rather than in a slot because the toast's lifetime is the
    // business: a toast created in one slot and expected in another would be a lifetime that outlives the slot
    // that made it.
    std::unique_ptr<quantum::app::ToastHost> toasts_;
    std::unique_ptr<QQmlEngine> engine_;
    quantum::config::Config config_;

    QDBusInterface daemon() const;
    QDBusConnection sender() const;

    // One run of the desktop's own sender against this test's bus: whether it started at all, whether it
    // stopped inside the wait, what it exited with, and what it wrote to stderr. `notify-send` is the one
    // program this test does not own, and the two things that can be wrong with it — it is not installed, or
    // it is installed and cannot run — are conditions of the machine rather than defects of the daemon, so
    // both are read here and reported by the slot that asked for the run.
    struct SenderRun {
        bool started = false;
        bool finished = false;
        int exitCode = -1;
        QString standardError;
    };
    SenderRun runNotifySend(const QString& program, const QStringList& arguments);

    // Whether the bus hands the shell a name it asked for while somebody else held it, and whether the shell
    // then behaves like the daemon: waited for rather than slept on, because the handover is a signal and
    // nothing in this test orders it against the holder's exit.
    bool waitUntilAvailable(int timeoutMs);

    // Makes a `Notify` call the way a sender does and returns the id the daemon answered with, or
    // reports the failure the way the spec's caller would see it. Asynchronous, because the daemon
    // this test calls is in the same process and a blocking call would hold the event loop that has to
    // deliver it.
    quint32 notify(const QString& summary, const QString& body, quint32 replacesId,
                   qint32 expireTimeout = -1);
    QDBusMessage call(const QString& method);
    QDBusMessage waitForReply(QDBusPendingCall pending, const QString& methodName) const;
};

void NotificationTest::initTestCase() {
    // The bus, or a skip that says why: `dbus-daemon` not existing is a fact about the machine, and the
    // claims below are about a shell that is the desktop's notification daemon on one. The message is the
    // bus's own, because "no dbus-daemon" and "a daemon that would not serve" are different situations and
    // only the attempt knows which one happened.
    if (!bus_.start(QStringLiteral(FIXTURES)))
        QSKIP(qPrintable(bus_.error()));

    connection_ = QDBusConnection::connectToBus(bus_.address(), QString::fromLatin1(kBusName));
    if (!connection_.isConnected())
        QSKIP("Could not connect to this test's own bus");

    // The service is constructed against the test's bus rather than the session's, which is what makes
    // every claim below a claim about this shell rather than about whatever the desktop happens to run.
    // `start()` is the composition root's call, kept here for the same reason it is kept there: a
    // registration that fails is reported rather than silently swallowed.
    service_ = std::make_unique<quantum::dbus::NotificationService>(connection_);
    service_->start();

    // The toast host, against the service this test starts and the configuration this test's own. The toast's
    // component is loaded from the source tree rather than from a resource, for the same reason the bar's is
    // in `bar-interaction-test`: the packed copies load in a real shell, which is what the layer-shell live
    // tests assert on the built binary. The configuration is registered before the host is constructed,
    // because the component is created in that constructor and reads `Config.bar.*` — a singleton registered
    // after the component is created is one the component never saw. And the type the toast's component is
    // made of is registered the way the composition root does: a component whose root is a
    // `LayerShellWindow` cannot load in an engine that has not been told about it.
    engine_ = std::make_unique<QQmlEngine>();
    qmlRegisterType<QuantumShell::LayerShellWindow>("QuantumShell", 1, 0, "LayerShellWindow");
    quantum::config::Config::registerQmlSingleton(config_);
    toasts_ = std::make_unique<quantum::app::ToastHost>(*service_, config_, *engine_,
                                                        QUrl::fromLocalFile(QStringLiteral(QS_TOAST_QML)));
    QVERIFY2(toasts_->ready(), qPrintable(toasts_->componentError()));

    // The toast's default expiry, made short here for the same reason every wait in this suite is short: a
    // `-1` resolves to whatever the configuration says, and the suite waits for that expiry, so the value is
    // the configuration's own floor rather than the shipped default of 5000. The mechanism is the same number
    // at either length, and what is being tested is that a `-1` resolves to the configuration's default rather
    // than to one baked into the daemon.
    quantum::config::ConfigValues values;
    values.bar.notifications.timeoutMs = 500;
    config_.apply(values);
}

void NotificationTest::cleanupTestCase() {
    if (service_)
        connection_.unregisterService(QString::fromLatin1(quantum::dbus::NotificationsServiceName));
    QDBusConnection::disconnectFromBus(QString::fromLatin1(kBusName));
}

// A second connection to the same private bus, so the call is made from outside the service's own
// dispatch. A call on the service's own connection is a deadlock in both directions: the blocking
// `call()` holds the event loop that would deliver the message to the object it is calling, and the
// daemon never gets to answer. This is what a real sender looks like on the wire — a different
// connection, the same bus, no shared event loop.
QDBusConnection NotificationTest::sender() const {
    return QDBusConnection::connectToBus(bus_.address(), QString::fromLatin1(kBusName) + "-sender");
}

// One run of the sender, with the wait this test's shape forces on every program it launches itself.
//
// `notify-send` reads the bus address from its environment, and the test's bus is already the only daemon it
// can reach: the service directory the bus was given contains the shell's service file and nothing else, so
// the desktop's own notifier is not a thing it can activate here. Nothing else about the desktop's bus is
// inherited, because `dbus-daemon` does not read it either.
NotificationTest::SenderRun NotificationTest::runNotifySend(const QString& program,
                                                            const QStringList& arguments) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), bus_.address());

    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessEnvironment(environment);
    process.start();

    SenderRun run;
    run.started = process.waitForStarted(5000);
    if (!run.started) {
        run.standardError = process.errorString();
        return run;
    }

    // `waitForFinished` cannot be used here, although it is the obvious call: the daemon this sender is
    // asking is in this process, and a blocking wait holds the event loop that dispatches the message it
    // sent, so the sender would wait for a reply that can never be written. This is the same deadlock the
    // async helpers above exist for, reached from the other end. So the wait is a poll that keeps
    // dispatching until the sender is done, which is what a desktop running the shell as its daemon is doing
    // the whole time.
    QElapsedTimer finished;
    finished.start();
    while (process.state() != QProcess::NotRunning && finished.elapsed() < 5000)
        QCoreApplication::processEvents();

    run.finished = process.state() == QProcess::NotRunning;
    if (!run.finished) {
        // Killed rather than left running, and the exit code stays the "never ran to completion" the caller
        // reads: a program this test had to kill did not exit, and reporting the signal as its status would
        // be this test answering for it.
        process.kill();
        process.waitForFinished(1000);
        return run;
    }

    run.exitCode = process.exitCode();
    run.standardError = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    return run;
}

// Any spec method that takes no arguments, asked the way a sender asks it and answered the way a
// daemon answers — asynchronously, with the event loop pumped, because the daemon is in this process
// and a blocking `call()` would hold the loop that has to dispatch the message it sent.
QDBusMessage NotificationTest::call(const QString& method) {
    QDBusMessage message = QDBusMessage::createMethodCall(
        QString::fromLatin1(quantum::dbus::NotificationsServiceName),
        QString::fromLatin1(quantum::dbus::NotificationsObjectPath),
        QString::fromLatin1(quantum::dbus::NotificationsInterface), method);

    return waitForReply(sender().asyncCall(message), method);
}

// The one wait the whole test's shape depends on. The daemon this test calls is in the same process,
// so the reply cannot arrive until the event loop turns — a blocking `call()` would hold the very loop
// that has to dispatch the message it sent, and the wait would end in a timeout reported as a refusal
// by a daemon that was never asked. Pumping the loop is what makes the round trip real rather than
// self-answered, and the timeout is what says the daemon did not answer rather than the test giving up.
QDBusMessage NotificationTest::waitForReply(QDBusPendingCall pending, const QString& methodName) const {
    QDBusPendingCallWatcher watcher(pending);
    QElapsedTimer elapsed;
    elapsed.start();
    while (!watcher.isFinished() && elapsed.elapsed() < 5000)
        QCoreApplication::processEvents();
    if (!watcher.isFinished()) {
        QTest::qFail(qPrintable(methodName + QStringLiteral(" did not answer")), __FILE__, __LINE__);
        return QDBusMessage();
    }
    return watcher.reply();
}

quint32 NotificationTest::notify(const QString& summary, const QString& body, quint32 replacesId,
                                 qint32 expireTimeout) {
    QDBusMessage notifyCall = QDBusMessage::createMethodCall(
        QString::fromLatin1(quantum::dbus::NotificationsServiceName),
        QString::fromLatin1(quantum::dbus::NotificationsObjectPath),
        QString::fromLatin1(quantum::dbus::NotificationsInterface), QStringLiteral("Notify"));
    notifyCall << QStringLiteral("Quantum Shell Test") << replacesId << QString() << summary << body
         << QStringList() << QVariantMap() << qint32(expireTimeout);

    QDBusPendingCall pending = sender().asyncCall(notifyCall);
    const QDBusMessage reply = waitForReply(pending, QStringLiteral("Notify"));
    if (reply.type() != QDBusMessage::ReplyMessage)
        return 0;
    return reply.arguments().constFirst().toUInt();
}

QDBusInterface NotificationTest::daemon() const {
    // The interface a sender constructs: the well-known name, the object path, and the spec's interface
    // name. This is `notify-send`'s own first three steps, done from the test rather than from a method
    // call, so what is under test is what arrives over the wire.
    return QDBusInterface(QString::fromLatin1(quantum::dbus::NotificationsServiceName),
                          QString::fromLatin1(quantum::dbus::NotificationsObjectPath),
                          QString::fromLatin1(quantum::dbus::NotificationsInterface),
                          sender());
}

void NotificationTest::theShellTakesTheNotificationsName() {
    // The shell is the daemon or it is nothing, so this is the property everything else depends on. Read
    // from the bus rather than from the service, because the bus is what a sender resolves against and
    // the service's own flag is only as good as the reply it got.
    QDBusReply<bool> registered = connection_.interface()->isServiceRegistered(
        QString::fromLatin1(quantum::dbus::NotificationsServiceName));
    QVERIFY2(registered.isValid(), qPrintable(registered.error().message()));
    QVERIFY(registered.value());

    // The name's owner is this connection, which is what "the shell is the daemon" means on the wire:
    // a name that is registered but owned elsewhere is a queue position, not a daemon.
    QDBusReply<QString> owner = connection_.interface()->serviceOwner(
        QString::fromLatin1(quantum::dbus::NotificationsServiceName));
    QVERIFY2(owner.isValid(), qPrintable(owner.error().message()));
    QCOMPARE(owner.value(), connection_.baseService());

    QVERIFY(service_->notificationAvailable());
}

void NotificationTest::theNotifyMethodAnswersWithAnId() {
    // The spec's contract: a sender gets back an id for the notification it made. Zero is the spec's
    // "no id", so an answer of zero is a daemon that did nothing rather than one that answered.
    const quint32 id = notify(QStringLiteral("Summary one"), QStringLiteral("Body one"), 0);
    QVERIFY(id != 0);
}

void NotificationTest::replacesIdIsAnsweredWithTheSameId() {
    // A caller updates a notification by sending the id it was given as `replaces_id`; the spec says the
    // daemon answers with that same id. A daemon that minted a new one would turn an update into a
    // second notification, which is a defect a sender cannot defend against. `notify-send`'s own
    // `--replace-id` is this path, and a window manager or a chat client relies on it the same way.
    const quint32 firstId = 42;
    QCOMPARE(notify(QStringLiteral("Updated"), QStringLiteral("Replaced body"), firstId), firstId);
}

void NotificationTest::notifyPublishesTheSendersOwnText() {
    // The readout's contract: what it shows is what the sender wrote, in the sender's own words. The
    // summary, the body and the application name are the spec's three text arguments, published
    // verbatim rather than reformatted or composed — a daemon that reworded a notification would be
    // answering for a sender. `notify-send`'s exit is what says the call was accepted, and the spy is
    // what says the shell's own state moved because of it.
    QSignalSpy changed(service_.get(), &quantum::dbus::NotificationService::notificationChanged);
    QVERIFY(changed.isValid());
    const int countBefore = service_->notificationCount();

    // The spy is created before the call because the state moves while the reply is in flight: the
    // daemon publishes during the reply and not after it, so `wait()` would be waiting for a second
    // change that never comes. `notify()` returns only once the reply has been read, which is the
    // point the properties are already set.
    QVERIFY(notify(QStringLiteral("Memory is low"),
                    QStringLiteral("Close some applications to free space"), 0) != 0);
    QCOMPARE(changed.count(), 1);

    QCOMPARE(service_->notificationApplication(), QStringLiteral("Quantum Shell Test"));
    QCOMPARE(service_->notificationSummary(), QStringLiteral("Memory is low"));
    QCOMPARE(service_->notificationBody(), QStringLiteral("Close some applications to free space"));
    QCOMPARE(service_->notificationCount(), countBefore + 1);

    // A notification with no body is a legal notification, and the daemon's answer is to publish the
    // absence rather than to invent a line of text.
    QVERIFY(notify(QStringLiteral("Alarm"), QString(), 0) != 0);

    QCOMPARE(service_->notificationSummary(), QStringLiteral("Alarm"));
    QVERIFY(service_->notificationBody().isEmpty());
    QCOMPARE(service_->notificationCount(), countBefore + 2);
}

void NotificationTest::getServerInformationNamesTheDaemon() {
    // A sender asks this to know who it is talking to. The four out-arguments are the spec's, and a
    // daemon that answers with a spec version it has not read is claiming a contract it may not keep.
    const QDBusMessage reply = call(QStringLiteral("GetServerInformation"));
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.arguments().size(), 4);
    QVERIFY(!reply.arguments().at(0).toString().isEmpty());
    QVERIFY(!reply.arguments().at(1).toString().isEmpty());
    QVERIFY(!reply.arguments().at(2).toString().isEmpty());
    QVERIFY(!reply.arguments().at(3).toString().isEmpty());
}

void NotificationTest::getCapabilitiesReportsWhatTheDaemonHas() {
    // The capabilities are the daemon's promise to a sender. `body` is listed because a body is carried
    // and published exactly as sent — nothing draws it yet, which is the landed state the design document
    // records — and a capability the shell does not honour would be a sender planning around a feature
    // that is not there.
    const QDBusMessage capabilities = call(QStringLiteral("GetCapabilities"));
    QCOMPARE(capabilities.type(), QDBusMessage::ReplyMessage);
    const QStringList caps = capabilities.arguments().constFirst().toStringList();
    QVERIFY2(caps.contains(QStringLiteral("body")),
             qPrintable(caps.join(QStringLiteral(", "))));
    // The toast draws every text as plain text, so markup is a capability the daemon does not have: a sender
    // told otherwise would send `<b>` and have the characters drawn.
    QVERIFY2(!caps.contains(QStringLiteral("body-markup")), qPrintable(caps.join(QStringLiteral(", "))));
}

void NotificationTest::aNotifySendReachesTheService() {
    // The exit criteria's smoke test: the actual `notify-send` program on this machine, which is a
    // different binary from the shell and the test, resolves the name and delivers a notification. Its
    // exit status is the sender's own report that a daemon answered — an exit 1 is `notify-send` saying
    // nothing was listening, which is what a registration that did not take looks like from outside the
    // process. The shell's own state moving is what says it was the shell that answered.
    //
    // The program is *probed* before it is depended on, which is the one thing this slot learned from being
    // run where it could not work. It is not part of this project: it belongs to libnotify. So it can be
    // absent, and it can be present and unable to run at all — a `libnotify.so.4` earlier on `LD_LIBRARY_PATH`
    // than the system's own makes the dynamic loader refuse the program before its `main` runs, which is
    // exit 127 with a symbol lookup error on stderr. Both are facts about the machine and neither is
    // anything the daemon did, so neither may be reported as the daemon failing: depended on unprobed, the
    // second one fails this slot in a way that reads as the shell's, and takes the whole run with it, the
    // order checks included, because a non-zero exit is what those checks run on.
    const QString program = QStandardPaths::findExecutable(QStringLiteral("notify-send"));
    if (program.isEmpty()) {
        QSKIP("no notify-send on PATH, so no sender that is not this test can be run against this daemon: "
              "the claim that the desktop's own sender reaches it is not tested here. What the daemon answers "
              "is covered by the slots above, which call the same methods over the same bus.");
    }

    // The probe is the sender's own `--version`: the one invocation that needs no bus and delivers nothing,
    // and the program's own statement that it can run. A working sender exits 0 there; libnotify's
    // `notify-send` carries the option in its own option table, and the 0.8.8 installed on the machine this
    // was written on prints `notify-send 0.8.8` and exits 0 — measured, both ways, with the loader's own
    // diagnostic as the other answer. The probe does not fail the run: a sender that cannot be run says
    // nothing about this daemon either, so what it gets is a skip naming the exit code and the message, and
    // the missing half is stated rather than left to be inferred.
    const SenderRun probe = runNotifySend(program, {QStringLiteral("--version")});
    if (!probe.started) {
        QSKIP(qPrintable(QStringLiteral("notify-send could not be started (%1), so the claim that the "
                                        "desktop's own sender reaches this daemon is not tested here")
                             .arg(probe.standardError)));
    }
    if (probe.exitCode != 0) {
        QSKIP(qPrintable(QStringLiteral("notify-send --version exited %1 on this machine (%2), so this "
                                        "program cannot run here and the claim that the desktop's own sender "
                                        "reaches this daemon is not tested here")
                             .arg(probe.exitCode)
                             .arg(probe.standardError)));
    }

    QSignalSpy changed(service_.get(), &quantum::dbus::NotificationService::notificationChanged);
    QVERIFY(changed.isValid());

    // The sender that was probed, run with what a person's own `notify-send` would put on the wire. From
    // here on nothing is skipped: the program runs on this machine, so an exit that is not 0 is the sender
    // reporting that no daemon answered, which is this shell failing to be the daemon.
    const SenderRun sent = runNotifySend(program, {QStringLiteral("A real notification through the real daemon"),
                                                   QStringLiteral("Sent by the desktop's own sender")});
    QVERIFY2(sent.started, "notify-send could not be started");
    QVERIFY2(sent.finished, "notify-send did not finish");
    QVERIFY2(sent.exitCode == 0, qPrintable(QStringLiteral("notify-send exited %1 (stderr: %2)")
                                                .arg(sent.exitCode)
                                                .arg(sent.standardError)));

    // The call `notify-send` made is the same one the tests above make, so the shell's state moves for
    // the same reason — and this is the claim that a real sender and the shell agree on the wire.
    QVERIFY(changed.count() >= 1);
    QCOMPARE(service_->notificationSummary(),
             QStringLiteral("A real notification through the real daemon"));
}

// The bus's own answer about who owns the name, which is what a sender resolves against.
bool NotificationTest::waitUntilAvailable(int timeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!service_->notificationAvailable() && elapsed.elapsed() < timeoutMs)
        QCoreApplication::processEvents();
    return service_->notificationAvailable();
}

void NotificationTest::aQueuedShellBecomesTheDaemonWhenTheHolderLeaves() {
    // The life the `QueueService` request exists for, and the one the shipped shell is in on any desktop
    // whose other notifier exits: the shell starts while another daemon holds the name, so it is queued and
    // publishes nothing; the holder leaves; the bus grants the name to the queued request; and the shell is
    // now the desktop's notifier and has to say so, because the alternative is a shell that answers every
    // `Notify` on the bus while its bar draws nothing and its record claims it is not the daemon.
    //
    // The holder is this test's own second connection, which is the only way to make the shape happen on
    // demand: the desktop's notifier is not on this bus, and nothing else here can hold a name the shell
    // is asking for.
    const QString name = QString::fromLatin1(quantum::dbus::NotificationsServiceName);

    QVERIFY(service_->notificationAvailable());
    service_->stop();
    QVERIFY(!service_->notificationAvailable());

    QDBusConnection holder = QDBusConnection::connectToBus(bus_.address(),
                                                           QString::fromLatin1(kBusName) + "-holder");
    QVERIFY(holder.isConnected());
    const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> held =
        holder.interface()->registerService(name, QDBusConnectionInterface::DontQueueService);
    QVERIFY2(held.isValid() &&
                 held.value() == QDBusConnectionInterface::ServiceRegistered,
             qPrintable(held.isValid() ? QStringLiteral("the holder could not take the name")
                                       : held.error().message()));

    // The shell asks for the name while the holder has it. A queue position is not the daemon: a sender that
    // resolves the name right now reaches the holder, so the shell publishing availability here would make
    // its readout draw a summary it was never sent.
    service_->start();
    QVERIFY(!service_->notificationAvailable());
    const QDBusReply<QString> ownerWhileQueued = connection_.interface()->serviceOwner(name);
    QVERIFY2(ownerWhileQueued.isValid(), qPrintable(ownerWhileQueued.error().message()));
    QVERIFY(ownerWhileQueued.value() != connection_.baseService());

    // The holder leaves. What follows is the bus's doing — it grants the name to the queued request and
    // reports it — so the wait is for the shell's own state to move rather than for a fixed delay, and the
    // timeout is what makes a shell that never publishes a failure instead of a slow pass.
    const QDBusReply<bool> released = holder.interface()->unregisterService(name);
    QVERIFY2(released.isValid() && released.value(),
             qPrintable(released.isValid() ? QStringLiteral("the holder could not release the name")
                                           : released.error().message()));
    QVERIFY2(waitUntilAvailable(5000),
             "the shell was granted the notifications name but never published that it is the daemon");

    // And it is the daemon rather than a flag that says so: the bus names this connection as the owner, and
    // a notification sent to the name now arrives here and publishes its text.
    const QDBusReply<QString> ownerNow = connection_.interface()->serviceOwner(name);
    QVERIFY2(ownerNow.isValid(), qPrintable(ownerNow.error().message()));
    QCOMPARE(ownerNow.value(), connection_.baseService());
    QVERIFY(notify(QStringLiteral("The handover reached the shell"), QString(), 0) != 0);
    QCOMPARE(service_->notificationSummary(), QStringLiteral("The handover reached the shell"));

    // The slot ends where it began, with the shell the daemon: every other slot in this binary reads that
    // state, and the shuffled order check runs them in any order.
    QVERIFY(service_->notificationAvailable());
}

void NotificationTest::closingTheNameWithdrawsTheReading() {
    // The name going away is the daemon being demoted, and the readout's honest answer to it is to stop
    // claiming it has a notification. A reading left standing would be a summary from a previous life of
    // the daemon drawn as though it were current — the same lie a stale cache tells.
    QVERIFY(service_->notificationAvailable());

    // `stop()` is the shell standing down as the daemon, which is the same shape of event another
    // daemon's exit makes on the bus: the name goes, and the reading goes with it. The state is read
    // directly because the change is synchronous — nothing has to cross the bus to reach it.
    service_->stop();

    QVERIFY(!service_->notificationAvailable());
    QVERIFY(service_->notificationSummary().isEmpty());
    QVERIFY(service_->notificationBody().isEmpty());
    QVERIFY(service_->notificationApplication().isEmpty());

    // And the daemon that comes back is the daemon again: `start()` re-registers and takes the name
    // back, which is the recovery the queue exists for. This is the composition root's own outage
    // case, exercised here on a bus nothing else uses.
    service_->start();
    QVERIFY(service_->notificationAvailable());
}
void NotificationTest::aDemotionSignalFromBeforeAReRegistrationDoesNotWithdrawAReadingHeldNow() {
    // `stop()` releases the name and the bus reports that release as the service watcher's
    // `serviceUnregistered`, but not inside `stop()` — the signal is queued on the bus, so it can be
    // delivered after a later `start()` has taken the name back. Clearing the reading on that signal
    // alone withdraws a reading the shell owns, which is the demotion reported for the wrong life of
    // the daemon. This slot is that ordering, held still long enough for the bus's delayed report to
    // arrive, and the reading has to survive it.
    QVERIFY(service_->notificationAvailable());
    service_->stop();
    service_->start();
    QVERIFY(service_->notificationAvailable());

    // The wait is for the bus's delayed report to arrive rather than for a fixed delay: the name is
    // owned by this connection now, and a release reported for the previous registration is not a
    // release of this one.
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 500)
        QCoreApplication::processEvents();

    QVERIFY(service_->notificationAvailable());
}

void NotificationTest::aRegistrationReportFromBeforeStopDoesNotReacquireTheName() {
    // `stop()` releases the name. The bus can still deliver the `serviceRegistered` that belonged to
    // the registration just given up — the same queued-signal shape as the demotion above, the other
    // way around. "Not the daemon" is also the state of a shell queued behind another holder, which is
    // why that state alone cannot mean "take the name". A shell that has stood down stays stood down
    // until an explicit `start()`, and a report from the registration it released does not take the
    // name back.
    const QString notificationsName = QString::fromLatin1(quantum::dbus::NotificationsServiceName);
    const QDBusReply<QString> ownerBefore = connection_.interface()->serviceOwner(notificationsName);
    QVERIFY(service_->notificationAvailable());
    QCOMPARE(ownerBefore.value(), connection_.baseService());

    service_->stop();

    // Deliver the report explicitly. Pumping events after `stop()` can miss it: the acquisition's own
    // `serviceRegistered` is often dispatched inside `start()`, while this connection still owns the
    // name, and the guard then does nothing. The report that matters is the one that arrives after the
    // reading has been cleared.
    const bool invoked = QMetaObject::invokeMethod(
        service_.get(), "onServiceRegistered", Qt::DirectConnection,
        Q_ARG(QString, notificationsName));
    QVERIFY(invoked);

    QVERIFY(!service_->notificationAvailable());
    const QDBusReply<QString> ownerAfterStop = connection_.interface()->serviceOwner(notificationsName);
    QVERIFY(ownerAfterStop.value() != connection_.baseService());

    // An explicit start is still the daemon. Then drain this `stop()`'s delayed `serviceUnregistered`
    // before returning: every other slot reads a shell that owns the name, and the shuffled order
    // check runs them in any order.
    service_->start();
    QVERIFY(service_->notificationAvailable());
    const QDBusReply<QString> ownerAfterStart = connection_.interface()->serviceOwner(notificationsName);
    QCOMPARE(ownerAfterStart.value(), connection_.baseService());

    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 500)
        QCoreApplication::processEvents();

    QVERIFY(service_->notificationAvailable());
}

void NotificationTest::aNotificationCreatesOneToastPerOutput() {
    // A toast is created for what the daemon publishes, so the sender's own call is what creates it: sent over
    // the bus this test's daemon registers on, by the second connection, and waited for rather than slept on
    // because the creation is a component being completed and a configure round trip behind it.
    // One toast per output, whatever was up before: the slots before this one have sent notifications of their
    // own, and a toast still standing from one of them is the one *updated* by this notification — the newest
    // wins — rather than one more. So the count after is one per screen, and it is counted rather than
    // assumed, because the creation is a component being completed and a configure round trip behind it.
    const int outputs = QGuiApplication::screens().size();
    QVERIFY2(outputs > 0, "no screen for a toast to be created against");

    QVERIFY2(notify(QStringLiteral("Update available"), QStringLiteral("Quantum Shell 0.2.0"), 0) != 0,
             "the daemon did not answer a Notify");

    QTRY_COMPARE_WITH_TIMEOUT(toasts_->toasts().size(), outputs, 5000);
    for (QWindow* toast : toasts_->toasts())
        QVERIFY2(toast->isVisible(), "a toast was created for an output it is not on");

    // And withdrawn by the expiry the sender asked for, which is the toast's own clock rather than a flag of
    // ours: waited for, because a timer is a length of time and nothing orders it against this line. The wait
    // outlasts the expiry rather than matching it — the timer is armed when the notification arrives and this
    // line runs after the toast is up, so a window equal to the expiry is a window that can end a tick before
    // the toast does. Nothing left standing afterwards — the whole set is withdrawn together, one message on
    // every output.
    QTRY_COMPARE_WITH_TIMEOUT(toasts_->toasts().size(), 0, 8000);
}

void NotificationTest::theToastDrawsTheSendersOwnFacts() {
    // The three facts the toast is created for are the sender's own, published by the daemon as the spec hands
    // them: the application name, the summary and the body. Read off the window the toast is drawn in, because
    // that is what a person sees.
    QVERIFY2(notify(QStringLiteral("Volume muted"), QStringLiteral("by the keyboard"), 0) != 0,
             "the daemon did not answer a Notify");

    QTRY_VERIFY_WITH_TIMEOUT(!toasts_->toasts().isEmpty(), 5000);
    for (QWindow* toast : toasts_->toasts()) {
        QCOMPARE(toast->property("toastApplication").toString(), QStringLiteral("Quantum Shell Test"));
        QCOMPARE(toast->property("toastSummary").toString(), QStringLiteral("Volume muted"));
        QCOMPARE(toast->property("toastBody").toString(), QStringLiteral("by the keyboard"));
    }

    // And the toast's own typography, which is the configuration's: the size and family the bar's text is
    // drawn in are the same ones, because a palette table and a font table are the shell's and not a toast's.
    for (QWindow* toast : toasts_->toasts()) {
        QCOMPARE(toast->property("face").toString(), config_.bar()->font()->family());
        QCOMPARE(toast->property("fontSize").toInt(), config_.bar()->font()->size());
    }

    // Put back to the state the slots around this one read: the toasts withdrawn, so no slot after this one is
    // looking at a toast this one left standing.
    QTRY_COMPARE_WITH_TIMEOUT(toasts_->toasts().size(), 0, 5000);
}

void NotificationTest::theExpiryTheSenderAskedForIsTheOneTheToastLivesFor() {
    // A sender that names its own length is honoured rather than clamped: the spec's `expire_timeout` is a
    // sender's wish, and a daemon that shortened it to its own default would be answering a notification it
    // was not sent. Five hundred milliseconds is the floor the schema states, so it is the shortest a toast
    // can be asked to live for.
    //
    // The same expiry is what the sender hears as the spec's reason 1: without a `NotificationClosed` a sender
    // waiting on it would wait for ever. Read off the wire, in this slot because the wait is the one a real
    // expiry costs and a second slot would pay it again.
    QVERIFY2(settle(), "the daemon could not be brought to a state with nothing showing");
    ClosedRecorder recorder;
    listenForClosed(recorder);
    const quint32 id = notify(QStringLiteral("A short toast"), QString(), 0, 500);
    QVERIFY2(id != 0, "the daemon did not answer a Notify");

    QTRY_VERIFY_WITH_TIMEOUT(!toasts_->toasts().isEmpty(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(toasts_->toasts().size(), 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(recorder.seen.size(), 1, 5000);
    QCOMPARE(recorder.seen.constFirst().id, id);
    QCOMPARE(recorder.seen.constFirst().reason, 1u);
}

void NotificationTest::serviceRegistersWithQml() {
    // The QML side resolves `NotificationService` by that name, so the registration is interface. The
    // properties are read off the meta-object because that is the spelling a QML binding resolves
    // against, and a `Q_PROPERTY` renamed on one side would fail here rather than at load time.
    quantum::dbus::NotificationService service;
    service.registerQmlSingleton(service);

    const QMetaObject* meta = service.metaObject();
    QVERIFY(meta->indexOfProperty("notificationAvailable") >= 0);
    QVERIFY(meta->indexOfProperty("notificationSummary") >= 0);
    QVERIFY(meta->indexOfProperty("notificationBody") >= 0);
    QVERIFY(meta->indexOfProperty("notificationApplication") >= 0);
    QVERIFY(meta->indexOfProperty("notificationCount") >= 0);
    QVERIFY(meta->indexOfSignal("notificationChanged()") >= 0);
}

void NotificationTest::closeOverTheWire(quint32 id) {
    QDBusMessage closeCall = QDBusMessage::createMethodCall(
        QString::fromLatin1(quantum::dbus::NotificationsServiceName),
        QString::fromLatin1(quantum::dbus::NotificationsObjectPath),
        QString::fromLatin1(quantum::dbus::NotificationsInterface), QStringLiteral("CloseNotification"));
    closeCall << id;
    const QDBusMessage reply =
        waitForReply(sender().asyncCall(closeCall), QStringLiteral("CloseNotification"));
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
}

// Where a slot that reads `NotificationClosed` starts from: the daemon is up and nothing is showing. The slots
// around it leave notifications behind that are still on a clock, and a recorder connected while one of them
// is showing would hear its expiry — or its displacement by this slot's first `Notify` — as though it were
// this slot's own. Closing what is showing first, and connecting the recorder after, is what makes the
// signal the recorder hears the one the slot caused: the bus routes a signal when it is emitted, and a
// match rule added after the close's reply cannot receive it.
bool NotificationTest::settle() {
    if (!service_->notificationAvailable()) {
        service_->start();
        if (!waitUntilAvailable(5000)) {
            qWarning("settle: the shell did not become the daemon");
            return false;
        }
    }
    if (const quint32 showing = service_->currentNotificationId(); showing != 0)
        closeOverTheWire(showing);
    QElapsedTimer elapsed;
    elapsed.start();
    while (!toasts_->toasts().isEmpty() && elapsed.elapsed() < 5000)
        QCoreApplication::processEvents();
    if (!toasts_->toasts().isEmpty())
        qWarning("settle: %lld toast(s) still up, current id %u", static_cast<long long>(toasts_->toasts().size()),
                 service_->currentNotificationId());
    return toasts_->toasts().isEmpty();
}

void NotificationTest::listenForClosed(ClosedRecorder& recorder) {
    QVERIFY(sender().connect(QString::fromLatin1(quantum::dbus::NotificationsServiceName),
                             QString::fromLatin1(quantum::dbus::NotificationsObjectPath),
                             QString::fromLatin1(quantum::dbus::NotificationsInterface),
                             QStringLiteral("NotificationClosed"), &recorder, SLOT(closed(quint32, quint32))));
}

void NotificationTest::aCloseForTheShowingNotificationIsAnsweredWithNotificationClosed() {
    // The spec's contract for a close the sender asked for: `NotificationClosed(id, 3)`. Read off the wire from
    // a second connection, because a signal the service emitted in C++ and never exported is one no sender hears.
    QVERIFY2(settle(), "the daemon could not be brought to a state with nothing showing");
    ClosedRecorder recorder;
    listenForClosed(recorder);
    const quint32 id = notify(QStringLiteral("To be closed"), QString(), 0, 0);
    QVERIFY(id != 0);
    QCOMPARE(service_->currentNotificationId(), id);

    closeOverTheWire(id);
    QTRY_COMPARE_WITH_TIMEOUT(recorder.seen.size(), 1, 5000);
    QCOMPARE(recorder.seen.constFirst().id, id);
    QCOMPARE(recorder.seen.constFirst().reason, 3u);
    QCOMPARE(service_->currentNotificationId(), 0u);
    QTRY_COMPARE_WITH_TIMEOUT(toasts_->toasts().size(), 0, 5000);
}

void NotificationTest::aCloseForAnIdThatIsNotShowingSaysNothing() {
    // Accepted, as the spec requires, and silent: a signal for a notification that was not up would tell a
    // sender something about a life the daemon never gave it. Both cases — never sent, and already closed.
    QVERIFY2(settle(), "the daemon could not be brought to a state with nothing showing");
    ClosedRecorder recorder;
    listenForClosed(recorder);
    const quint32 id = notify(QStringLiteral("Closed twice"), QString(), 0, 0);
    closeOverTheWire(id);
    QTRY_COMPARE_WITH_TIMEOUT(recorder.seen.size(), 1, 5000);

    closeOverTheWire(id);
    closeOverTheWire(id + 1000);
    closeOverTheWire(0);
    // Silence is proved by order rather than by waiting for nothing: the bus delivers a sender's signals in the
    // order they were emitted, so a close that *does* answer, made after the three above, arrives after any
    // signal they could have caused. Exactly one more signal, and it is the probe's.
    const quint32 probe = notify(QStringLiteral("Probe"), QString(), 0, 0);
    closeOverTheWire(probe);
    QTRY_COMPARE_WITH_TIMEOUT(recorder.seen.size(), 2, 5000);
    QCOMPARE(recorder.seen.at(1).id, probe);
}

void NotificationTest::aNewerNotificationClosesTheOneItDisplaced() {
    // One notification is showing at a time, so the second takes the first's place, and the first's sender is
    // told rather than left waiting on a notification nobody will ever close.
    QVERIFY2(settle(), "the daemon could not be brought to a state with nothing showing");
    ClosedRecorder recorder;
    listenForClosed(recorder);
    const quint32 first = notify(QStringLiteral("First"), QString(), 0, 0);
    const quint32 second = notify(QStringLiteral("Second"), QString(), 0, 0);
    QVERIFY(first != 0 && second != 0 && first != second);
    QTRY_COMPARE_WITH_TIMEOUT(recorder.seen.size(), 1, 5000);
    QCOMPARE(recorder.seen.constFirst().id, first);
    QCOMPARE(recorder.seen.constFirst().reason, 4u);

    // An update of the showing notification displaces nothing.
    QCOMPARE(notify(QStringLiteral("Second, updated"), QString(), second, 0), second);
    closeOverTheWire(second);
    // In order on the wire: the update closed nothing, so the next signal after the displacement is the close.
    QTRY_COMPARE_WITH_TIMEOUT(recorder.seen.size(), 2, 5000);
    QCOMPARE(recorder.seen.at(1).id, second);
    QCOMPARE(recorder.seen.at(1).reason, 3u);
    QTRY_COMPARE_WITH_TIMEOUT(toasts_->toasts().size(), 0, 5000);
}

void NotificationTest::aNewIdIsNeverTheOneThatIsShowing() {
    // A sender may hand back any `replaces_id` and the daemon echoes it, so the counter can arrive at an id
    // that is already showing — and two notifications with one id are a close that closes the wrong one.
    QVERIFY2(settle(), "the daemon could not be brought to a state with nothing showing");
    const quint32 a = notify(QStringLiteral("Counter"), QString(), 0, 0);
    // The id the counter would issue next, made the showing one by the sender's own hand.
    const quint32 taken = a + 1;
    QCOMPARE(notify(QStringLiteral("Echoed"), QString(), taken, 0), taken);
    QCOMPARE(service_->currentNotificationId(), taken);
    const quint32 fresh = notify(QStringLiteral("Fresh"), QString(), 0, 0);
    QVERIFY(fresh != 0);
    QVERIFY2(fresh != taken, "a new notification was given the id of the one showing");
    closeOverTheWire(fresh);
    QTRY_COMPARE_WITH_TIMEOUT(toasts_->toasts().size(), 0, 5000);
}

void NotificationTest::aSenderClosingItsNotificationWithdrawsTheToast() {
    // A notification that never expires is withdrawn by its own sender or by nothing: the toast has to follow
    // the close, or a `CloseNotification` would leave the message on screen for as long as the shell runs.
    QVERIFY2(settle(), "the daemon could not be brought to a state with nothing showing");
    const quint32 id = notify(QStringLiteral("Stays"), QString(), 0, 0);
    QTRY_VERIFY_WITH_TIMEOUT(!toasts_->toasts().isEmpty(), 5000);
    // That a `0` is not subject to the configured default is `aNeverExpiring...`'s claim, which waits for it.
    closeOverTheWire(id);
    QTRY_COMPARE_WITH_TIMEOUT(toasts_->toasts().size(), 0, 5000);
}

void NotificationTest::aNeverExpiringNotificationOutlivesTheClockAnEarlierOneArmed() {
    // The earlier notification arms the clock; the one that replaces it says never. A clock left running
    // would withdraw a toast its sender asked to keep.
    QVERIFY2(settle(), "the daemon could not be brought to a state with nothing showing");
    const quint32 timed = notify(QStringLiteral("Timed"), QString(), 0, 500);
    QVERIFY(timed != 0);
    const quint32 kept = notify(QStringLiteral("Kept"), QString(), 0, 0);
    QTRY_VERIFY_WITH_TIMEOUT(!toasts_->toasts().isEmpty(), 5000);
    QTest::qWait(700);  // past the 500 ms the earlier notification armed
    QVERIFY2(!toasts_->toasts().isEmpty(), "the earlier notification's clock withdrew a toast that never expires");
    closeOverTheWire(kept);
    QTRY_COMPARE_WITH_TIMEOUT(toasts_->toasts().size(), 0, 5000);
}

void NotificationTest::aToastIsNotTheSizeOfTheScreen() {
    // Anchored to two edges only, a layer surface that declares no size is proposed the whole output, so the
    // toast must name its own: a width, and a height that is its text's. Read off the window the toast is,
    // since the size is what the layer-shell integration sends. Bounds alone would not tell: the offscreen
    // platform gives a window that declared nothing 640x480, which is inside a 800x600 screen, so the claim
    // is made the way only a declared size can satisfy — the width is the toast's own, and the height follows
    // the text, which a default cannot do.
    QVERIFY2(settle(), "the daemon could not be brought to a state with nothing showing");
    auto heightFor = [&](const QString& body) {
        const quint32 id = notify(QStringLiteral("Sized"), body, 0, 0);
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!toasts_->toasts().isEmpty(), 5000); }();
        QWindow* toast = toasts_->toasts().constFirst();
        QTest::qWait(50);  // the window's height is a binding on the text, settled once the text is
        const QSize size = toast->size();
        closeOverTheWire(id);
        [&] { QTRY_COMPARE_WITH_TIMEOUT(toasts_->toasts().size(), 0, 5000); }();
        return size;
    };

    const QSize shortBody = heightFor(QStringLiteral("A body"));
    const QSize longBody = heightFor(QStringLiteral(
        "A body that is long enough that it cannot sit on one line of a toast this wide, so it wraps onto "
        "several, and a toast that is as tall as its text has to grow to hold every one of them"));
    QCOMPARE(shortBody.width(), 380);
    QCOMPARE(longBody.width(), 380);
    QVERIFY2(shortBody.height() > 0, "the toast declares no height");
    QVERIFY2(longBody.height() > shortBody.height(), "the toast's height does not follow its text");
    for (QScreen* screen : QGuiApplication::screens())
        QVERIFY2(longBody.height() < screen->geometry().height(), "the toast is as tall as its screen");
}

void NotificationTest::aClickOnAToastDismissesItAndTheSenderIsTold() {
    // The spec's reason 2, "dismissed by the user": the click is the only way a person ends a notification that
    // never expires. Delivered as a real mouse event to the window the toast is, so it goes through the
    // MouseArea the component declares rather than through a call the test invents.
    QVERIFY2(settle(), "the daemon could not be brought to a state with nothing showing");
    ClosedRecorder recorder;
    listenForClosed(recorder);
    const quint32 id = notify(QStringLiteral("Click me"), QStringLiteral("body"), 0, 0);
    QTRY_VERIFY_WITH_TIMEOUT(!toasts_->toasts().isEmpty(), 5000);
    QWindow* toast = toasts_->toasts().constFirst();
    QTRY_VERIFY_WITH_TIMEOUT(toast->isExposed(), 5000);
    QTest::mouseClick(toast, Qt::LeftButton, Qt::NoModifier, QPoint(30, 20));

    QTRY_COMPARE_WITH_TIMEOUT(recorder.seen.size(), 1, 5000);
    QCOMPARE(recorder.seen.constFirst().id, id);
    QCOMPARE(recorder.seen.constFirst().reason, 2u);
    QTRY_COMPARE_WITH_TIMEOUT(toasts_->toasts().size(), 0, 5000);
}

void NotificationTest::becomingTheDaemonCreatesNoToast() {
    // The service announces a change when the shell takes the notifications name, and that change carries no
    // notification. Reacting to it as though it did put a blank toast on every output — on any desktop where
    // the shell waits behind another notifier and inherits the name when it exits. Stood down and started
    // again here, which is the same announcement; the toast is created synchronously from the signal, so
    // nothing needs waiting for and the check is immediate.
    QVERIFY2(settle(), "the daemon could not be brought to a state with nothing showing");
    service_->stop();
    QVERIFY(!service_->notificationAvailable());
    service_->start();
    QVERIFY(waitUntilAvailable(5000));
    QCOMPARE(service_->currentNotificationId(), 0u);
    QVERIFY2(toasts_->toasts().isEmpty(), "becoming the daemon put a toast on screen for no notification");
}

#include "notification_test.moc"

// The platform is chosen here rather than inherited, and the application is a `QGuiApplication` rather than
// the `QCoreApplication` `QTEST_MAIN` would have used: the toast is a layer-shell surface created against
// Qt's own screen list, and a window mapped on the session's own compositor would be one on the developer's
// desktop. Offscreen is what makes every claim below a claim about the toast rather than about whatever is on
// screen — one screen, so one toast per notification.
int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    NotificationTest test;
    return QTest::qExec(&test, argc, argv);
}
