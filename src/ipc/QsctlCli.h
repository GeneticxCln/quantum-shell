// What a `qsctl` command line means, and what its answer looks like on a terminal.
//
// This is separate from `src/ipc/qsctl/main.cpp` on purpose. The binary is the part that touches a socket
// and cannot be tested without one; everything that can be decided from strings — which words name which
// verb, what a usage error says, how a response is printed and with which exit code — is here, where the
// unit test drives it directly. What is left in `main.cpp` is a connection and four calls.
//
// Two decisions are worth stating, because they are the ones a script depends on:
//
//   * **Exit codes are interface.** A script branches on them rather than parsing text, so zero means the
//     shell answered, and every other code means something specific about why it did not: a refusal is not
//     the same failure as no shell running, and neither is the same as a command line `qsctl` cannot parse.
//   * **`config get` prints the bare value** and every other verb prints one line of JSON. That verb exists
//     to be used as `x=$(qsctl config get bar.height)`, and quoting a number for a JSON document would make
//     that the one command with a wrapper around the answer it was asked for.
//
// A verb this build does not implement is refused as a *usage* error naming the words, not sent to the
// shell to be refused there: `qsctl volume up` is a command the design document mentions and this shell has
// no audio service for, so the honest answer is that qsctl does not take it — not a request the shell
// receives and declines, which would read as "the shell is broken" rather than "that is not a command".
#pragma once

#include "ipc/IPCProtocol.h"

#include <QString>
#include <QStringList>

namespace quantum::ipc {

// The exit codes `qsctl` returns. Named here so the tests can say which one they expect rather than
// repeating an integer, and documented in QUANTUM_SHELL.md § IPC for whoever writes the script.
enum class ExitCode : int {
    // The shell answered and the answer was used.
    ok = 0,
    // The shell answered and refused. The message says what was refused.
    refused = 1,
    // The command line is not a command this qsctl takes. Nothing was sent.
    usage = 2,
    // No shell is listening, or its answer never arrived.
    unreachable = 3,
    // A shell is listening and speaks a different protocol. Both versions are named.
    protocolMismatch = 4,
};

// One parsed command line.
struct Command {
    // The request to send. Meaningless when `problem` is set.
    Request request;

    // Why this command line is not a request, in a sentence that names the words at fault. Empty when the
    // line is valid, which is the only case in which anything is sent.
    QString problem;
};

// Parses the arguments *after* the program name.
Command parseCommandLine(const QStringList& arguments);

// The list of commands, printed after a usage error so the reader sees what they could have typed.
QString usageText();

// What to print and what to exit with.
struct Outcome {
    int exitCode = int(ExitCode::ok);

    // Whether the text is a refusal, which belongs on stderr, rather than an answer, which belongs on
    // stdout: a script that pipes stdout must not find a diagnostic in the value it captured.
    bool toStandardError = false;

    QString text;
};

// Turns a response into that. The protocol version is compared before the refusal is read, because the
// refusal of a shell speaking a different protocol is about a request it may not have understood.
Outcome interpret(const Request& request, const Response& response);

}  // namespace quantum::ipc
