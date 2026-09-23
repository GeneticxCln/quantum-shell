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
#include <QCoreApplication>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QDBusPendingCallWatcher>
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

private:
    // The bus this test's daemon registers on, and the one a sender is pointed at.
    qstest::NotificationBus bus_;
    QDBusConnection connection_ = QDBusConnection::sessionBus();
    std::unique_ptr<quantum::dbus::NotificationService> service_;

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
    quint32 notify(const QString& summary, const QString& body, quint32 replacesId);
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

quint32 NotificationTest::notify(const QString& summary, const QString& body, quint32 replacesId) {
    QDBusMessage notifyCall = QDBusMessage::createMethodCall(
        QString::fromLatin1(quantum::dbus::NotificationsServiceName),
        QString::fromLatin1(quantum::dbus::NotificationsObjectPath),
        QString::fromLatin1(quantum::dbus::NotificationsInterface), QStringLiteral("Notify"));
    notifyCall << QStringLiteral("Quantum Shell Test") << replacesId << QString() << summary << body
         << QStringList() << QVariantMap() << qint32(-1);

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

#include "notification_test.moc"

QTEST_MAIN(NotificationTest)
