// `qsctl`: the command the shell is answered by.
//
// This file is a connection and four calls. Everything decidable from strings — which command line means
// which verb, what a usage error says, how an answer prints, which exit code it returns — is in
// `src/ipc/QsctlCli.cpp` where a test drives it without a socket. What is left here is the part that needs
// a running shell, and it holds no decision a test could have made.
//
// It talks to the abstract socket of `IPCProtocol.h`, and it sets `AbstractNamespaceOption` for the reason
// the constant's comment gives: the option is what adds the abstract namespace's leading NUL, and a client
// without it addresses a different socket — "connection refused" while the shell is plainly listening.
#include "ipc/IPCProtocol.h"
#include "ipc/QsctlCli.h"

#include <QCoreApplication>
#include <QLocalSocket>
#include <QString>
#include <QTextStream>

namespace {

// A local shell answers in well under a millisecond; the margin is for a machine under load, and it exists
// so that a shell which is not coming back produces a message rather than a hang.
constexpr int connectTimeoutMs = 2000;
constexpr int answerTimeoutMs = 5000;

int report(const QString& text, bool toStandardError)
{
    QTextStream stream(toStandardError ? stderr : stdout);
    stream << text << '\n';
    stream.flush();
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qsctl"));
    QCoreApplication::setApplicationVersion(QStringLiteral(QS_VERSION));

    const quantum::ipc::Command command =
        quantum::ipc::parseCommandLine(QCoreApplication::arguments().mid(1));
    if (!command.problem.isEmpty()) {
        // The problem comes first and the usage after it, so the line that explains this particular mistake
        // is not buried under the list of commands that would have worked.
        report(command.problem + QLatin1Char('\n') + quantum::ipc::usageText(), true);
        return int(quantum::ipc::ExitCode::usage);
    }

    QLocalSocket socket;
    socket.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
    socket.connectToServer(QString::fromLatin1(quantum::ipc::SocketName));
    if (!socket.waitForConnected(connectTimeoutMs)) {
        report(QStringLiteral("no Quantum Shell is listening on the abstract socket \\0%1 (%2)")
                   .arg(QString::fromLatin1(quantum::ipc::SocketName), socket.errorString()),
               true);
        return int(quantum::ipc::ExitCode::unreachable);
    }

    QByteArray frame = quantum::ipc::encodeRequest(command.request);
    frame.append('\n');
    socket.write(frame);
    if (!socket.waitForBytesWritten(connectTimeoutMs)) {
        report(QStringLiteral("the shell accepted the connection and then stopped reading (%1)")
                   .arg(socket.errorString()),
               true);
        return int(quantum::ipc::ExitCode::unreachable);
    }

    // One request, one response line: the loop reads until a whole line has arrived, because a socket is a
    // stream and a read can return half of one.
    QByteArray answer;
    while (!answer.contains('\n')) {
        if (!socket.waitForReadyRead(answerTimeoutMs)) {
            report(QStringLiteral("the shell did not answer within %1 ms").arg(answerTimeoutMs), true);
            return int(quantum::ipc::ExitCode::unreachable);
        }
        answer.append(socket.readAll());
        if (answer.size() > quantum::ipc::MaxLineBytes) {
            report(QStringLiteral("the shell's answer is longer than %1 bytes and was not read")
                       .arg(quantum::ipc::MaxLineBytes),
                   true);
            return int(quantum::ipc::ExitCode::unreachable);
        }
    }

    quantum::ipc::Response response;
    const QString decodeError =
        quantum::ipc::decodeResponse(answer.left(answer.indexOf('\n')), &response);
    if (!decodeError.isEmpty()) {
        report(QStringLiteral("the shell's answer could not be read: %1").arg(decodeError), true);
        return int(quantum::ipc::ExitCode::unreachable);
    }

    const quantum::ipc::Outcome outcome = quantum::ipc::interpret(command.request, response);
    report(outcome.text, outcome.toStandardError);
    return outcome.exitCode;
}
