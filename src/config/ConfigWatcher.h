// The config file, kept in step with the file on disk.
//
// The sequence is the one QUANTUM_SHELL.md § Configuration draws:
//
//     config.toml changed
//            ↓
//     ConfigWatcher (file + parent directory, to survive atomic replaces)
//            ↓
//     parse + validate on a worker thread
//            ↓
//     diff against the previous tree
//            ↓
//     Config.bar.heightChanged()
//            ↓
//     QML updates automatically
//
// Two things in that sequence are worth stating because they are decisions rather than mechanics.
//
// The **parent directory is watched as well as the file**, and the watch set is re-derived after every
// event. Most editors and `cat >` style writes do not modify the file in place: they write a new file and
// rename it over the old one, which leaves the original inode gone and the watch on it dead. Watching the
// directory is what turns that into a visible event, and re-deriving is what notices the file — or the
// whole `quantum-shell` directory — appearing for the first time.
//
// The **first read is synchronous and every later one is not**. `start()` runs before the QML engine
// loads, so the bar is built with the values in the file rather than assembled with the defaults and
// corrected a moment later; there is no frame to block yet. After that the watcher is reacting to a file
// changing underneath a shell that is drawing, so the parse goes to the worker thread and only the diff
// and the signals come back to this one.
//
// Applying is all-or-nothing per file. A file that cannot be used at all — it is not TOML, or it was
// written for a schema version this build does not know — leaves the previous configuration standing and
// says so, because a typo in an unrelated key must not reset a working bar to the defaults.
#pragma once

#include "config/Config.h"
#include "config/ConfigSchema.h"

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>

namespace quantum::config {

class ConfigWatcher : public QObject {
    Q_OBJECT

public:
    // `config` must outlive this object. `path` defaults to the file the shell ships with:
    // `$XDG_CONFIG_HOME/quantum-shell/config.toml`, or `~/.config/quantum-shell/config.toml`. It is
    // injectable because that is the whole of what a test needs to run against a file of its own instead
    // of against the user's.
    explicit ConfigWatcher(Config& config, const QString& path = defaultPath(),
                           QObject* parent = nullptr);

    // The path of the file this shell reads.
    static QString defaultPath();

    // Reads the file once and begins watching it. Calling it with no file present is normal and not an
    // error: the schema's defaults stand until one appears.
    void start();

Q_SIGNALS:
    // What the last read had to say, in the order it had to say it: the warnings reported while a usable
    // file was applied, or — when nothing could be applied — the errors that stopped it. Empty after a
    // clean read. These are the same lines that go to the log, so a listener and a user see one account
    // of the file rather than two.
    void diagnosed(const QStringList& messages);

private:
    void onWatchedPathChanged();
    // Starts a read, or records that one is wanted if one is already running: an editor that writes a
    // file in several steps produces several events, and the last state of the file is the one that
    // matters, not each intermediate one.
    void scheduleRead();
    void readFinished();
    void apply(const ParseResult& result);
    // Points the watcher at the file and at the deepest directory above it that exists today.
    void rewatch();

    Config& config_;
    QString path_;
    QFileSystemWatcher watcher_;
    QFutureWatcher<ParseResult> read_;
    bool reading_ = false;
    bool readAgain_ = false;
};

}  // namespace quantum::config
