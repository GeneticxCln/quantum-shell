// The shell's logging: one place that decides where a record goes and what it looks like.
//
// Qt already has the mechanism — `qCInfo`/`qCWarning` on a named category, and a message handler that
// picks a sink. What this file adds is the decisions the shell has to make on top of it, and there are
// three.
//
// **The category names.** They are interface twice over: a user filters with
// `QT_LOGGING_RULES="quantum.shell.ipc.debug=false"`, and a bug report quotes them. They are declared
// here once, so a library that logs does not spell a second name that almost matches — a typo in a
// category name is invisible, because a log line with a misspelled category still prints and still looks
// like a log line.
//
// **The pattern.** Qt's default output is the bare message with no time and no level, which is not enough
// to reconstruct what happened. The pattern below is applied unless the process environment already
// carries `QT_MESSAGE_PATTERN`, because that variable is the user's own way to make the same decision and
// overriding it would be the shell taking away a choice.
//
// **Where the records go, and why nothing here chooses it.** Qt sends them to the terminal when stderr is
// one, and to the systemd journal when it is not. A shell started by niri has no terminal, so its records
// go to the journal and are read with `journalctl --user` — which was measured rather than assumed, and
// is why this file installs no file sink: a second destination would be a second thing to keep out of
// sync, a file that grows without bound, and a durable copy of whatever a record happens to contain.
//
// Nothing here logs a secret, and nothing here is a place to put one: the rules in SYSTEM_PROMPT.md §
// Security make PAM conversation content, passwords and fingerprints unloggable, and a logger that
// accepts anything is exactly how that becomes a file on disk.
#pragma once

#include <QLoggingCategory>
#include <QString>
#include <QtLogging>

namespace quantum::app {

// The shell's lifecycle: startup, the configuration file it read, the socket it listens on, shutdown.
Q_DECLARE_LOGGING_CATEGORY(shellLog)

// The compositor connection: which socket was found, when it attached, when it was lost, and the backoff
// between attempts. A compositor restart is the case these records exist for.
Q_DECLARE_LOGGING_CATEGORY(niriLog)

// The configuration: the path that was read, every key that was refused, and every parse failure.
Q_DECLARE_LOGGING_CATEGORY(configLog)

// The local IPC server: what it is listening on, and every request it refused or could not answer.
Q_DECLARE_LOGGING_CATEGORY(ipcLog)

// The layer-shell route: a window that cannot be given a role, and a namespace the integration refuses.
Q_DECLARE_LOGGING_CATEGORY(waylandLog)

// The system readings taken from /proc: every file that could not be read and every line that was refused
// rather than parsed. A reading that stops arriving is otherwise invisible — the widget shows its empty
// state either way — so this category is where the reason for it is.
Q_DECLARE_LOGGING_CATEGORY(systemLog)

// The audio connection: the daemon the shell attached to, the sink it resolved as the default, every
// param it refused to read as a volume, every write it refused to make, and the backoff between attempts
// after a daemon went away. A volume that stops moving is otherwise indistinguishable from a volume that
// did not change, so this category is where the difference is written down.
Q_DECLARE_LOGGING_CATEGORY(audioLog)

// The network connection: whether the bus could be reached at all, the daemon the shell attached to and the
// unique name this process holds on the bus, the daemon leaving and arriving, and every object it refused to
// read. This is the category that answers the two questions a network readout cannot answer for itself — why
// there is no reading, and whether the shell is the thing that is asking — because a bar that shows nothing and
// a bar that shows `offline` look the same from outside the process.
Q_DECLARE_LOGGING_CATEGORY(networkLog)

// The battery connection: the same three questions the network category answers, one daemon over — whether the
// bus could be reached, the name the shell attached to, the daemon leaving and arriving, and every object or
// property it refused to read. It has one question of its own, and it is the one a battery readout cannot
// answer from its own state: a machine with no battery and a device that could not be read both draw nothing,
// so this is where the difference between the two is written down.
Q_DECLARE_LOGGING_CATEGORY(batteryLog)

// The media player connection: which MPRIS players appeared and disappeared, which player is being followed,
// track changes, and playback status updates. This category answers why a media readout shows nothing — no
// players exist, or the active player has no track loaded.
Q_DECLARE_LOGGING_CATEGORY(mediaLog)

namespace Logging {

// The pattern every record is formatted with, when the environment has not named another. Exposed so the
// test can assert what a record looks like rather than describing it: a pattern that has lost its
// timestamp still produces records and would otherwise pass unnoticed.
inline constexpr auto DefaultPattern = "%{time yyyy-MM-dd HH:mm:ss.zzz} [%{type}] %{category}: %{message}";

// The pattern records are formatted with: the environment's when it names one, the shell's otherwise. It is
// a function rather than only a constant because the environment can name one, and a test can then check
// that the shell defers to it instead of overriding it.
QString pattern();

// Applies that pattern to records the default handler prints. Call once, before anything logs.
void install();

// Formats one record the way `install()` makes the shell format it. This is what `qFormatLogMessage`
// does, and it exists here so the format is testable without capturing a process's output.
QString format(QtMsgType type, const QMessageLogContext& context, const QString& message);

}  // namespace Logging
}  // namespace quantum::app
