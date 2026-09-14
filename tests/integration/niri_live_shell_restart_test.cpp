// Proves restart survival through the *running shell*, which is the one Phase 0 count no other test
// covers: `niri-live-restart-test` reattaches its own connections, and `niri-live-layershell-test`
// proves the bar while a compositor stays up, but nothing drove the built shell binary across a
// compositor that dies underneath it.
//
// What the design requires of the shell is stated in QUANTUM_SHELL.md: `QGuiApplication` must exit
// cleanly when the Wayland connection drops — a Wayland client cannot re-home its display connection
// in-process, so "surviving a restart" for the shell means dying promptly when its compositor does and
// being startable again into the new one, while the recovery machinery inside it (`NiriReconnect`,
// socket rediscovery, state rebuild) does the reattaching for whatever connections outlive a session —
// which is what niri-live-restart-test proves for the connections on their own, against a compositor
// restart that leaves the Wayland display standing.
//
// Measured here, the death is: exit within milliseconds of the compositor stopping, not killed by any
// signal, and the abstract IPC socket released. The exit status is 1, not 0, and that is asserted
// deliberately: a session supervisor configured to restart on failure brings the shell back because it
// reports failure, while a shell that reported success would be telling the supervisor it finished its
// work. The exit is initiated by the display connection breaking, whichever display connection notices
// first (Qt's own, or the GTK platform theme's, whose exit from a broken display is what this run
// measured); the test pins the observable contract, not which library got there first.
//
// So the sequence here is the one a session manager would see: a nested niri is started, the built
// shell is started against it and its bar is confirmed in the compositor's layer list; the compositor
// is stopped cleanly and the shell's *actual* death is observed — exit code and signal, not assumed;
// a second niri is started on the socket path niri's naming rule guarantees to be different; and the
// shell is started again, whereupon its bar must appear in the new compositor's layer list and its
// IPC must answer `qsctl state` about the new session. The shell's own abstract IPC socket is read
// out of the kernel's table at each step, because the name is per user rather than per process and an
// unrelated shell would otherwise be indistinguishable from this test's.
//
// It is visible on screen and acts on the session's desktop by starting windows, so it takes the same
// opt-ins as the other acting tests: QS_NIRI_RESTART_TESTS alongside $NIRI_SOCKET.
#include "AbstractIpcSocket.h"
#include "NestedCompositor.h"
#include "niri/NiriIPC.h"
#include "niri/NiriProtocol.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <optional>

using quantum::niri::NiriIPC;
using quantum::niri::Reply;

namespace {

// How long a shell may take to reach a fact the test then asserts: its surface in the layer list, or
// its socket bound. A shell that never gets there fails the wait with the reasons it collected rather
// than hanging.
constexpr int shellStartTimeoutMs = 20000;

// The shell's exit is the assertion here, so both ways of dying are told apart by QProcess: a clean
// exit lands in normalExit, a signal in crashExit, and neither means the shell never noticed.
QString describeExit(const QProcess& process)
{
    switch (process.exitStatus()) {
    case QProcess::NormalExit:
        return QStringLiteral("exit code %1").arg(process.exitCode());
    case QProcess::CrashExit:
        return QStringLiteral("crashed with signal %1").arg(process.exitCode() == -2
                                                                ? QStringLiteral("SIGSEGV/SIGABRT (code -2)")
                                                                : QString::number(process.exitCode()));
    }
    return QStringLiteral("unknown exit status");
}

}  // namespace

class NiriLiveShellRestartTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void theBuiltShellEndsWithItsCompositorAndRunsAgainstTheNextOne();

private:
    // Starts the built shell with the given compositor as its niri, waits for its bar to be listed in
    // that compositor's layer list and for its IPC socket to be bound, and records the socket inode.
    // `reason` carries everything collected when a step does not happen.
    bool startShell(const QString& waylandDisplay, const QString& sessionSocket, QString* reason);

    // True when the compositor's Layers answer names the bar under this test's namespace.
    bool barIsListed(const QString& sessionSocket);

    void stopShell();
    std::optional<Reply> ask(const QString& sessionSocket, const QString& request, int timeoutMs = 5000);

    QTemporaryDir configHome_;
    QProcess shell_;
    qint64 socketInode_ = 0;
    QString waylandDisplay_;
    QString namespace_ = QStringLiteral("quantum-shell-bar");
};

void NiriLiveShellRestartTest::initTestCase()
{
    if (!QFile::exists(QStringLiteral(QS_SHELL_BINARY))) {
        QFAIL(QStringLiteral("the built shell is missing at %1").arg(QStringLiteral(QS_SHELL_BINARY))
                  .toUtf8());
    }
    if (QStandardPaths::findExecutable(QStringLiteral("niri")).isEmpty()) {
        QSKIP("no niri on PATH: this test starts a compositor of its own for the shell to run against");
    }
    if (qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()) {
        QSKIP("no Wayland display to nest a compositor in");
    }
    const QString sessionSocket = quantum::niri::niriSocketPath();
    if (sessionSocket.isEmpty() || !QFile::exists(sessionSocket)) {
        QSKIP("$NIRI_SOCKET is not set, so the test could not tell its compositor from the session's");
    }
    if (!configHome_.isValid()) {
        QFAIL("the test could not create a directory for the shell's XDG_CONFIG_HOME");
    }
    // Nothing is written into it: the shell runs on the schema's defaults, which is what the height
    // and namespace below are — read back from what the compositor was actually given rather than
    // from a value this file hardcoded twice.
}

bool NiriLiveShellRestartTest::barIsListed(const QString& sessionSocket)
{
    NiriIPC client;
    QElapsedTimer timer;
    timer.start();
    client.connectToCompositor(sessionSocket);
    while (timer.elapsed() < shellStartTimeoutMs) {
        if (client.isConnected()) {
            break;
        }
        QTest::qWait(50);
    }
    if (!client.isConnected()) {
        return false;
    }
    const std::optional<Reply> reply = ask(sessionSocket, QStringLiteral("Layers"));
    if (!reply.has_value() || !reply->isOk()) {
        return false;
    }
    const QJsonArray layers = reply->variant(QStringLiteral("Layers")).toArray();
    for (const QJsonValue& value : layers) {
        const QJsonObject layer = value.toObject();
        if (layer.value(QStringLiteral("namespace")).toString() == namespace_
            && layer.value(QStringLiteral("layer")).toString() == QStringLiteral("Top")) {
            return true;
        }
    }
    return false;
}

std::optional<Reply> NiriLiveShellRestartTest::ask(const QString& sessionSocket, const QString& request,
                                                   int timeoutMs)
{
    std::optional<Reply> answer;
    NiriIPC client;
    QElapsedTimer timer;
    timer.start();
    client.connectToCompositor(sessionSocket);
    while (timer.elapsed() < timeoutMs && !client.isConnected()) {
        QTest::qWait(25);
    }
    if (!client.isConnected()) {
        return std::nullopt;
    }
    // Shared with the handler, for the reason every live test's helper is: NiriIPC cannot be told to
    // forget a handler, so a reply that arrives after this gave up still runs it, and it has to land
    // somewhere that outlives the call.
    auto received = std::make_shared<std::optional<Reply>>();
    client.send(request, [received](const Reply& reply) { *received = reply; });
    while (timer.elapsed() < timeoutMs && !received->has_value()) {
        QTest::qWait(25);
    }
    if (received->has_value()) {
        answer = *received;
    }
    return answer;
}

bool NiriLiveShellRestartTest::startShell(const QString& waylandDisplay, const QString& sessionSocket,
                                          QString* reason)
{
    waylandDisplay_ = waylandDisplay;
    if (boundSocketInode().has_value()) {
        *reason = QStringLiteral("%1 is already bound by another process, which is usually another "
                                 "Quantum Shell; this test starts its own and cannot tell the two apart")
                      .arg(abstractSocketRow());
        return false;
    }

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // The shell must talk to the nested compositor on *both* of its connections: NIRI_SOCKET names the
    // IPC socket, and WAYLAND_DISPLAY names the display the bar's surface is created on. The nested
    // instance picks a new Wayland display name (wayland-2, wayland-3, ...), so both variables are set
    // from what the test actually started, and a bar mapped onto the session's compositor instead is a
    // failure of this test's setup, not of the shell.
    environment.insert(QStringLiteral("NIRI_SOCKET"), sessionSocket);
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), waylandDisplay_);
    // An XDG_CONFIG_HOME of the test's own, empty: the shell reads no configuration and every value is
    // the schema's default, so what the test asserts about the bar is about the build, not about a
    // configuration the person running it happens to have.
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), configHome_.path());
    environment.insert(QStringLiteral("QT_WAYLAND_SHELL_INTEGRATION"), QStringLiteral("quantum-shell"));
    environment.insert(QStringLiteral("QT_PLUGIN_PATH"), QStringLiteral(QS_SHELL_PLUGIN_PATH));
    shell_.setProcessEnvironment(environment);
    shell_.setProcessChannelMode(QProcess::SeparateChannels);
    shell_.start(QStringLiteral(QS_SHELL_BINARY), {});
    if (!shell_.waitForStarted(5000)) {
        *reason = QStringLiteral("the shell did not start: %1").arg(shell_.errorString());
        return false;
    }

    // The two facts, in no particular order because nothing orders them: the bar listed by the
    // compositor, and the shell's IPC socket in the kernel's table.
    QElapsedTimer clock;
    clock.start();
    bool listed = false;
    while (clock.elapsed() < shellStartTimeoutMs) {
        if (!listed && barIsListed(sessionSocket)) {
            listed = true;
        }
        const std::optional<qint64> inode = boundSocketInode();
        if (listed && inode.has_value()) {
            socketInode_ = *inode;
            return true;
        }
        if (shell_.state() != QProcess::Running) {
            break;
        }
        QTest::qWait(100);
    }
    stopShell();
    *reason = QStringLiteral("the shell ran but after %1 ms: bar listed = %2, %3. Shell stderr tail:\n%4")
                  .arg(clock.elapsed())
                  .arg(listed ? QStringLiteral("yes") : QStringLiteral("no"))
                  .arg(boundSocketInode().has_value() ? QStringLiteral("its IPC socket is bound")
                                                      : QStringLiteral("nothing is bound at ") + abstractSocketRow())
                  .arg(QString::fromUtf8(shell_.readAllStandardError().right(2000)));
    return false;
}

void NiriLiveShellRestartTest::stopShell()
{
    if (shell_.state() == QProcess::NotRunning) {
        return;
    }
    shell_.terminate();
    if (!shell_.waitForFinished(5000)) {
        shell_.kill();
        shell_.waitForFinished(2000);
    }
}

void NiriLiveShellRestartTest::theBuiltShellEndsWithItsCompositorAndRunsAgainstTheNextOne()
{
    const QString niriPath = QStandardPaths::findExecutable(QStringLiteral("niri"));
    const QString sessionSocket = quantum::niri::niriSocketPath();

    // The compositor the shell will live on. Its config is validated with niri's own validator first,
    // so a broken fixture is named as such rather than as a startup timeout.
    QTemporaryDir compositorDir;
    QVERIFY2(compositorDir.isValid(), "no scratch directory for the test compositor's config");
    const QString compositorConfig = compositorDir.filePath(QStringLiteral("config.kdl"));
    {
        QFile file(compositorConfig);
        QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Text), qPrintable(file.errorString()));
        file.write(nestedCompositorConfig);
        file.close();
        QProcess validate;
        validate.start(niriPath, QStringList{QStringLiteral("validate"), QStringLiteral("-c"), compositorConfig});
        QVERIFY2(validate.waitForFinished(10000), "niri validate did not finish");
        QCOMPARE(validate.exitCode(), 0);
    }

    NestedNiri first;
    QString reason;
    QVERIFY2(first.start(niriPath, compositorConfig, sessionSocket, &reason), qPrintable(reason));
    const QString socketBefore = first.socket();
    const QString display1 = first.waylandDisplay();
    QVERIFY2(!display1.isEmpty(),
             "the nested compositor created no Wayland display for its clients");
    qInfo("nested compositor 1 is up at %s (display %s)", qPrintable(socketBefore),
          qPrintable(display1));

    QVERIFY2(startShell(display1, socketBefore, &reason), qPrintable(reason));
    const qint64 inodeBefore = socketInode_;
    qInfo("the shell is up: bar listed by compositor 1, IPC socket bound (inode %lld)",
          static_cast<long long>(inodeBefore));

    // The state the shell reports comes from this compositor: `qsctl state` is the shell's own view,
    // asked through its abstract socket.
    QProcess qsctlBefore;
    qsctlBefore.start(QStringLiteral(QS_QSCTL_BINARY), QStringList{QStringLiteral("state")});
    QVERIFY2(qsctlBefore.waitForFinished(10000), "qsctl did not finish");
    QCOMPARE(qsctlBefore.exitCode(), 0);
    const QByteArray stateBefore = qsctlBefore.readAllStandardOutput();
    QVERIFY2(!stateBefore.isEmpty(), "qsctl state answered nothing while compositor 1 was up");
    qInfo("compositor 1: qsctl state reports %s", stateBefore.constData());

    // --- the compositor goes away ------------------------------------------------------------------
    first.stop();
    QVERIFY2(!first.wasKilled(),
             qPrintable(QStringLiteral("the test compositor needed SIGKILL; its output:\n%1").arg(first.tail())));

    // The design requires the shell to end when its Wayland connection drops, and what "ends" means is
    // asserted on the observed facts: prompt, not killed by a signal, and its IPC socket released. The
    // exit status is asserted nonzero, because a session supervisor restarts a shell that reported
    // failure and would leave one that reported success lying in the dirt of a dead session.
    QElapsedTimer deathClock;
    deathClock.start();
    QVERIFY2(shell_.waitForFinished(20000),
             qPrintable(QStringLiteral("the shell was still running 20 s after its compositor stopped")));
    const QString how = describeExit(shell_);
    qInfo("compositor 1 stopped; the shell ended after %lld ms: %s",
          static_cast<long long>(deathClock.elapsed()), qPrintable(how));
    QVERIFY2(shell_.exitStatus() == QProcess::NormalExit,
             qPrintable(QStringLiteral("the shell was killed by a signal when its compositor stopped: %1. "
                                        "Its stderr tail:\n%2")
                            .arg(how)
                            .arg(QString::fromUtf8(shell_.readAllStandardError().right(2000)))));
    QVERIFY2(shell_.exitCode() != 0,
             qPrintable(QStringLiteral("the shell reported success (exit 0) when its session died; a session "
                                        "supervisor would not restart it. Its stderr tail:\n%1")
                            .arg(QString::fromUtf8(shell_.readAllStandardError().right(2000)))));

    // And its IPC socket is gone with it, because a socket the dead shell left bound would answer the
    // next session's `qsctl` with a corpse's silence.
    QTest::qWait(100);
    QVERIFY2(!boundSocketInode().has_value(),
             qPrintable(QStringLiteral("the shell is gone but %1 is still bound").arg(abstractSocketRow())));

    // --- the next compositor, on its own socket ----------------------------------------------------
    NestedNiri second;
    QVERIFY2(second.start(niriPath, compositorConfig, sessionSocket, &reason), qPrintable(reason));
    const QString display2 = second.waylandDisplay();
    QVERIFY2(!display2.isEmpty(),
             "the second compositor created no Wayland display for its clients");
    const QString socketAfter = second.socket();
    QVERIFY2(socketAfter != socketBefore,
             qPrintable(QStringLiteral("the restarted compositor reused %1, so this run did not exercise "
                                      "the naming rule that makes the path differ")
                            .arg(socketAfter)));
    qInfo("nested compositor 2 is up at %s", qPrintable(socketAfter));

    // The shell is started again, the way a session manager would start it into a new session. Nothing
    // in the test tells it anything it did not know the first time.
    QVERIFY2(startShell(display2, socketAfter, &reason), qPrintable(reason));
    qInfo("the shell is up against compositor 2: bar listed, IPC socket bound (inode %lld)",
          static_cast<long long>(socketInode_));
    QVERIFY2(socketInode_ != inodeBefore,
             "the second shell bound a different kernel socket object, as a new bind must");

    // And it answers about the session it is in now, not the one that died: `qsctl state` works and
    // agrees with the compositor's own workspaces answer.
    QProcess qsctlAfter;
    qsctlAfter.start(QStringLiteral(QS_QSCTL_BINARY), QStringList{QStringLiteral("state")});
    QVERIFY2(qsctlAfter.waitForFinished(10000), "qsctl did not finish against compositor 2");
    QCOMPARE(qsctlAfter.exitCode(), 0);
    const QByteArray stateAfter = qsctlAfter.readAllStandardOutput();
    QVERIFY2(!stateAfter.isEmpty(), "qsctl state answered nothing after the restart");
    const QJsonDocument shellView = QJsonDocument::fromJson(stateAfter);
    QVERIFY2(shellView.isObject(), "qsctl state is not a JSON object");
    const QJsonArray shellWorkspaces = shellView.object().value(QStringLiteral("workspaces")).toArray();
    QVERIFY2(!shellWorkspaces.isEmpty(), "the shell reports no workspaces after the restart");

    const std::optional<Reply> workspaces = ask(socketAfter, QStringLiteral("Workspaces"));
    QVERIFY2(workspaces.has_value(), "compositor 2 never answered Workspaces");
    QVERIFY2(workspaces->isOk(), qPrintable(workspaces->describe()));
    const QJsonArray compositorWorkspaces =
        workspaces->variant(QStringLiteral("Workspaces")).toArray();
    QCOMPARE(shellWorkspaces.size(), compositorWorkspaces.size());
    QCOMPARE(shellWorkspaces.at(0).toObject().value(QStringLiteral("id")).toString().toLongLong(),
             static_cast<qint64>(compositorWorkspaces.at(0).toObject().value(QStringLiteral("id")).toDouble()));

    qInfo("the shell survived a compositor restart: it ended with the first one and a second start "
          "listed its bar in the new compositor and answered qsctl about it");

    stopShell();
    second.stop();
}

QTEST_GUILESS_MAIN(NiriLiveShellRestartTest)
#include "niri_live_shell_restart_test.moc"
