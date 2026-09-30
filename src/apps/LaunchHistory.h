// What the launcher remembers about what was started: for each desktop file ID, how many times and when
// last. The pure half of frecency — a function of a record and a moment, and of a JSON text — so every
// shape of file and every age is a case in `apps-test` without a clock or a home directory.
//
// The file is the launcher's own state, not the user's configuration: it is written by the shell after a
// launch and never by a person, so it carries no `schema_version` and no key of it is a config key. A file
// that cannot be read as this format is refused whole with the reason and the history starts empty; the next
// launch then writes a valid one over it.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

#include <optional>

namespace quantum::apps {

struct LaunchRecord {
    int count = 0;
    // Milliseconds since the epoch of the most recent launch.
    qint64 lastMs = 0;

    bool operator==(const LaunchRecord&) const = default;
};

using LaunchHistory = QHash<QString, LaunchRecord>;

// Records one launch of `id` at `nowMs`.
void recordLaunch(LaunchHistory& history, const QString& id, qint64 nowMs);

// How much a record should lift an entry among equally good matches: the count, weighted by how recent the
// last launch was — 100 within four days, 70 within two weeks, 50 within a month, 30 within three months and
// 10 after that (the buckets Firefox's frecency uses), so something launched often long ago yields to
// something launched a few times this week. A record with no launches, or one dated after `nowMs` (a clock set
// back), is weighed as the newest bucket rather than as a negative age.
qint64 frecency(const LaunchRecord& record, qint64 nowMs);

// The file's text: a JSON object whose keys are desktop file IDs and whose values are `{"count", "last"}`.
QByteArray serializeHistory(const LaunchHistory& history);

// Parses the file's text. Returns nothing, with `error` naming why, for text that is not that object; an entry
// whose count is not a positive integer or whose time is not an integer is dropped rather than failing the
// file, because one bad entry is not a reason to forget the rest.
std::optional<LaunchHistory> parseHistory(const QByteArray& text, QString* error = nullptr);

// Reads `path`. A file that does not exist is an empty history and not an error — nothing has been launched
// yet — while one that exists and cannot be parsed is an empty history *and* `error` is set.
LaunchHistory readHistoryFile(const QString& path, QString* error = nullptr);

// Writes `history` to `path` atomically (a `QSaveFile`, so a reader sees the old file or the new one), creating
// its directory. Returns whether it was written; `error` says why not.
bool writeHistoryFile(const QString& path, const LaunchHistory& history, QString* error = nullptr);

}  // namespace quantum::apps
