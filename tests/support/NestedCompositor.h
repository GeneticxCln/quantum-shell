// A compositor run and stopped by a test: a niri started nested in the session, without `--session`,
// which niri's own help says to use for a non-main instance because it imports the environment globally.
//
// Nothing here is a test double: the compositor is real, and so is the way a client finds it — niri
// names its IPC socket `niri.<wayland socket name>.<pid>.sock` and unlinks it when it exits, so the
// path a client was using before a restart is not the path after it. That is the fact this class makes
// the test live with: the socket of the instance it started is found by niri's own naming rule, never
// inherited from the session, and a second instance necessarily lands on a different path.
//
// Taken from tests/integration/niri_live_restart_test.cpp, which started a compositor of its own before
// there was a second test that needed to.
#pragma once

#include "niri/NiriProtocol.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QRegularExpression>
#include <QString>
#include <QTest>

// The config for the compositor a test starts. Nothing but a comment, so the instance cannot inherit
// anything from the session it runs in; the test runs niri's own validator over it before use, so a
// broken fixture fails as a validation error rather than as a mysterious startup timeout.
inline constexpr auto nestedCompositorConfig =
    "// The compositor this file configures is started by a Quantum Shell live test.\n";

// A compositor run and stopped by the test that owns it.
class NestedNiri {
public:
    ~NestedNiri() { stop(); }

    bool start(const QString& niriPath, const QString& configPath, const QString& sessionSocket,
               QString* reason) {
        sessionSocket_ = sessionSocket;
        process_.setProcessChannelMode(QProcess::MergedChannels);
        // niri logs its startup in detail. The pipe is drained continuously so the compositor can never
        // block writing to it, and the tail is kept for a failure message.
        QObject::connect(&process_, &QProcess::readyRead, &process_, [this] { collectOutput(); });

        process_.start(niriPath, QStringList{QStringLiteral("-c"), configPath});
        if (!process_.waitForStarted(5000)) {
            *reason = QStringLiteral("the test compositor did not start: %1").arg(process_.errorString());
            return false;
        }

        socket_ = waitForSocket();
        if (socket_.isEmpty()) {
            *reason = QStringLiteral("the test compositor never created an IPC socket. Its output:\n%1")
                          .arg(tail());
            return false;
        }
        waylandDisplay_ = waitForWaylandDisplay();
        if (waylandDisplay_.isEmpty()) {
            *reason = QStringLiteral("the test compositor created no Wayland display for its clients. "
                                     "Its output:\n%1")
                          .arg(tail());
            return false;
        }
        return true;
    }

    // The socket this instance created, found the way a client has to find it: niri's naming rule, and
    // never the socket this test process was started with, which belongs to the session's own compositor.
    QString socket() const { return socket_; }

    // The Wayland display name this instance serves, read the way its log announces it. A nested niri
    // picks the next free name (wayland-2, wayland-3, ...), which is what a client of *this* instance
    // must be given rather than the session's display.
    QString waylandDisplay() const { return waylandDisplay_; }

    void stop() {
        if (process_.state() == QProcess::NotRunning) {
            return;
        }
        collectOutput();
        process_.terminate();
        if (!process_.waitForFinished(5000)) {
            // SIGTERM is not enough, which matters because a clean exit is what unlinks the socket; the
            // test reports it rather than treating a killed compositor as a clean one.
            killed_ = true;
            process_.kill();
            process_.waitForFinished(2000);
        }
        collectOutput();
    }

    bool wasKilled() const { return killed_; }
    QString tail() const { return log_; }

private:
    QString waitForWaylandDisplay() {
        // niri names its display in its own startup log; every other source (a directory listing of
        // $XDG_RUNTIME_DIR) could race a session client creating its own socket there. The log line is
        // the compositor's own word for what it created.
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            collectOutput();
            static const QRegularExpression pattern(
                QStringLiteral("(?<!=)wayland-\\d+"));
            const QRegularExpressionMatch match = pattern.match(log_);
            if (match.hasMatch()) {
                return match.captured();
            }
            QTest::qWait(25);
        }
        return QString();
    }

    QString waitForSocket() {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            for (const QString& candidate : quantum::niri::niriSocketFiles()) {
                if (candidate != sessionSocket_) {
                    return candidate;
                }
            }
            QTest::qWait(25);
            collectOutput();
        }
        return QString();
    }

    void collectOutput() {
        const QString text = QString::fromUtf8(process_.readAll());
        log_ += text;
        // Only the tail is ever shown, so the log cannot grow without bound while the compositor runs.
        if (log_.size() > 4000) {
            log_ = log_.right(4000);
        }
    }

    QProcess process_;
    QString sessionSocket_;
    QString socket_;
    QString waylandDisplay_;
    QString log_;
    bool killed_ = false;
};
