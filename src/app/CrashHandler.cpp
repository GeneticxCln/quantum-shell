#include "app/CrashHandler.h"

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>

namespace quantum::app {

namespace {

constexpr int fatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

// The version, copied out of whatever owned it: the handler cannot touch a QString or a std::string, so the text
// it needs is a plain character array with a terminator that was written before any signal could arrive.
char versionText[64] = "unknown";

// Frames to print. Deep enough to show where the process was and shallow enough to fit a buffer of frame
// addresses on the alternate stack.
constexpr int maxFrames = 48;

// The alternate stack. Static rather than allocated so installing cannot fail for want of memory, and large enough
// for `backtrace_symbols_fd`, which formats one line at a time and needs little.
alignas(16) char alternateStack[64 * 1024];

void writeAll(const char* text, std::size_t length)
{
    while (length > 0) {
        const ssize_t written = ::write(STDERR_FILENO, text, length);
        if (written <= 0)
            return;  // nothing useful to do about a stderr that cannot be written in a crashing process
        text += written;
        length -= static_cast<std::size_t>(written);
    }
}

void writeText(const char* text)
{
    writeAll(text, std::strlen(text));
}

// A non-negative integer as decimal, by hand: `snprintf` is not async-signal-safe.
void writeNumber(int value)
{
    char digits[16];
    int count = 0;
    if (value <= 0) {
        writeText("0");
        return;
    }
    while (value > 0 && count < 15) {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    char forward[16];
    for (int i = 0; i < count; ++i)
        forward[i] = digits[count - 1 - i];
    writeAll(forward, static_cast<std::size_t>(count));
}

void handleFatalSignal(int signal)
{
    writeText("quantum-shell ");
    writeText(versionText);
    writeText(" crashed with signal ");
    writeNumber(signal);
    writeText(" (");
    writeText(CrashHandler::signalName(signal));
    writeText(")\nbacktrace:\n");

    void* frames[maxFrames];
    const int count = ::backtrace(frames, maxFrames);
    ::backtrace_symbols_fd(frames, count, STDERR_FILENO);

    // The default action, raised again: the process still dies of the signal, so a core is still written and the
    // supervisor still sees a crash. `SA_RESETHAND` below has already restored the default, but restoring it here
    // as well is what makes this correct for a signal that arrived while another handler was running.
    ::signal(signal, SIG_DFL);
    ::raise(signal);
}

}  // namespace

const char* CrashHandler::signalName(int signal)
{
    switch (signal) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "unknown";
    }
}

void CrashHandler::install(const char* version)
{
    std::strncpy(versionText, version != nullptr ? version : "unknown", sizeof(versionText) - 1);
    versionText[sizeof(versionText) - 1] = '\0';

    // The one non-async-signal-safe step of the handler, taken now: the first `backtrace` call loads the
    // unwinder, which allocates. After this a handler only reads.
    void* warm[2];
    ::backtrace(warm, 2);

    stack_t stack{};
    stack.ss_sp = alternateStack;
    stack.ss_size = sizeof(alternateStack);
    stack.ss_flags = 0;
    ::sigaltstack(&stack, nullptr);

    struct sigaction action{};
    action.sa_handler = handleFatalSignal;
    sigemptyset(&action.sa_mask);
    // ONSTACK: run on the alternate stack, which is what makes a stack overflow reportable. RESETHAND: a fault
    // inside the handler itself gets the default action instead of recursing.
    action.sa_flags = SA_ONSTACK | SA_RESETHAND;
    for (const int signal : fatalSignals)
        ::sigaction(signal, &action, nullptr);
}

}  // namespace quantum::app
