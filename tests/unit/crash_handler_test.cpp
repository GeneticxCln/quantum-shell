// What the shell says when a fatal signal kills it.
//
// The handler can only be tested by being killed, so this binary runs itself: the test slots start a copy of
// this executable with a `--crash-*` argument, the copy installs the handler and dies of the named signal, and the
// slot reads what the copy wrote to standard error and how it ended. That is the whole claim under test — the
// report exists, names the version and the signal, carries a call stack, and the process still dies *of that
// signal* rather than exiting cleanly — so it is asserted on a real process death and not on a function.
#include "app/CrashHandler.h"

#include <QCoreApplication>
#include <QProcess>
#include <QTest>

#include <csignal>
#include <cstring>
#include <string_view>

using quantum::app::CrashHandler;

namespace {

constexpr auto testVersion = "9.9.9-test";

// A recursion the compiler cannot turn into a loop or drop, to overflow the stack for real. The stopping value is
// read from a volatile the compiler cannot see through, so it is not infinite recursion as far as the analysis
// can prove (which -Werror would refuse) and it is never reached at run time; the frame is touched so its pages
// are really used.
volatile int neverReached = -1;

[[gnu::noinline]] int recurseForever(int depth)
{
    volatile char frame[1024];
    frame[0] = static_cast<char>(depth);
    if (depth == neverReached)
        return depth;
    return recurseForever(depth + 1) + frame[0];
}

// The child: install the handler and die of the requested signal.
int runChild(const char* mode)
{
    CrashHandler::install(testVersion);
    if (std::strcmp(mode, "--crash-segv") == 0) {
        volatile int* nothing = nullptr;
        *nothing = 1;
    } else if (std::strcmp(mode, "--crash-abort") == 0) {
        std::abort();
    } else if (std::strcmp(mode, "--crash-fpe") == 0) {
        std::raise(SIGFPE);
    } else if (std::strcmp(mode, "--crash-overflow") == 0) {
        return recurseForever(0);
    } else if (std::strcmp(mode, "--crash-none") == 0) {
        return 0;  // installed, and nothing happens: the handler must not change a normal exit
    }
    return 2;
}

}  // namespace

class CrashHandlerTest : public QObject {
    Q_OBJECT

private slots:
    void aSegmentationFaultIsReportedAndStillKillsTheProcess();
    void anAbortIsReportedAndStillKillsTheProcess();
    void anArithmeticFaultIsReportedWithItsOwnName();
    void aStackOverflowIsReportedFromTheAlternateStack();
    void aCleanExitWithTheHandlerInstalledSaysNothing();
    void everyFatalSignalHasAName();

private:
    struct Death {
        QProcess::ExitStatus status = QProcess::NormalExit;
        int exitCode = 0;
        QString standardError;
    };
    Death runChildProcess(const char* mode) const;
};

CrashHandlerTest::Death CrashHandlerTest::runChildProcess(const char* mode) const
{
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({QString::fromLatin1(mode)});
    // Keep a sanitizer's own crash reporter from answering first: the handler under test is the one being asked.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("ASAN_OPTIONS"),
                       QStringLiteral("handle_segv=0:handle_abort=0:handle_sigfpe=0:allow_user_segv_handler=1:"
                                      "detect_leaks=0"));
    child.setProcessEnvironment(environment);
    child.start();
    if (!child.waitForFinished(20000)) {
        child.kill();
        child.waitForFinished(3000);
    }
    return {child.exitStatus(), child.exitCode(), QString::fromUtf8(child.readAllStandardError())};
}

void CrashHandlerTest::aSegmentationFaultIsReportedAndStillKillsTheProcess()
{
    const Death death = runChildProcess("--crash-segv");
    // Killed by the signal, not exited: the handler re-raises, so a core dump and a supervisor's restart still
    // work. A handler that returned would have resumed the faulting instruction; one that called exit() would be a
    // clean-looking exit and this would be NormalExit.
    QCOMPARE(death.status, QProcess::CrashExit);
    QVERIFY2(death.standardError.contains(
                 QStringLiteral("quantum-shell %1 crashed with signal 11 (SIGSEGV)").arg(QLatin1String(testVersion))),
             qPrintable(death.standardError));
    QVERIFY2(death.standardError.contains(QStringLiteral("backtrace:\n")), qPrintable(death.standardError));
    // At least one frame line follows the header: a report with the header and no stack is the failure this
    // handler exists to prevent.
    const qsizetype at = death.standardError.indexOf(QStringLiteral("backtrace:\n")) + 11;
    QVERIFY2(death.standardError.mid(at).trimmed().size() > 0, qPrintable(death.standardError));
}

void CrashHandlerTest::anAbortIsReportedAndStillKillsTheProcess()
{
    const Death death = runChildProcess("--crash-abort");
    QCOMPARE(death.status, QProcess::CrashExit);
    QVERIFY2(death.standardError.contains(QStringLiteral("crashed with signal 6 (SIGABRT)")),
             qPrintable(death.standardError));
}

void CrashHandlerTest::anArithmeticFaultIsReportedWithItsOwnName()
{
    const Death death = runChildProcess("--crash-fpe");
    QCOMPARE(death.status, QProcess::CrashExit);
    QVERIFY2(death.standardError.contains(QStringLiteral("crashed with signal 8 (SIGFPE)")),
             qPrintable(death.standardError));
}

void CrashHandlerTest::aStackOverflowIsReportedFromTheAlternateStack()
{
    // The one case where the thread's own stack cannot run a handler: the report exists only because the handler
    // is on its own stack.
    const Death death = runChildProcess("--crash-overflow");
    QCOMPARE(death.status, QProcess::CrashExit);
    QVERIFY2(death.standardError.contains(QStringLiteral("crashed with signal 11 (SIGSEGV)")),
             qPrintable(death.standardError));
    QVERIFY2(death.standardError.contains(QStringLiteral("backtrace:\n")), qPrintable(death.standardError));
}

void CrashHandlerTest::aCleanExitWithTheHandlerInstalledSaysNothing()
{
    const Death death = runChildProcess("--crash-none");
    QCOMPARE(death.status, QProcess::NormalExit);
    QCOMPARE(death.exitCode, 0);
    QVERIFY2(!death.standardError.contains(QStringLiteral("crashed")), qPrintable(death.standardError));
}

void CrashHandlerTest::everyFatalSignalHasAName()
{
    QCOMPARE(std::string_view(CrashHandler::signalName(SIGSEGV)), std::string_view("SIGSEGV"));
    QCOMPARE(std::string_view(CrashHandler::signalName(SIGBUS)), std::string_view("SIGBUS"));
    QCOMPARE(std::string_view(CrashHandler::signalName(SIGILL)), std::string_view("SIGILL"));
    QCOMPARE(std::string_view(CrashHandler::signalName(SIGFPE)), std::string_view("SIGFPE"));
    QCOMPARE(std::string_view(CrashHandler::signalName(SIGABRT)), std::string_view("SIGABRT"));
    QCOMPARE(std::string_view(CrashHandler::signalName(SIGTERM)), std::string_view("unknown"));
}

#include "crash_handler_test.moc"

int main(int argc, char* argv[])
{
    // A child of the test is this same binary with one `--crash-*` argument, and it does not start QtTest.
    if (argc == 2 && std::strncmp(argv[1], "--crash-", 8) == 0)
        return runChild(argv[1]);
    QCoreApplication app(argc, argv);
    CrashHandlerTest test;
    return QTest::qExec(&test, argc, argv);
}
