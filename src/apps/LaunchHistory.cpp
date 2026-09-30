#include "apps/LaunchHistory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace quantum::apps {

namespace {

constexpr qint64 DayMs = 24LL * 60 * 60 * 1000;

void setError(QString* error, const QString& text)
{
    if (error != nullptr)
        *error = text;
}

}  // namespace

void recordLaunch(LaunchHistory& history, const QString& id, qint64 nowMs)
{
    LaunchRecord& record = history[id];
    record.count += 1;
    record.lastMs = nowMs;
}

qint64 frecency(const LaunchRecord& record, qint64 nowMs)
{
    if (record.count <= 0)
        return 0;
    const qint64 age = std::max<qint64>(nowMs - record.lastMs, 0);
    qint64 weight = 10;
    if (age < 4 * DayMs)
        weight = 100;
    else if (age < 14 * DayMs)
        weight = 70;
    else if (age < 31 * DayMs)
        weight = 50;
    else if (age < 90 * DayMs)
        weight = 30;
    return static_cast<qint64>(record.count) * weight;
}

QByteArray serializeHistory(const LaunchHistory& history)
{
    QJsonObject root;
    for (auto it = history.cbegin(); it != history.cend(); ++it) {
        root.insert(it.key(), QJsonObject{{QStringLiteral("count"), it.value().count},
                                          {QStringLiteral("last"), it.value().lastMs}});
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

std::optional<LaunchHistory> parseHistory(const QByteArray& text, QString* error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(text, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        setError(error, parseError.errorString());
        return std::nullopt;
    }
    if (!doc.isObject()) {
        setError(error, QStringLiteral("the top level is not an object"));
        return std::nullopt;
    }
    LaunchHistory history;
    const QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject value = it.value().toObject();
        const QJsonValue count = value.value(QStringLiteral("count"));
        const QJsonValue last = value.value(QStringLiteral("last"));
        // `isDouble` is true for every JSON number; the integer test is that it survives the round trip.
        if (!count.isDouble() || !last.isDouble())
            continue;
        const double countValue = count.toDouble();
        const double lastValue = last.toDouble();
        if (countValue < 1 || countValue > 1e9 || countValue != static_cast<double>(static_cast<int>(countValue)))
            continue;
        if (lastValue != static_cast<double>(static_cast<qint64>(lastValue)))
            continue;
        history.insert(it.key(), LaunchRecord{static_cast<int>(countValue), static_cast<qint64>(lastValue)});
    }
    return history;
}

LaunchHistory readHistoryFile(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return {};
    }
    QString parseError;
    const std::optional<LaunchHistory> parsed = parseHistory(file.readAll(), &parseError);
    if (!parsed.has_value()) {
        setError(error, parseError);
        return {};
    }
    return *parsed;
}

bool writeHistoryFile(const QString& path, const LaunchHistory& history, QString* error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        setError(error, QStringLiteral("could not create the directory"));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    file.write(serializeHistory(history));
    if (!file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    return true;
}

}  // namespace quantum::apps
