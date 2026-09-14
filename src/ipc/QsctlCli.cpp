#include "ipc/QsctlCli.h"

#include <QJsonDocument>
#include <QJsonValue>

#include <optional>

namespace quantum::ipc {
namespace {

// The words a command line is written in. They are the same as the verbs on the wire, which is what makes
// `qsctl config get bar.height` send the verb `config get`: a client that types a verb types the verb.
constexpr auto VersionWord = "version";
constexpr auto StateWord = "state";
constexpr auto ConfigWord = "config";
constexpr auto GetWord = "get";
constexpr auto BarWord = "bar";
constexpr auto ToggleWord = "toggle";

Command usageProblem(const QString& problem)
{
    Command command;
    command.problem = problem;
    return command;
}

// A request for `verb`, or the usage error for a command line that gave it the wrong arguments. Every verb
// below takes no argument except `config get`, so the same check is written once: the words after the verb
// are counted, and any that are there are named.
std::optional<QString> tooManyArguments(const QStringList& arguments, qsizetype taken)
{
    if (arguments.size() <= taken)
        return std::nullopt;
    return QStringLiteral("\"%1\" takes no more arguments, and it was given \"%2\"")
        .arg(arguments.mid(0, taken).join(QLatin1Char(' ')),
             arguments.mid(taken).join(QLatin1Char(' ')));
}

QString numberOrString(const QJsonValue& value)
{
    if (value.isString())
        return value.toString();
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) {
        // Printed as an integer when it is one: a height of 32 is not `32.0`, and a script comparing the
        // answer against `32` should not have to know how JSON numbers are held internally.
        const double number = value.toDouble();
        if (qFuzzyCompare(number, qRound64(number)))
            return QString::number(qRound64(number));
        return QString::number(number);
    }
    return QString::fromUtf8(QJsonDocument::fromVariant(value.toVariant()).toJson(QJsonDocument::Compact));
}

QString formatSuccess(const Request& request, const Response& response)
{
    if (request.verb == QLatin1StringView(verb::Version)) {
        return QStringLiteral("%1 %2, protocol %3")
            .arg(response.data.value(QStringLiteral("name")).toString(),
                 response.data.value(QStringLiteral("shell")).toString())
            .arg(response.data.value(QStringLiteral("protocol")).toInt());
    }

    if (request.verb == QLatin1StringView(verb::ConfigGet))
        return numberOrString(response.data.value(QStringLiteral("value")));

    return QString::fromUtf8(QJsonDocument(response.data).toJson(QJsonDocument::Compact));
}

}  // namespace

Command parseCommandLine(const QStringList& arguments)
{
    if (arguments.isEmpty()) {
        return usageProblem(QStringLiteral(
            "no command was given, and qsctl does not connect to the shell without one"));
    }

    const QString& first = arguments.first();

    if (first == QLatin1StringView(VersionWord) || first == QLatin1StringView(StateWord)) {
        if (const std::optional<QString> problem = tooManyArguments(arguments, 1); problem.has_value())
            return usageProblem(*problem);
        Command command;
        command.request.version = ProtocolVersion;
        command.request.verb = first;
        return command;
    }

    if (first == QLatin1StringView(BarWord)) {
        if (arguments.size() < 2 || arguments.at(1) != QLatin1StringView(ToggleWord)) {
            const QString given = arguments.mid(1).join(QLatin1Char(' '));
            return usageProblem(
                given.isEmpty()
                    ? QStringLiteral("\"bar\" needs a subcommand, and there is one: `bar toggle`")
                    : QStringLiteral("\"bar %1\" is not a command this shell implements; it implements \"bar toggle\"")
                          .arg(given));
        }
        if (const std::optional<QString> problem = tooManyArguments(arguments, 2); problem.has_value())
            return usageProblem(*problem);
        Command command;
        command.request.version = ProtocolVersion;
        command.request.verb = QString::fromLatin1(verb::BarToggle);
        return command;
    }

    if (first == QLatin1StringView(ConfigWord)) {
        if (arguments.size() < 2 || arguments.at(1) != QLatin1StringView(GetWord)) {
            const QString given = arguments.mid(1).join(QLatin1Char(' '));
            return usageProblem(
                given.isEmpty()
                    ? QStringLiteral("\"config\" needs a subcommand, and there is one: `config get <key>`")
                    : QStringLiteral("\"config %1\" is not a command this shell implements; it implements "
                                     "\"config get <key>\"")
                          .arg(given));
        }
        if (arguments.size() < 3) {
            return usageProblem(QStringLiteral(
                "config get needs a key path, as in `qsctl config get bar.height`"));
        }
        if (const std::optional<QString> problem = tooManyArguments(arguments, 3); problem.has_value())
            return usageProblem(*problem);
        Command command;
        command.request.version = ProtocolVersion;
        command.request.verb = QString::fromLatin1(verb::ConfigGet);
        command.request.path = arguments.at(2);
        return command;
    }

    return usageProblem(
        QStringLiteral("\"%1\" is not a command this shell implements").arg(arguments.join(QLatin1Char(' '))));
}

QString usageText()
{
    return QStringLiteral(
        "usage: qsctl <command>\n"
        "\n"
        "  version                the shell's version and the protocol it speaks\n"
        "  state                  the state the bar is drawn from, as one line of JSON\n"
        "  config get <key>       one configuration value, unquoted, for use in a script\n"
        "  bar toggle             hide or show the bar, and report which\n"
        "\n"
        "exit codes: 0 answered, 1 refused, 2 bad command line, 3 no shell listening, 4 protocol mismatch");
}

Outcome interpret(const Request& request, const Response& response)
{
    Outcome outcome;

    // Before the refusal is read: a shell speaking another protocol may have understood a different
    // request, so its message would be about something this client did not ask for.
    if (response.version != ProtocolVersion) {
        outcome.exitCode = int(ExitCode::protocolMismatch);
        outcome.toStandardError = true;
        outcome.text = QStringLiteral("the shell speaks protocol %1 and this qsctl speaks %2, so neither can "
                                      "trust the other's answer")
                           .arg(response.version)
                           .arg(ProtocolVersion);
        return outcome;
    }

    if (!response.ok) {
        outcome.exitCode = int(ExitCode::refused);
        outcome.toStandardError = true;
        outcome.text = response.error;
        return outcome;
    }

    outcome.exitCode = int(ExitCode::ok);
    outcome.toStandardError = false;
    outcome.text = formatSuccess(request, response);
    return outcome;
}

}  // namespace quantum::ipc
