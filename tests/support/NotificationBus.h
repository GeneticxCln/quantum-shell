// A `dbus-daemon` a test starts for itself, with the shell's notification service file in a service
// directory of its own — so `org.freedesktop.Notifications` is free for the shell to take here and can never
// be taken from it.
//
// **Why not the desktop's own session bus.** `dbus-daemon --session` inherits the desktop's service
// directories, and a service file for `org.freedesktop.Notifications` there carries a `SystemdService=` line
// — `swaync` on the machine this was first measured on — so a sender's request for the name asks systemd to
// start a process, and systemd does not read `XDG_DATA_DIRS`. The desktop's real notifier then takes the name
// before the shell can and answers the call itself. Measured rather than assumed: against a `--session` bus
// with the shell not yet holding the name, a `GetServerInformation` call was answered by
// `SwayNotificationCenter` in half a second, and emptying `XDG_DATA_DIRS` did not stop it.
//
// A config file of the test's own is the isolation that works, because a config names its service directories
// rather than inheriting them: this one names a directory holding the shell's own service file and nothing
// else, so the bus has exactly one way to provide the name and a sender cannot reach any other daemon.
//
// **Why the directory is this process's own.** Two binaries of this suite run at the same time — `ctest -j4`,
// and the four shuffled slot-order shards — so a path shared between them would have one process deleting and
// rewriting the service file while the other's bus was reading it: a failure that shows up under load and
// nowhere else. The process id in the path makes the run directory, the config naming it and the address the
// daemon prints private to the process that asked, and the config is therefore **written** rather than
// checked in — its whole content is the service directory below, which is a path only the running process
// knows. The service file itself stays a fixture, because it is the same file on every run: the one name a
// sender resolving `org.freedesktop.Notifications` reaches when no daemon holds it.
#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QString>

namespace qstest {

// The service file's name in the bus's service directory. It is the fixture's name too — the bus finds
// services by reading the directory, so the file has to be there under its own name.
inline constexpr auto NotificationsServiceFileName = "quantum-shell.notification.service";

class NotificationBus {
public:
    // Starts the daemon, writing a config that names a service directory of this process's own and putting
    // the shell's service file in it. `fixturesDirectory` is the tree the file is copied out of — the bus's
    // service directory holds files, so a copy of the fixture is what a desktop's directory would hold.
    //
    // False means the daemon could not be started (no `dbus-daemon`, or it would not serve), which is a fact
    // about the machine rather than a defect of anything under test: `error()` says which, so the caller can
    // report it the way this suite reports an unusable external program — by skipping the claims that need
    // it, naming the reason, rather than failing a run for a bus that was never there.
    bool start(const QString& fixturesDirectory) {
        runDirectory_ = QDir::tempPath() + QStringLiteral("/quantum-shell-notification-bus-")
            + QString::number(QCoreApplication::applicationPid());
        if (!QDir().mkpath(serviceDirectory())) {
            error_ = QStringLiteral("could not create %1").arg(serviceDirectory());
            return false;
        }

        // The service file the notifications name resolves to. `Exec=/bin/true` is what makes it a *dead end*
        // rather than a daemon: the bus tries to activate it, the program exits at once, and the name stays
        // unowned — so a sender that asks for the name while the shell does not hold it is told nothing is
        // listening rather than being served by something else. That is the condition `notify-send`'s exit
        // status reports, and `notification-test` reads it there.
        const QString serviceFile = serviceDirectory() + QLatin1Char('/')
            + QLatin1String(NotificationsServiceFileName);
        const QString fixture = fixturesDirectory + QStringLiteral("/notification-bus/")
            + QLatin1String(NotificationsServiceFileName);
        if (!copy(fixture, serviceFile)) {
            // Both paths, because the two ways this fails — a fixtures directory that is not where the caller
            // thinks it is, and a service directory this process cannot write — are told apart by which of them
            // is wrong, and a message naming one would make a person read the other half of the code to find out.
            error_ = QStringLiteral("could not install the notifications service file: %1 -> %2")
                         .arg(fixture, serviceFile);
            return false;
        }

        const QString configPath = runDirectory_ + QStringLiteral("/bus.conf");
        QFile config(configPath);
        if (!config.open(QIODevice::WriteOnly | QIODevice::Truncate) || !config.write(configText().toUtf8())
            || !config.flush()) {
            error_ = QStringLiteral("could not write %1").arg(configPath);
            return false;
        }
        config.close();

        // `--print-address=1` is the reference daemon's own way of telling a parent where its bus is, and
        // `--nofork` is what keeps it in this process's tree so it dies with the test.
        process_.start(QStringLiteral("dbus-daemon"),
                       {QStringLiteral("--config-file"), configPath, QStringLiteral("--print-address=1"),
                        QStringLiteral("--nofork")});
        if (!process_.waitForStarted(5000)) {
            error_ = QStringLiteral("dbus-daemon could not be started: %1").arg(process_.errorString());
            return false;
        }

        // The address is the one line the daemon writes before it serves, and reading it is a wait rather
        // than a single `readLine()`: the daemon writes it as soon as its socket is bound, but the write and
        // the read are not synchronised, and a line taken the moment `waitForReadyRead` returns can still be
        // split across the buffer boundary — measured as `waitForReadyRead` succeeding with
        // `bytesAvailable() == 0` and the line arriving anyway. So the wait is for the line itself, and
        // anything that is not a complete address is waited for again rather than accepted as an empty one.
        QElapsedTimer elapsed;
        elapsed.start();
        while (address_.isEmpty() && elapsed.elapsed() < 5000) {
            process_.waitForReadyRead(1000);
            address_ = QString::fromUtf8(process_.readLine()).trimmed();
        }
        if (address_.isEmpty()) {
            error_ = QStringLiteral("dbus-daemon wrote no address, so this test's bus was never reached");
            return false;
        }
        return true;
    }

    // The address a connection to this bus is made with, and what a child process is told to use.
    QString address() const { return address_; }

    // The bus is up and was reached. False after a failed `start()`, so a caller can ask before it depends on
    // the address.
    bool isRunning() const { return !address_.isEmpty(); }

    // Why the bus is not there, for the reason a skip gives a person.
    QString error() const { return error_; }

    // Where the bus looks for services. Named here rather than reconstructed by a caller: it is the path the
    // config points at, and a second spelling of it in a test would be a second place to change.
    QString serviceDirectory() const { return runDirectory_ + QStringLiteral("/services"); }

    ~NotificationBus() {
        process_.terminate();
        if (!process_.waitForFinished(3000)) {
            process_.kill();
            process_.waitForFinished(3000);
        }
        // And the run directory goes with the daemon. It has to: the shuffled slot-order check runs this binary
        // thousands of times, and a directory per pass is a thousand directories left in the system's scratch
        // space — measured, not feared: 1,164 of them, one per pass of a single suite run, before this line
        // existed. What is in it is this process's own — the config it wrote and the service file it copied.
        if (!runDirectory_.isEmpty()) {
            QDir(runDirectory_).removeRecursively();
        }
    }

    NotificationBus() = default;
    NotificationBus(const NotificationBus&) = delete;
    NotificationBus& operator=(const NotificationBus&) = delete;

private:
    // The config, as text, for the reason the class comment gives: its one varying line is the path of this
    // process's own service directory. The policy is the permissive one a test bus needs — a connection that
    // may own any name and address any name — and it is a `session` bus so that a sender looking for the
    // session bus finds this one.
    QString configText() const {
        return QStringLiteral(
                   "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN\"\n"
                   " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                   "<busconfig>\n"
                   "  <type>session</type>\n"
                   "  <listen>unix:tmpdir=/tmp</listen>\n"
                   "  <servicedir>%1</servicedir>\n"
                   "  <policy context=\"default\">\n"
                   "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                   "    <allow eavesdrop=\"true\"/>\n"
                   "    <allow own=\"*\"/>\n"
                   "  </policy>\n"
                   "</busconfig>\n")
            .arg(serviceDirectory());
    }

    static bool copy(const QString& from, const QString& to) {
        if (QFile::exists(to) && !QFile::remove(to))
            return false;
        return QFile::copy(from, to);
    }

    QProcess process_;
    QString address_;
    QString error_;
    QString runDirectory_;
};

}  // namespace qstest
