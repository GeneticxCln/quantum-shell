#include "config/ConfigWatcher.h"

#include "app/Logging.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace quantum::config {
namespace {

// The deepest directory at or above `filePath` that exists. It is not always the file's own directory:
// on a first run `~/.config/quantum-shell` may not exist yet, and the only thing there is to watch is
// `~/.config` itself — watching nothing would mean the file the user is about to create is never
// noticed. Watching the parent is also quieter once the config directory does exist, which is why the
// walk stops at the first directory it finds rather than climbing further.
QString deepestExistingDirectory(const QString& filePath) {
    QDir directory = QFileInfo(filePath).absoluteDir();
    while (!directory.exists()) {
        const QString before = directory.absolutePath();
        if (!directory.cdUp() || directory.absolutePath() == before)
            return {};
    }
    return directory.absolutePath();
}

// Reads and validates one file. Runs on the worker thread, so it takes the path by value and touches no
// member of the watcher: nothing here can outlive the object that asked for it.
ParseResult readAndParse(const QString& path) {
    QFile file(path);

    // A missing file is not an error and not a warning. Defaults ship embedded precisely so that a
    // machine where nobody has written a config is a working shell, and warning about it on every start
    // would train a user to ignore the warnings that matter.
    if (!file.exists())
        return {};

    if (!file.open(QIODevice::ReadOnly)) {
        ParseResult result;
        result.errors.append(QStringLiteral("could not be opened: %1").arg(file.errorString()));
        return result;
    }

    return parseConfig(file.readAll());
}

}  // namespace

ConfigWatcher::ConfigWatcher(Config& config, const QString& path, QObject* parent)
    : QObject(parent), config_(config), path_(path) {}

QString ConfigWatcher::defaultPath() {
    // `GenericConfigLocation` is `$XDG_CONFIG_HOME` or `~/.config`, asked of Qt rather than assembled
    // from the environment by hand. It is deliberately not `AppConfigLocation`: that path depends on the
    // application name having been set, and a config file whose location depends on the order of calls in
    // `main` is a file that moves when someone reorders them.
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("quantum-shell/config.toml"));
}

void ConfigWatcher::start() {
    // Synchronous, and the only read that is: this runs before the QML engine loads, so the bar is built
    // with the values in the file instead of being built with the defaults and resized a moment later.
    apply(readAndParse(path_));

    rewatch();
    // Both signals, because either can be the one that fires: an in-place write changes the file, an
    // atomic replace changes the directory. They lead to the same place.
    QObject::connect(&watcher_, &QFileSystemWatcher::fileChanged, this,
                     &ConfigWatcher::onWatchedPathChanged);
    QObject::connect(&watcher_, &QFileSystemWatcher::directoryChanged, this,
                     &ConfigWatcher::onWatchedPathChanged);
    QObject::connect(&read_, &QFutureWatcher<ParseResult>::finished, this,
                     &ConfigWatcher::readFinished);
}

void ConfigWatcher::onWatchedPathChanged() {
    // Before reading, re-derive what to watch: the event that just arrived may be the config directory or
    // the config file being created for the first time, and a watcher still pointed at the old path would
    // never hear about the next change to the new one.
    rewatch();
    scheduleRead();
}

void ConfigWatcher::scheduleRead() {
    if (reading_) {
        readAgain_ = true;
        return;
    }
    reading_ = true;
    // The lambda captures the path by value, not `this`: the read outlives nothing, but a lambda that
    // reached into the watcher would be a lambda whose lifetime is the worker thread's, not this
    // object's.
    read_.setFuture(QtConcurrent::run([path = path_] { return readAndParse(path); }));
}

void ConfigWatcher::readFinished() {
    const ParseResult result = read_.result();
    reading_ = false;
    apply(result);

    if (readAgain_) {
        readAgain_ = false;
        scheduleRead();
    }
}

void ConfigWatcher::apply(const ParseResult& result) {
    const QStringList found = result.errors.isEmpty() ? result.warnings : result.errors;

    // The schema's messages are about a file's contents and say nothing about where that file is, so the
    // path is added once, here, to the one list that is both logged and reported. A listener and a user
    // therefore read the same sentences, which is the point: a bug report quoting this signal is a bug
    // report quoting the log.
    QStringList messages;
    messages.reserve(found.size());
    for (const QString& message : found)
        messages.append(QStringLiteral("%1: %2").arg(path_, message));

    // The category carries the "this is the configuration" half that the text used to spell, and it does it
    // the way the rest of the shell's records do: `quantum.shell.config` is what a person filters on.
    for (const QString& message : messages)
        qCWarning(quantum::app::configLog, "%s", qUtf8Printable(message));

    // An unusable file is not applied. The values already in `Config` came from the last file that
    // worked, or from the schema's defaults, and both are better than a half-read file's.
    if (result.errors.isEmpty())
        config_.apply(result.values);

    // Emitted even when it is empty: "this read found nothing to report" is an answer, and a listener
    // should not have to infer it from the absence of a signal.
    Q_EMIT diagnosed(messages);
}

void ConfigWatcher::rewatch() {
    QStringList wanted;
    if (QFileInfo::exists(path_))
        wanted.append(path_);
    const QString directory = deepestExistingDirectory(path_);
    if (!directory.isEmpty())
        wanted.append(directory);

    QStringList current = watcher_.files();
    current.sort();
    wanted.sort();
    // Only when the set really changed. Removing and re-adding the same directory on every event would
    // make the watcher's own bookkeeping produce the next event.
    if (current == wanted)
        return;

    if (!current.isEmpty())
        watcher_.removePaths(current);
    for (const QString& path : wanted)
        watcher_.addPath(path);
}

}  // namespace quantum::config
