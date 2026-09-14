// What happens to the shell when the compositor goes away and comes back.
//
// Three separate claims are covered here, because they fail in different ways: finding the compositor's
// socket again after a restart (niri's socket name carries its process id, so the path from before the
// restart no longer exists), retrying with growing backoff while it is gone, and rebuilding the state
// model from the subscription that follows — with the state cleared rather than left describing a
// compositor that no longer exists.
//
// The compositor's end of the socket is FakeNiriServer; a restart is one fake being destroyed and
// another listening on a new path, which is what a real restart looks like from a client.
#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"
#include "niri/NiriProtocol.h"
#include "niri/NiriReconnect.h"
#include "niri/NiriState.h"

#include "FakeNiriServer.h"
#include "NiriProtocolTestData.h"

#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

using quantum::niri::NiriEventStream;
using quantum::niri::NiriIPC;
using quantum::niri::NiriReconnect;
using quantum::niri::NiriState;

namespace {

// A real listening socket at `path`, so discovery reads a socket the kernel made rather than a name
// that only looks like one. The caller keeps `server` alive for as long as the socket should exist.
bool listenAt(const QString& path, QLocalServer& server) {
    QLocalServer::removeServer(path);
    return server.listen(path);
}

QString waylandSocketName(const QString& directory, int pid) {
    return QDir(directory).filePath(QStringLiteral("niri.wayland-1.%1.sock").arg(pid));
}

// The policy the tests use: delays small enough to observe quickly, and jitter switched off so the
// sequence is exact. Jitter is a real part of the policy and is asserted separately by range.
NiriReconnect::Policy fastPolicy() {
    NiriReconnect::Policy policy;
    policy.firstDelayMs = 1;
    policy.factor = 2;
    policy.maximumDelayMs = 8;
    policy.jitterMs = 0;
    policy.attemptTimeoutMs = 200;
    return policy;
}

QByteArray versionReply() {
    return QByteArrayLiteral("{\"Ok\":{\"Version\":\"26.04 (8ed0da4)\"}}");
}

}  // namespace

class NiriReconnectTest : public QObject {
    Q_OBJECT

private slots:
    void findsTheSocketsNiriWritesAndIgnoresNamesItWouldNotWrite();
    void trustsTheNamedSocketOnlyWhileThatPathStillExists();
    void retriesWithGrowingBackoffAndStopsAtTheCap();
    void theScheduleDoublesFromTheFirstDelayWhenAConnectionDrops();
    void aDroppedConnectionIsClearedAndRebuiltWithoutTheCompositorMoving();
    void reattachesAfterARestartAndRebuildsStateFromTheNewSubscription();
};

void NiriReconnectTest::findsTheSocketsNiriWritesAndIgnoresNamesItWouldNotWrite() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QLocalServer older;
    QLocalServer newer;
    QVERIFY(listenAt(waylandSocketName(directory.path(), 41001), older));
    // The ordering claim is about the file's time, so the two are separated by more than the
    // filesystem's timestamp resolution rather than by luck.
    QTest::qWait(20);
    QVERIFY(listenAt(waylandSocketName(directory.path(), 41002), newer));

    // Names niri would not write: a socket without the pid, and an unrelated socket. Guessing at either
    // would attach the shell to something that is not a niri IPC socket.
    QLocalServer decoy;
    QVERIFY(listenAt(QDir(directory.path()).filePath(QStringLiteral("niri.wayland-1.sock")), decoy));
    QLocalServer unrelated;
    QVERIFY(listenAt(QDir(directory.path()).filePath(QStringLiteral("other.sock")), unrelated));

    const QStringList found = quantum::niri::niriSocketFiles(directory.path());
    QCOMPARE(found.size(), 2);
    QCOMPARE(found.at(0), waylandSocketName(directory.path(), 41002));
    QCOMPARE(found.at(1), waylandSocketName(directory.path(), 41001));

    // Which compositor a socket belongs to is in the middle of the name, so it can be selected.
    QCOMPARE(quantum::niri::niriSocketFor(QStringLiteral("wayland-1"), directory.path()), found);
    QVERIFY(quantum::niri::niriSocketFor(QStringLiteral("wayland-2"), directory.path()).isEmpty());
    // No Wayland socket name is no answer, not a wildcard.
    QVERIFY(quantum::niri::niriSocketFor(QString(), directory.path()).isEmpty());

    // A directory with nothing in it reports nothing rather than inventing a path.
    QTemporaryDir empty;
    QVERIFY(empty.isValid());
    QVERIFY(quantum::niri::niriSocketFiles(empty.path()).isEmpty());
}

void NiriReconnectTest::trustsTheNamedSocketOnlyWhileThatPathStillExists() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QLocalServer server;
    const QString socket = waylandSocketName(directory.path(), 41003);
    QVERIFY(listenAt(socket, server));

    // The environment is what a shell inherits, so the test sets it and puts it back: the socket niri
    // named for this process is stale after a restart, which is the whole reason for rediscovery.
    const QByteArray savedRuntime = qgetenv("XDG_RUNTIME_DIR");
    const QByteArray savedDisplay = qgetenv("WAYLAND_DISPLAY");
    const QByteArray savedSocket = qgetenv("NIRI_SOCKET");

    qputenv("XDG_RUNTIME_DIR", directory.path().toUtf8());
    qputenv("WAYLAND_DISPLAY", QByteArrayLiteral("wayland-1"));

    qputenv("NIRI_SOCKET", QDir(directory.path()).filePath(QStringLiteral("niri.wayland-1.1.sock")).toUtf8());
    QCOMPARE(quantum::niri::compositorSocket(), socket);

    qputenv("NIRI_SOCKET", socket.toUtf8());
    QCOMPARE(quantum::niri::compositorSocket(), socket);

    // Nothing named and nothing to rediscover: an empty answer, so the caller can say it has no
    // compositor rather than retry a path that never worked.
    qunsetenv("NIRI_SOCKET");
    qunsetenv("WAYLAND_DISPLAY");
    QVERIFY(quantum::niri::compositorSocket().isEmpty());

    qputenv("XDG_RUNTIME_DIR", savedRuntime);
    qputenv("WAYLAND_DISPLAY", savedDisplay);
    if (savedSocket.isEmpty()) {
        qunsetenv("NIRI_SOCKET");
    } else {
        qputenv("NIRI_SOCKET", savedSocket);
    }
}

void NiriReconnectTest::retriesWithGrowingBackoffAndStopsAtTheCap() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    NiriIPC request;
    NiriEventStream stream;
    NiriReconnect reconnect;
    reconnect.keepAttached(request);
    reconnect.keepAttached(stream);
    reconnect.setPolicy(fastPolicy());
    // A path inside a real directory where nothing listens: connecting fails the way it does when the
    // compositor is down, without depending on a socket file's absence for the failure.
    const QString deadSocket = QDir(directory.path()).filePath(QStringLiteral("niri.wayland-1.41004.sock"));
    reconnect.setSocketLocator([deadSocket] { return deadSocket; });

    QList<int> delays;
    QList<int> attempts;
    connect(&reconnect, &NiriReconnect::retryScheduled, this,
            [&delays, &attempts](int attempt, int delayMs, const QString&) {
                attempts.append(attempt);
                delays.append(delayMs);
            });
    // Nothing was ever attached, so there is no outage to report. A supervisor that cried "lost" on
    // its way up would have consumers clearing state they had not been given.
    QSignalSpy lost(&reconnect, &NiriReconnect::lost);

    reconnect.start();
    QTRY_VERIFY_WITH_TIMEOUT(delays.size() >= 5, 5000);
    reconnect.stop();

    QCOMPARE(lost.count(), 0);
    // The first four retries follow the policy exactly: 1, 2, 4, 8.
    QCOMPARE(delays.mid(0, 4), QList<int>({1, 2, 4, 8}));
    // And every later one is held at the cap instead of growing without bound.
    for (int index = 4; index < delays.size(); ++index) {
        QCOMPARE(delays.at(index), 8);
    }
    // Attempts are numbered one after another, with none skipped and none repeated.
    for (int index = 0; index < attempts.size(); ++index) {
        QCOMPARE(attempts.at(index), index + 1);
    }
    QVERIFY(!reconnect.isAttached());
    QCOMPARE(reconnect.attempt(), attempts.size());
}

// The schedule a real restart is retried on. It is asserted separately from the cold start because the
// drop path and the start path count differently, and a real restart was observed to produce 250, 250,
// 500 — the first delay twice — before this was corrected. The compositor here is one that stays down:
// the connections are dropped and the locator then points at a socket with nothing behind it.
void NiriReconnectTest::theScheduleDoublesFromTheFirstDelayWhenAConnectionDrops() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("EventStream"), QByteArrayLiteral("{\"Ok\":\"Handled\"}"));

    NiriIPC request;
    NiriEventStream stream;
    NiriReconnect reconnect;
    reconnect.keepAttached(request);
    reconnect.keepAttached(stream);
    reconnect.setPolicy(fastPolicy());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString deadSocket = QDir(directory.path()).filePath(QStringLiteral("niri.wayland-1.41005.sock"));
    QString socket = server.path();
    reconnect.setSocketLocator([&socket] { return socket; });

    QList<int> delays;
    connect(&reconnect, &NiriReconnect::retryScheduled, this,
            [&delays](int, int delayMs, const QString&) { delays.append(delayMs); });

    reconnect.start();
    QTRY_VERIFY_WITH_TIMEOUT(reconnect.isAttached(), 5000);
    QVERIFY(delays.isEmpty());  // nothing to retry while it is attached

    // The compositor goes away for good: the connections drop, and every retry after them finds nothing.
    socket = deadSocket;
    server.closeConnections();
    QTRY_VERIFY_WITH_TIMEOUT(delays.size() >= 5, 5000);
    reconnect.stop();

    QCOMPARE(delays.mid(0, 4), QList<int>({1, 2, 4, 8}));
    QCOMPARE(delays.at(4), 8);
}

void NiriReconnectTest::aDroppedConnectionIsClearedAndRebuiltWithoutTheCompositorMoving() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("EventStream"), QByteArrayLiteral("{\"Ok\":\"Handled\"}"));

    NiriIPC request;
    NiriEventStream stream;
    NiriState state;
    state.observe(stream);
    NiriReconnect reconnect;
    reconnect.keepAttached(request);
    reconnect.keepAttached(stream);
    reconnect.setPolicy(fastPolicy());
    reconnect.setSocketLocator([&server] { return server.path(); });

    QSignalSpy attached(&reconnect, &NiriReconnect::attached);
    QSignalSpy lost(&reconnect, &NiriReconnect::lost);

    reconnect.start();
    QTRY_VERIFY_WITH_TIMEOUT(attached.count() >= 1, 5000);
    QVERIFY(reconnect.isAttached());

    // What the compositor reports on subscribing: the state the model is built from.
    server.writeRawTo(1, qstest::eventLine(QStringLiteral("WorkspacesChanged"),
                                           QJsonObject{{QStringLiteral("workspaces"),
                                                        qstest::array({qstest::workspaceObject(
                                                            7, 1, QStringLiteral("DP-3"), true, true)})}}));
    QTRY_COMPARE(state.workspaces().size(), 1);

    // The connections drop without this client asking. The compositor is still there — this is the
    // case of a connection going away, not the compositor exiting.
    server.closeConnections();

    QTRY_COMPARE(lost.count(), 1);
    // The state described a connection that is gone, so it is cleared rather than left standing.
    QTRY_VERIFY(state.workspaces().isEmpty());

    // And then the connections come back on their own, and the compositor's answer to the new
    // subscription rebuilds the state. Nothing asked for this: it is the retry that was already
    // scheduled doing its job.
    QTRY_VERIFY_WITH_TIMEOUT(reconnect.isAttached(), 5000);
    QCOMPARE(attached.count(), 2);
    // One outage was reported, not one per attempt: the retries are attempts, not new losses.
    QCOMPARE(lost.count(), 1);

    server.writeRawTo(1, qstest::eventLine(QStringLiteral("WorkspacesChanged"),
                                          QJsonObject{{QStringLiteral("workspaces"),
                                                       qstest::array({qstest::workspaceObject(
                                                           7, 1, QStringLiteral("DP-3"), true, true)})}}));
    QTRY_COMPARE(state.workspaces().size(), 1);
    QVERIFY(state.focusedWorkspace().isValid());

    reconnect.stop();
}

void NiriReconnectTest::reattachesAfterARestartAndRebuildsStateFromTheNewSubscription() {
    NiriIPC request;
    NiriEventStream stream;
    NiriState state;
    state.observe(stream);
    NiriReconnect reconnect;
    reconnect.keepAttached(request);
    reconnect.keepAttached(stream);
    reconnect.setPolicy(fastPolicy());

    // The socket the supervisor is told to use. A restart changes it, exactly as a real one does: niri
    // puts its process id in the socket name.
    QString socket;
    reconnect.setSocketLocator([&socket] { return socket; });

    QSignalSpy attached(&reconnect, &NiriReconnect::attached);
    QSignalSpy lost(&reconnect, &NiriReconnect::lost);
    QSignalSpy versions(&request, &NiriIPC::versionDetected);
    QList<int> retryDelays;
    connect(&reconnect, &NiriReconnect::retryScheduled, this,
            [&retryDelays](int, int delayMs, const QString&) { retryDelays.append(delayMs); });

    {
        FakeNiriServer before;
        QVERIFY2(before.listen(), qPrintable(before.serverError()));
        before.setReply(QStringLiteral("EventStream"), QByteArrayLiteral("{\"Ok\":\"Handled\"}"));
        before.setReply(QStringLiteral("Version"), versionReply());
        socket = before.path();

        reconnect.start();
        QTRY_VERIFY_WITH_TIMEOUT(reconnect.isAttached(), 5000);
        QCOMPARE(attached.count(), 1);
        QCOMPARE(versions.count(), 1);

        before.writeRawTo(1, qstest::eventLine(QStringLiteral("WorkspacesChanged"),
                                              QJsonObject{{QStringLiteral("workspaces"),
                                                           qstest::array({qstest::workspaceObject(
                                                               7, 1, QStringLiteral("DP-3"), true, true)})}}));
        QTRY_COMPARE(state.workspaces().size(), 1);
    }

    // The compositor is gone, and so is its socket: the path the supervisor was using no longer
    // exists. This is the state a shell is in between a compositor exiting and a new one starting.
    QVERIFY(!QFile::exists(socket));
    QTRY_COMPARE(lost.count(), 1);
    QTRY_VERIFY(state.workspaces().isEmpty());
    // Attempts that find nothing are made and counted; the retry is scheduled rather than immediate.
    QTRY_VERIFY_WITH_TIMEOUT(reconnect.attempt() >= 1, 5000);

    // Retries are counted, and they start at the first delay and double: the schedule of a cold start
    // and the schedule after a drop are the same shape.
    QTRY_VERIFY_WITH_TIMEOUT(retryDelays.size() >= 2, 5000);
    QCOMPARE(retryDelays.at(0), 1);
    QCOMPARE(retryDelays.at(1), 2);

    // A new compositor starts, and names its socket differently.
    FakeNiriServer after;
    QVERIFY2(after.listen(), qPrintable(after.serverError()));
    after.setReply(QStringLiteral("EventStream"), QByteArrayLiteral("{\"Ok\":\"Handled\"}"));
    after.setReply(QStringLiteral("Version"), versionReply());
    QVERIFY(after.path() != socket);
    socket = after.path();

    // Nothing tells the supervisor to try again: the retry it already scheduled finds the new socket.
    QTRY_VERIFY_WITH_TIMEOUT(reconnect.isAttached(), 5000);
    QCOMPARE(attached.count(), 2);
    QCOMPARE(lost.count(), 1);

    // Both connections were made again to the new compositor, with their handshakes: the version was
    // asked again, and the event stream was subscribed again.
    QCOMPARE(versions.count(), 2);
    // The bytes are asserted as they went on the wire, quotes included: a unit request is a bare JSON
    // string, and the new compositor has to have been sent the subscription again.
    QVERIFY(after.receivedRequests().contains(QStringLiteral("\"EventStream\"")));
    QVERIFY(after.receivedRequests().contains(QStringLiteral("\"Version\"")));

    // And the state is rebuilt from the subscription that the new compositor answers with, without
    // anything having to re-read it: the same event, a different workspace id.
    after.writeRawTo(1, qstest::eventLine(QStringLiteral("WorkspacesChanged"),
                                         QJsonObject{{QStringLiteral("workspaces"),
                                                      qstest::array({qstest::workspaceObject(
                                                          11, 1, QStringLiteral("DP-1"), true, true)})}}));
    QTRY_COMPARE(state.workspaces().size(), 1);
    QCOMPARE(state.workspaces().first().id(), 11u);
    QVERIFY(state.workspaces().first().isFocused());
    // The bytes this shape of test uses never send layouts, so the model reports none rather than
    // keeping the ones it held before the restart.
    QVERIFY(!state.keyboardLayouts().isValid());

    reconnect.stop();
}

QTEST_GUILESS_MAIN(NiriReconnectTest)
#include "niri_reconnect_test.moc"
