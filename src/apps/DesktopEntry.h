// The pure half of the launcher: what a `.desktop` file says, what an `Exec` line means as an argument
// vector, and how a query ranks a name.
//
// Every function here is a function of its arguments — text in, value out — so the shapes a real desktop's
// files come in, and the ones it should never contain, are cases in `apps-test` and no file system is needed
// to state them. The scan over the directories and the process a launch starts are `AppCatalog`'s, one file
// over.
//
// The format is the freedesktop.org Desktop Entry Specification (1.5): a `[Desktop Entry]` group of `Key=Value`
// lines, `Name[locale]` keys, the string escapes `\s \n \t \r \\`, and an `Exec` value that is a command line
// with its own quoting and `%` field codes. That is the document the names below were taken from; where this
// file makes a choice the specification leaves open, the choice is written at the place it is made.
#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace quantum::apps {

// One application a person can start. Only what a launcher draws or acts on: a key that nothing reads is not
// carried.
struct DesktopEntry {
    // The desktop file ID: the path under `applications/` with `/` replaced by `-`, which is what the
    // specification says identifies an entry and what makes a user's copy shadow the system's. Set by the
    // scan; the parser does not know where the text came from.
    QString id;
    QString name;
    QString comment;
    // The command and its arguments, already split and with every field code resolved. The program is
    // `arguments.first()`. No shell is involved anywhere: this is what is handed to `execve`.
    QStringList arguments;
    // `TryExec`: a program that has to exist for the entry to be offered. Kept as written; whether it exists
    // is a fact about the machine and so the scan's to check.
    QString tryExec;
    bool hidden = false;
    bool terminal = false;
    QStringList onlyShowIn;
    QStringList notShowIn;

    bool operator==(const DesktopEntry&) const = default;
};

// Parses one file's text. Returns nothing for a file that is not an application entry this shell can offer:
// no `[Desktop Entry]` group, a `Type` other than `Application`, no `Name`, or no `Exec` that resolves to a
// program. `NoDisplay=true` and `Hidden=true` are not refusals — they are entries, marked hidden, because a
// caller that wants to know why an entry was dropped can ask. `locale` is the POSIX locale name
// (`de_DE`, `sr@latin`): `Name[de_DE]` is preferred over `Name[de]` over `Name`, the order the specification
// gives for matching.
std::optional<DesktopEntry> parseDesktopEntry(const QString& text, const QString& locale);

// Whether an entry is offered to a person on a desktop named by `currentDesktops` (the colon-separated
// `XDG_CURRENT_DESKTOP`, already split). Offered means: not hidden, not a terminal application, allowed by
// `OnlyShowIn` and not excluded by `NotShowIn`.
//
// Terminal applications are not offered, and that is a decision rather than a gap: starting one means
// choosing a terminal emulator, and there is no verified, installed convention for that on this desktop to
// choose by. Offering an entry that cannot be started is the dead control the project's rules ban, and
// guessing `xterm` is the canned answer.
bool isOffered(const DesktopEntry& entry, const QStringList& currentDesktops);

// Splits an `Exec` value into arguments per the specification: unquoted whitespace separates arguments, a
// double-quoted argument keeps its whitespace and may contain the escapes `\"`, `` \` ``, `\$` and `\\`, and the
// field codes are resolved — `%%` is a literal `%`, `%c` is `name`, and every other code (`%f %F %u %U %d
// %D %n %N %v %m %i %k`) names something a launcher started without a file, URL or icon has nothing to put
// there, so it is removed. An argument that was nothing but a removed code disappears rather than being
// passed empty. An unterminated quote returns nothing: the specification says such a value is invalid.
std::optional<QStringList> splitExec(const QString& exec, const QString& name);

// How well `query` matches `text`, or -1 for no match. Case-insensitive. Ranked, best first: the text starts
// with the query (300), a word of the text starts with it (200), the text contains it (100), and the
// query's characters occur in the text in order (10 up to a 50-point bonus for tighter runs). An empty query
// matches everything at 0, which is the state a launcher opens in.
int matchScore(const QString& query, const QString& text);

}  // namespace quantum::apps
