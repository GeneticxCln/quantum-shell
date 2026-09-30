#include "apps/LauncherService.h"

#include "QmlModule.h"
#include "app/Logging.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QDateTime>
#include <QFileInfo>
#include <QProcess>
#include <QQmlEngine>
#include <QSet>
#include <QStandardPaths>
#include <QtConcurrent>

#include <algorithm>

namespace quantum::apps {

namespace {

bool tryExecExists(const QString& tryExec)
{
    if (tryExec.isEmpty())
        return true;
    // An absolute path is checked as itself and a bare name is looked up in PATH: the two forms the
    // specification gives for `TryExec`.
    if (QDir::isAbsolutePath(tryExec)) {
        const QFileInfo info(tryExec);
        return info.exists() && info.isExecutable();
    }
    return !QStandardPaths::findExecutable(tryExec).isEmpty();
}

}  // namespace

LauncherService::LauncherService(const QStringList& dataDirs, const QStringList& currentDesktops,
                                 const QString& locale, QObject* parent)
    : QObject(parent)
    , dataDirs_(dataDirs)
    , currentDesktops_(currentDesktops)
    , locale_(locale)
{
    // One writer thread, so two launches in quick succession are written in the order they happened.
    writer_.setMaxThreadCount(1);
    watcher_ = new QFutureWatcher<Scan>(this);
    connect(watcher_, &QFutureWatcher<Scan>::finished, this, &LauncherService::applyScan);
}

LauncherService::~LauncherService()
{
    // A scan still running holds copies of the directories and nothing of ours; waiting for it is what keeps
    // the watcher from delivering to an object that is gone.
    if (watcher_ != nullptr)
        watcher_->waitForFinished();
    writer_.waitForDone();
}

void LauncherService::setHistoryPath(const QString& path)
{
    historyPath_ = path;
}

QString LauncherService::defaultHistoryPath()
{
    const QString state = qEnvironmentVariable("XDG_STATE_HOME");
    const QString base = state.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/state") : state;
    return base + QStringLiteral("/quantum-shell/launcher-history.json");
}

QStringList LauncherService::defaultDataDirs()
{
    QStringList dirs;
    const QString home = qEnvironmentVariable("XDG_DATA_HOME");
    dirs.append(home.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share") : home);
    const QString system = qEnvironmentVariable("XDG_DATA_DIRS");
    const QStringList configured = system.isEmpty()
        ? QStringList{QStringLiteral("/usr/local/share"), QStringLiteral("/usr/share")}
        : system.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    dirs.append(configured);
    return dirs;
}

QList<DesktopEntry> LauncherService::scan(const QStringList& dataDirs, const QStringList& currentDesktops,
                                          const QString& locale, int* refused)
{
    QList<DesktopEntry> offered;
    // Every ID seen, offered or not: the first directory that has an ID owns it, so a user's `Hidden=true`
    // copy removes the system's entry rather than letting it through.
    QSet<QString> seen;
    int skipped = 0;

    for (const QString& base : dataDirs) {
        const QString root = QDir(base).filePath(QStringLiteral("applications"));
        if (!QFileInfo(root).isDir())
            continue;
        QDirIterator it(root, {QStringLiteral("*.desktop")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QString id = QDir(root).relativeFilePath(path);
            id.replace(QLatin1Char('/'), QLatin1Char('-'));
            if (seen.contains(id))
                continue;
            seen.insert(id);

            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                qCWarning(quantum::app::launcherLog) << "cannot read" << path << ":" << file.errorString();
                ++skipped;
                continue;
            }
            std::optional<DesktopEntry> entry = parseDesktopEntry(QString::fromUtf8(file.readAll()), locale);
            if (!entry.has_value()) {
                ++skipped;
                continue;
            }
            entry->id = id;
            if (!isOffered(*entry, currentDesktops))
                continue;
            if (!tryExecExists(entry->tryExec))
                continue;
            offered.append(*entry);
        }
    }
    if (refused != nullptr)
        *refused = skipped;
    return offered;
}

QList<DesktopEntry> LauncherService::rank(const QList<DesktopEntry>& entries, const QString& query, int limit,
                                          const LaunchHistory& history, qint64 nowMs)
{
    struct Scored
    {
        int score;
        qint64 frecency;
        const DesktopEntry* entry;
    };
    QList<Scored> scored;
    for (const DesktopEntry& entry : entries) {
        int score = matchScore(query, entry.name);
        // The program's own name is a weaker claim than the entry's name: `firefox` finds "Web Browser" only
        // when the name did not match, and never outranks a name that did.
        const QString program = QFileInfo(entry.arguments.first()).fileName();
        const int viaProgram = matchScore(query, program);
        if (viaProgram >= 0)
            score = std::max(score, viaProgram - 50);
        if (score < 0 && !query.isEmpty()) {
            const int viaComment = matchScore(query, entry.comment);
            if (viaComment >= 0 && viaComment >= 100)
                score = viaComment - 90;
        }
        if (score >= 0 || query.isEmpty())
            scored.append({std::max(score, 0), frecency(history.value(entry.id), nowMs), &entry});
    }
    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.frecency != b.frecency)
            return a.frecency > b.frecency;
        const int byName = a.entry->name.compare(b.entry->name, Qt::CaseInsensitive);
        return byName != 0 ? byName < 0 : a.entry->id < b.entry->id;
    });
    QList<DesktopEntry> out;
    for (qsizetype i = 0; i < scored.size() && i < limit; ++i)
        out.append(*scored.at(i).entry);
    return out;
}

QVariantList LauncherService::results() const
{
    QVariantList list;
    list.reserve(shown_.size());
    for (const DesktopEntry& entry : shown_) {
        list.append(QVariantMap{{QStringLiteral("id"), entry.id},
                                {QStringLiteral("name"), entry.name},
                                {QStringLiteral("comment"), entry.comment}});
    }
    return list;
}

void LauncherService::refresh()
{
    if (scanning_) {
        // One scan at a time; a second request while one runs is a scan to do after it, because the files may
        // have changed since the first one started reading them.
        rescanQueued_ = true;
        return;
    }
    scanning_ = true;
    emit scanningChanged();
    const QStringList dirs = dataDirs_;
    const QStringList desktops = currentDesktops_;
    const QString locale = locale_;
    const QString historyPath = historyPath_;
    watcher_->setFuture(QtConcurrent::run([dirs, desktops, locale, historyPath] {
        int refused = 0;
        Scan result;
        result.entries = scan(dirs, desktops, locale, &refused);
        qCInfo(quantum::app::launcherLog) << "scanned" << dirs.size() << "data directories:"
                                         << result.entries.size() << "applications offered," << refused
                                         << "entries refused";
        if (!historyPath.isEmpty()) {
            QString error;
            result.history = readHistoryFile(historyPath, &error);
            if (!error.isEmpty())
                qCWarning(quantum::app::launcherLog)
                    << "launch history" << historyPath << "could not be read:" << error << "; starting empty";
        }
        return result;
    }));
}

void LauncherService::applyScan()
{
    const Scan scanned = watcher_->result();
    entries_ = scanned.entries;
    // A launch made while this scan ran is already in `history_` and not yet in what the worker read; merging by
    // the larger count keeps it.
    for (auto it = scanned.history.cbegin(); it != scanned.history.cend(); ++it) {
        LaunchRecord& mine = history_[it.key()];
        if (it.value().count > mine.count)
            mine = it.value();
    }
    scanning_ = false;
    emit scanningChanged();
    emit applicationCountChanged();
    rerank();
    if (rescanQueued_) {
        rescanQueued_ = false;
        refresh();
    }
}

void LauncherService::rerank()
{
    QList<DesktopEntry> next = rank(entries_, query_, maxResults_, history_, QDateTime::currentMSecsSinceEpoch());
    const bool changed = next != shown_;
    shown_ = std::move(next);
    const int selected = shown_.isEmpty() ? -1 : 0;
    if (changed)
        emit resultsChanged();
    if (selected != selected_) {
        selected_ = selected;
        emit selectedIndexChanged();
    }
}

void LauncherService::setOpen(bool open)
{
    if (open_ == open)
        return;
    open_ = open;
    if (open) {
        // A fresh panel starts with an empty query: what was typed last time is not what is wanted now.
        if (!query_.isEmpty()) {
            query_.clear();
            emit queryChanged();
            rerank();
        }
        refresh();
    }
    emit openChanged();
}

bool LauncherService::toggle()
{
    setOpen(!open_);
    return open_;
}

void LauncherService::setQuery(const QString& query)
{
    if (query_ == query)
        return;
    query_ = query;
    emit queryChanged();
    rerank();
}

void LauncherService::setMaxResults(int maxResults)
{
    if (maxResults < MinMaxResults || maxResults > MaxMaxResults) {
        qCWarning(quantum::app::launcherLog)
            << "max_results" << maxResults << "is outside" << MinMaxResults << "to" << MaxMaxResults
            << "; keeping" << maxResults_;
        return;
    }
    if (maxResults_ == maxResults)
        return;
    maxResults_ = maxResults;
    emit maxResultsChanged();
    rerank();
}

void LauncherService::moveSelection(int delta)
{
    if (shown_.isEmpty())
        return;
    // Wraps: past the last result is the first, so a held arrow key never dead-ends.
    const int count = static_cast<int>(shown_.size());
    const int next = ((selected_ + delta) % count + count) % count;
    if (next == selected_)
        return;
    selected_ = next;
    emit selectedIndexChanged();
}

bool LauncherService::launchSelected()
{
    return launch(selected_);
}

bool LauncherService::launch(int index)
{
    if (index < 0 || index >= shown_.size())
        return false;
    const DesktopEntry& entry = shown_.at(index);
    const QString program = entry.arguments.first();
    const QStringList arguments = entry.arguments.mid(1);
    if (!QProcess::startDetached(program, arguments, QDir::homePath())) {
        qCWarning(quantum::app::launcherLog) << "could not start" << program << "for" << entry.id;
        return false;
    }
    qCInfo(quantum::app::launcherLog) << "started" << program << "for" << entry.id;
    if (!historyPath_.isEmpty()) {
        recordLaunch(history_, entry.id, QDateTime::currentMSecsSinceEpoch());
        const QString path = historyPath_;
        const LaunchHistory snapshot = history_;
        writer_.start([path, snapshot] {
            QString error;
            if (!writeHistoryFile(path, snapshot, &error))
                qCWarning(quantum::app::launcherLog)
                    << "launch history" << path << "could not be written:" << error;
        });
    }
    setOpen(false);
    return true;
}

void LauncherService::registerQmlSingleton(LauncherService& service)
{
    qmlRegisterSingletonInstance(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion,
                                 quantum::qml::ModuleMinorVersion, QmlTypeName, &service);
}

}  // namespace quantum::apps
