// The launcher as QML sees it: the applications this machine offers, a query over them, and the act of
// starting one.
//
// **What it reads.** The freedesktop.org `applications/` directories — `$XDG_DATA_HOME` first, then each of
// `$XDG_DATA_DIRS` — in that order, so a user's copy of an entry shadows the system's, and a user's copy
// marked `Hidden` removes it (the specification's way of deleting an entry). The parsing is
// `DesktopEntry.h`'s; this class decides which directories, in what order, and what a scan yields.
//
// **When it reads.** Once at construction and again each time the launcher is opened, on a worker thread:
// nothing blocks the GUI thread, and the list a person sees is the one from the most recent scan that has
// finished. This is not a poll — there is no timer and nothing re-reads on a schedule; opening is the event
// that makes the answer worth having, and watching every data directory for changes would be a second
// mechanism to keep in step with a fact that is only ever looked at when the panel opens.
//
// **What starting one means.** The entry's `Exec`, split by `splitExec` into an argument vector and handed to
// `QProcess::startDetached` — no shell, so nothing in a file name or a query is ever interpreted as one — with
// the home directory as the working directory. The child is detached: it outlives the shell and is not the
// shell's to wait on. A program that cannot be started is a record on `quantum.shell.launcher` and the
// launcher stays open, because closing on a failure would say the application started.
//
// Terminal applications are not offered (`isOffered` says why), and an entry whose `TryExec` is not
// installed is dropped, as the specification says.
//
// What QML gets:
//
//   open           whether the launcher is showing; the host follows it and writes it back when the surface goes
//   query          the text being searched for; writing it re-ranks
//   results        the offered applications matching `query`, best first, at most `maxResults`, each a map
//                  of `id`, `name` and `comment`
//   selectedIndex  which result Enter starts; 0 whenever the results change, -1 when there are none
//   maxResults     how many results are kept, `[launcher] max_results`
//   applicationCount  how many applications the last finished scan offered, before the query
//   scanning       whether a scan is in flight
#pragma once

#include "apps/DesktopEntry.h"

#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

QT_BEGIN_NAMESPACE
class QJSEngine;
class QQmlEngine;
QT_END_NAMESPACE

namespace quantum::apps {

class LauncherService : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool open READ isOpen WRITE setOpen NOTIFY openChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectedIndexChanged)
    Q_PROPERTY(int maxResults READ maxResults NOTIFY maxResultsChanged)
    Q_PROPERTY(int applicationCount READ applicationCount NOTIFY applicationCountChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)

public:
    // The default for `[launcher] max_results`, and the floor and ceiling the schema also holds. `config-test`
    // compares the schema's copies with these at compile time, the way the sampling cadence is.
    inline static constexpr int DefaultMaxResults = 8;
    inline static constexpr int MinMaxResults = 1;
    inline static constexpr int MaxMaxResults = 50;

    // `dataDirs` in priority order, `currentDesktops` the split `XDG_CURRENT_DESKTOP`, `locale` a POSIX locale
    // name. Nothing is read until `refresh()`; the composition root passes the environment's values and a
    // test passes a directory of its own.
    explicit LauncherService(const QStringList& dataDirs, const QStringList& currentDesktops,
                             const QString& locale, QObject* parent = nullptr);
    ~LauncherService() override;

    // `$XDG_DATA_HOME` (default `~/.local/share`) followed by `$XDG_DATA_DIRS` (default
    // `/usr/local/share:/usr/share`), as the XDG Base Directory Specification defines them. Each returned path
    // is a base directory; `applications/` is appended by the scan.
    static QStringList defaultDataDirs();

    // The scan itself, a function of the directories: every offered entry, shadowing resolved, in no
    // particular order. Public and static so a test drives it on a directory it wrote without a service, and
    // so the worker thread runs a function that touches no member.
    static QList<DesktopEntry> scan(const QStringList& dataDirs, const QStringList& currentDesktops,
                                    const QString& locale, int* refused = nullptr);

    // Ranks `entries` against `query`: best first, ties by name, nothing that does not match, at most `limit`.
    static QList<DesktopEntry> rank(const QList<DesktopEntry>& entries, const QString& query, int limit);

    bool isOpen() const { return open_; }
    QString query() const { return query_; }
    QVariantList results() const;
    int selectedIndex() const { return selected_; }
    int maxResults() const { return maxResults_; }
    int applicationCount() const { return static_cast<int>(entries_.size()); }
    bool scanning() const { return scanning_; }

    void setOpen(bool open);
    void setQuery(const QString& query);

    // A value outside `MinMaxResults`..`MaxMaxResults` is refused with a record, for the reason every other
    // service refuses what the schema would: two places with an opinion agree or one is wrong.
    void setMaxResults(int maxResults);

    // The two acts a person performs on the panel. `launchSelected` starts the highlighted entry and returns
    // whether a process was started; `launch(index)` starts a given result. Both close the panel on success.
    Q_INVOKABLE bool toggle();
    Q_INVOKABLE void moveSelection(int delta);
    Q_INVOKABLE bool launchSelected();
    Q_INVOKABLE bool launch(int index);

    // Re-reads the directories on a worker thread. Called when the panel opens; public because the composition
    // root asks for the first scan and a test asks for one after writing files.
    void refresh();

    static void registerQmlSingleton(LauncherService& service);
    inline static constexpr auto QmlTypeName = "LauncherService";

    // The results as entries, for the host and for tests: the same list `results()` renders as maps.
    const QList<DesktopEntry>& shown() const { return shown_; }

Q_SIGNALS:
    void openChanged();
    void queryChanged();
    void resultsChanged();
    void selectedIndexChanged();
    void maxResultsChanged();
    void applicationCountChanged();
    void scanningChanged();

private:
    void applyScan();
    void rerank();

    QStringList dataDirs_;
    QStringList currentDesktops_;
    QString locale_;

    QList<DesktopEntry> entries_;
    QList<DesktopEntry> shown_;
    QString query_;
    int selected_ = -1;
    int maxResults_ = DefaultMaxResults;
    bool open_ = false;
    bool scanning_ = false;
    bool rescanQueued_ = false;

    QFutureWatcher<QList<DesktopEntry>>* watcher_ = nullptr;
};

}  // namespace quantum::apps
