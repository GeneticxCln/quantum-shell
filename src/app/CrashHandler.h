// What the shell says when it is killed by a fatal signal, and nothing else.
//
// A shell started by niri has no terminal and nobody watching: when it dies of a segmentation fault the only
// account of it is what it wrote before it went. Without this, that is nothing — the journal shows a process that
// exited with a signal, and the version, the signal and the frames that were running are all lost. So for the
// signals that mean the process is corrupt and cannot continue (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT), a
// handler writes one line naming the version and the signal and then the call stack, to standard error, which is
// where the journal collects it, and then does the one thing that keeps every other tool working: it restores the
// signal's default action and raises it again. The process still dies *of that signal*, so a core dump is still
// written, `coredumpctl` still sees it, and a supervisor still sees a crash rather than a clean exit and restarts
// the shell (which is what the compositor-restart test's exit-code contract relies on).
//
// It does not try to recover, and it does not write a file, a dump or a report: a handler that runs inside a
// corrupt process must do as little as possible, and a durable copy of a crashed process's memory is exactly the
// kind of thing that can hold what should not be kept. What it prints is a version, a signal name and return
// addresses with the symbols the dynamic linker can name — nothing from the process's data.
//
// Everything in the handler is async-signal-safe: it formats into a stack buffer by hand and calls `write`, and
// the call stack is `backtrace` + `backtrace_symbols_fd`, whose one non-safe step — the first call loads the
// unwinder — is taken once at install time, before any signal can arrive. It runs on an alternate stack, so a
// crash by stack overflow, which is the one case where the thread's own stack cannot run a handler, is reported
// as well.
#pragma once

namespace quantum::app {

class CrashHandler
{
public:
    // Installs the handler for the fatal signals. `version` is the build's version string, printed in the report;
    // it must outlive the process (a string literal or `QCoreApplication`'s stored copy is not enough, so it is
    // copied into a static buffer here). Idempotent: a second call replaces the version and reinstalls.
    static void install(const char* version);

    // The name a fatal signal is reported under, or "unknown" for anything else. Public so the report's spelling
    // is a function a test can call, and because the handler is not the only place a signal is named.
    static const char* signalName(int signal);
};

}  // namespace quantum::app
