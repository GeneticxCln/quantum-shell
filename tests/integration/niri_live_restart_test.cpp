// Proves the recovery path against a niri that really restarts.
//
// Nothing here is a test double: the test starts a niri of its own — nested in the session, without
// `--session`, which niri's own help says to use for a non-main instance because it imports the
// environment globally — kills it, starts another, and checks that the shell reattached without being
// told to.
//
// The reason a real restart has to be proven rather than simulated is the socket. niri names its IPC
// socket `niri.<wayland socket name>.<pid>.sock` and unlinks it when it exits, so the path a client was
// using before a restart is not the path after it. A unit test with a fixed path cannot fail on that.
//
// It is visible on screen (the nested compositor is a window) and needs a session to nest in, so it is
// registered only when QS_NIRI_RESTART_TESTS is set alongside $NIRI_SOCKET.
#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"
#include "niri/NiriOutputs.h"
#include "niri/NiriProtocol.h"
#include "niri/NiriReconnect.h"
#include "niri/NiriState.h"
#include "NestedCompositor.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using quantum::niri::NiriEventStream;
using quantum::niri::NiriIPC;
using quantum::niri::NiriOutputs;
using quantum::niri::NiriReconnect;
using quantum::niri::NiriState;
using quantum::niri::Reply;

namespace {

// A compositor run and stopped by this test: NestedNiri, from support/NestedCompositor.h.

}  // namespace

class NiriLiveRestartTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void theShellReattachesAcrossARealCompositorRestart();

private:
    // The newest niri socket that is not the session's: the compositor this test started.
    QString testSocket() const;
    void reportState(const QString& when);

    NiriIPC request_;
    NiriEventStream stream_;
    NiriState state_;
    NiriReconnect reconnect_;
    std::unique_ptr<NiriOutputs> outputs_;
    QTemporaryDir directory_;
    QString configPath_;
    QString niriPath_;
    QString sessionSocket_;
};

void NiriLiveRestartTest::initTestCase() {
    niriPath_ = QStandardPaths::findExecutable(QStringLiteral("niri"));
    if (niriPath_.isEmpty()) {
        QSKIP("no niri on PATH: this test starts a compositor of its own to restart");
    }
    if (qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()) {
        QSKIP("no Wayland display to nest a compositor in; a restarted compositor needs one it can run in");
    }
    sessionSocket_ = quantum::niri::niriSocketPath();
    if (sessionSocket_.isEmpty()) {
        QSKIP("$NIRI_SOCKET is not set, so this test cannot tell its own compositor from the session's");
    }
    if (!QFile::exists(sessionSocket_)) {
        QSKIP(qPrintable(QStringLiteral("$NIRI_SOCKET names %1, which does not exist").arg(sessionSocket_)));
    }
    if (!directory_.isValid()) {
        QFAIL("the test could not create a directory for the test compositor's config");
    }

    configPath_ = directory_.filePath(QStringLiteral("config.kdl"));
    const QByteArray contents(nestedCompositorConfig);
    QFile config(configPath_);
    QVERIFY2(config.open(QIODevice::WriteOnly | QIODevice::Text), qPrintable(config.errorString()));
    QCOMPARE(config.write(contents), static_cast<qint64>(contents.size()));
    config.close();

    // niri's own validator, so the compositor's config is known good before a failure could be blamed on
    // the compositor not starting.
    QProcess validate;
    validate.start(niriPath_, QStringList{QStringLiteral("validate"), QStringLiteral("-c"), configPath_});
    QVERIFY2(validate.waitForFinished(10000), "niri validate did not finish");
    QCOMPARE(validate.exitCode(), 0);

    state_.observe(stream_);
    outputs_ = std::make_unique<NiriOutputs>(request_);
    state_.observeOutputs(*outputs_);
    outputs_->observe(stream_);

    reconnect_.keepAttached(request_);
    reconnect_.keepAttached(stream_);
    // The supervisor finds the compositor the way the test has to: never the session's socket.
    reconnect_.setSocketLocator([this] { return testSocket(); });

    qInfo("this process was started with %s (%s); the test compositor will be another socket entirely",
          qPrintable(sessionSocket_), "that one is left alone");
}

void NiriLiveRestartTest::cleanupTestCase() {
    reconnect_.stop();
}

QString NiriLiveRestartTest::testSocket() const {
    // `niriSocketFiles()` is newest first, so the first socket that is not the session's is the one the
    // compositor this test started created — and while there is none, the empty answer is what tells the
    // supervisor there is nothing to attach to yet.
    for (const QString& candidate : quantum::niri::niriSocketFiles()) {
        if (candidate != sessionSocket_) {
            return candidate;
        }
    }
    return QString();
}

void NiriLiveRestartTest::reportState(const QString& when) {
    qInfo("%s: %lld workspaces, %lld windows, %lld outputs, keyboard layouts %s, overview %s",
          qPrintable(when), static_cast<long long>(state_.workspaces().size()),
          static_cast<long long>(state_.windows().size()),
          static_cast<long long>(state_.outputs().size()),
          state_.keyboardLayouts().isValid()
              ? qPrintable(QStringLiteral("\"%1\"").arg(state_.keyboardLayouts().currentName()))
              : "unknown",
          state_.isOverviewOpen() ? "open" : "closed");
}

void NiriLiveRestartTest::theShellReattachesAcrossARealCompositorRestart() {
    QSignalSpy attached(&reconnect_, &NiriReconnect::attached);
    QSignalSpy lost(&reconnect_, &NiriReconnect::lost);
    QSignalSpy versions(&request_, &NiriIPC::versionDetected);
    QList<int> retryDelays;
    connect(&reconnect_, &NiriReconnect::retryScheduled, this,
            [&retryDelays](int, int delayMs, const QString&) { retryDelays.append(delayMs); });

    // --- the compositor this test owns -----------------------------------------------------------------
    NestedNiri first;
    QString reason;
    QVERIFY2(first.start(niriPath_, configPath_, sessionSocket_, &reason), qPrintable(reason));
    const QString socketBefore = first.socket();
    QVERIFY2(QFile::exists(socketBefore),
             qPrintable(QStringLiteral("the test compositor reported %1 but it does not exist")
                            .arg(socketBefore)));

    QElapsedTimer timer;
    timer.start();
    reconnect_.start();
    QTRY_VERIFY_WITH_TIMEOUT(reconnect_.isAttached(), 30000);
    const qint64 attachMs = timer.elapsed();

    // The state a freshly attached connection has to have on its own: nothing here reads it by hand.
    QTRY_VERIFY_WITH_TIMEOUT(!state_.workspaces().isEmpty(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!state_.outputs().isEmpty(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(state_.keyboardLayouts().isValid(), 10000);
    QCOMPARE(versions.count(), 1);

    const int workspacesBefore = state_.workspaces().size();
    const int outputsBefore = state_.outputs().size();
    const QString outputNameBefore =
        state_.outputs().isEmpty() ? QString() : state_.outputs().first().name();
    qInfo("attached to a real niri at %s in %lld ms; the session's own compositor at %s was not touched",
          qPrintable(socketBefore), static_cast<long long>(attachMs), qPrintable(sessionSocket_));
    reportState("state as given by the compositor");
    qInfo("the compositor reports output \"%s\"", qPrintable(outputNameBefore));

    // --- the compositor goes away ---------------------------------------------------------------------
    first.stop();
    QVERIFY2(!first.wasKilled(),
             qPrintable(QStringLiteral("the test compositor needed SIGKILL; its output:\n%1")
                            .arg(first.tail())));
    // niri unlinks its socket on a clean exit, so the path the supervisor was using is gone and the next
    // compositor will not reuse it.
    QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(socketBefore), 10000);
    QVERIFY2(testSocket().isEmpty(),
             qPrintable(QStringLiteral("another niri socket appeared where only the session's should be")));

    QTRY_COMPARE(lost.count(), 1);
    QTRY_VERIFY(state_.workspaces().isEmpty());
    QVERIFY2(state_.outputs().isEmpty(), "outputs survived a compositor that is gone");
    QVERIFY2(!state_.keyboardLayouts().isValid(), "layouts survived a compositor that is gone");
    reportState("while the compositor is gone");

    // --- the backoff, measured while it is down -------------------------------------------------------
    const qint64 downAt = timer.elapsed();
    QTest::qWait(900);
    QVERIFY2(retryDelays.size() >= 2,
             qPrintable(QStringLiteral("only %1 retry was scheduled in 900 ms with no compositor")
                            .arg(retryDelays.size())));
    for (int index = 1; index < retryDelays.size(); ++index) {
        QVERIFY2(retryDelays.at(index) >= retryDelays.at(index - 1),
                 "the retry delay is not growing");
    }
    qInfo("%lld ms with no compositor: %lld retries, delays %s ms (first %d, capped at %d)",
          static_cast<long long>(timer.elapsed() - downAt),
          static_cast<long long>(retryDelays.size()),
          qPrintable(QStringList([&retryDelays] {
                                     QStringList shown;
                                     for (const int delay : retryDelays) {
                                         shown.append(QString::number(delay));
                                     }
                                     return shown;
                                 }())
                         .join(QLatin1Char(','))),
          reconnect_.policy().firstDelayMs, reconnect_.policy().maximumDelayMs);

    // --- another compositor, on a different socket ----------------------------------------------------
    NestedNiri second;
    QVERIFY2(second.start(niriPath_, configPath_, sessionSocket_, &reason), qPrintable(reason));
    const QString socketAfter = second.socket();
    QVERIFY2(socketAfter != socketBefore,
             qPrintable(QStringLiteral("the restarted compositor reused the old socket path %1, so this "
                                       "run did not exercise rediscovery")
                            .arg(socketAfter)));
    qInfo("a new compositor is up at %s, a different path from %s", qPrintable(socketAfter),
          qPrintable(socketBefore));

    // Nothing tells the supervisor to try again: it was already retrying, and the retry finds the new
    // socket by niri's naming rule.
    timer.restart();
    QTRY_VERIFY_WITH_TIMEOUT(reconnect_.isAttached(), 30000);
    const qint64 recoveryMs = timer.elapsed();

    QCOMPARE(attached.count(), 2);
    // One outage was reported for the whole restart, not one per failed attempt.
    QCOMPARE(lost.count(), 1);
    // The version was asked again, because a compositor can be a different build after a restart.
    QCOMPARE(versions.count(), 2);

    // And the state is rebuilt from the subscription the new compositor answers with.
    QTRY_VERIFY_WITH_TIMEOUT(!state_.workspaces().isEmpty(), 10000);
    QTRY_COMPARE(state_.workspaces().size(), workspacesBefore);
    QTRY_COMPARE(state_.outputs().size(), outputsBefore);
    QTRY_VERIFY_WITH_TIMEOUT(state_.keyboardLayouts().isValid(), 10000);

    // The request connection is not merely connected: a request goes out and is answered.
    std::optional<Reply> version;
    request_.send(QStringLiteral("Version"), [&version](const Reply& reply) { version = reply; });
    QVERIFY2(QTest::qWaitFor([&version] { return version.has_value(); }, 5000),
             "the reattached request connection never answered");
    QVERIFY2(version->isOk(), qPrintable(version->describe()));

    qInfo("recovered %lld ms after a new compositor appeared (%lld ms of retrying while none was up)",
          static_cast<long long>(recoveryMs), static_cast<long long>(recoveryMs));
    reportState("state after the restart");

    qInfo("niri %s was restarted on this session: the shell reattached, rediscovered its socket and "
          "rebuilt its state on its own",
          qPrintable(version->variant(QStringLiteral("Version")).toString()));

    second.stop();
    reconnect_.stop();
}

QTEST_GUILESS_MAIN(NiriLiveRestartTest)
#include "niri_live_restart_test.moc"
