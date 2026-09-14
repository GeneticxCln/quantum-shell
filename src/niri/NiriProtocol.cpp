#include "niri/NiriProtocol.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

#include <algorithm>

namespace quantum::niri {
namespace {

// The variants of niri-ipc v26.04's `Event` enum, in the order the crate documents them. Kept here so
// a name that arrives from the compositor can be told apart from a name this build simply does not
// model yet.
constexpr std::array<std::string_view, 19> eventNames{{
    "WorkspacesChanged",
    "WorkspaceUrgencyChanged",
    "WorkspaceActivated",
    "WorkspaceActiveWindowChanged",
    "WindowsChanged",
    "WindowOpenedOrChanged",
    "WindowClosed",
    "WindowFocusChanged",
    "WindowFocusTimestampChanged",
    "WindowUrgencyChanged",
    "WindowLayoutsChanged",
    "KeyboardLayoutsChanged",
    "KeyboardLayoutSwitched",
    "OverviewOpenedOrClosed",
    "ConfigLoaded",
    "ScreenshotCaptured",
    "CastsChanged",
    "CastStartedOrChanged",
    "CastStopped",
}};

}  // namespace

QString niriSocketPath() {
    return qEnvironmentVariable("NIRI_SOCKET");
}

QString niriSocketDir() {
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return runtime.isEmpty() ? QDir::tempPath() : runtime;
}

namespace {

// The Wayland socket name inside a niri IPC socket file name, or empty for a name niri would not have
// written: `niri.<wayland socket name>.<pid>.sock`.
QString waylandNameInSocketName(const QString& fileName) {
    static const QRegularExpression pattern(QStringLiteral("^niri\\.(.+)\\.([0-9]+)\\.sock$"));
    const QRegularExpressionMatch match = pattern.match(fileName);
    return match.hasMatch() ? match.captured(1) : QString();
}

}  // namespace

QStringList niriSocketFiles(const QString& directory) {
    const QDir dir(directory.isEmpty() ? niriSocketDir() : directory);
    // A Unix socket is not a regular file, so `QDir::Files` alone would hide exactly the entries this
    // looks for; `QDir::System` is what includes them.
    const QFileInfoList entries =
        dir.entryInfoList(QStringList{QStringLiteral("niri.*.sock")}, QDir::System | QDir::NoDotAndDotDot);

    QStringList files;
    for (const QFileInfo& entry : entries) {
        if (waylandNameInSocketName(entry.fileName()).isEmpty()) {
            continue;
        }
        files.append(entry.absoluteFilePath());
    }
    // Newest first: after a restart the socket that belongs to the running compositor is the one that
    // was created last, while a compositor that crashed leaves its own file behind.
    std::sort(files.begin(), files.end(), [](const QString& left, const QString& right) {
        return QFileInfo(left).lastModified() > QFileInfo(right).lastModified();
    });
    return files;
}

QStringList niriSocketFor(const QString& waylandName, const QString& directory) {
    if (waylandName.isEmpty()) {
        return {};
    }
    QStringList matches;
    for (const QString& file : niriSocketFiles(directory)) {
        if (waylandNameInSocketName(QFileInfo(file).fileName()) == waylandName) {
            matches.append(file);
        }
    }
    return matches;
}

QString compositorSocket() {
    const QString named = niriSocketPath();
    if (!named.isEmpty() && QFileInfo::exists(named)) {
        return named;
    }
    const QStringList rediscovered = niriSocketFor(qEnvironmentVariable("WAYLAND_DISPLAY"));
    return rediscovered.isEmpty() ? QString() : rediscovered.first();
}

bool Reply::isOk() const {
    return kind == Kind::ok;
}

QJsonValue Reply::variant(QStringView expectedVariant) const {
    if (kind != Kind::ok) {
        return QJsonValue{};
    }
    return value.toObject().value(expectedVariant.toString());
}

QString Reply::describe() const {
    if (kind == Kind::error) {
        return QStringLiteral("error: %1").arg(text);
    }
    if (kind == Kind::transportError) {
        return QStringLiteral("transport error: %1").arg(text);
    }
    if (value.isObject()) {
        return QStringLiteral("ok: %1").arg(value.toObject().keys().join(QLatin1Char(',')));
    }
    return value.isString() ? QStringLiteral("ok: %1").arg(value.toString())
                            : QStringLiteral("ok: a non-object value");
}

QList<QByteArray> takeCompleteLines(QByteArray& buffer) {
    QList<QByteArray> lines;
    // qsizetype, not int: indexOf returns the buffer's index type, and narrowing it here would be a
    // silent wrap on a buffer larger than an int can address.
    qsizetype newline = buffer.indexOf('\n');
    while (newline >= 0) {
        QByteArray line = buffer.left(newline);
        buffer.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (!line.trimmed().isEmpty()) {
            lines.append(line);
        }
        newline = buffer.indexOf('\n');
    }
    return lines;
}

Reply decodeReplyLine(const QByteArray& line) {
    Reply reply;

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError) {
        reply.text = QStringLiteral("reply is not JSON: %1").arg(error.errorString());
        return reply;
    }
    if (!document.isObject()) {
        reply.text = QStringLiteral("reply is not a JSON object");
        return reply;
    }

    const QJsonObject object = document.object();
    if (object.contains(QStringLiteral("Ok"))) {
        reply.kind = Reply::Kind::ok;
        reply.value = object.value(QStringLiteral("Ok"));
        return reply;
    }

    const QJsonValue failure = object.value(QStringLiteral("Err"));
    if (failure.isString()) {
        reply.kind = Reply::Kind::error;
        reply.text = failure.toString();
        return reply;
    }

    reply.text = QStringLiteral("reply is neither an Ok nor an Err value");
    return reply;
}

QString oversizedBufferReason(const QByteArray& buffer) {
    if (buffer.size() <= maximumLineBytes) {
        return QString{};
    }
    return QStringLiteral("niri sent more than %1 bytes without a line break, which is not the IPC "
                          "protocol")
        .arg(maximumLineBytes);
}

bool EventLine::isValid() const {
    return !name.isEmpty();
}

EventLine decodeEventLine(const QByteArray& line) {
    EventLine event;

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return event;
    }

    const QJsonObject object = document.object();
    if (object.size() != 1) {
        // Every event is an externally tagged enum with exactly one key. Two keys means this is not
        // one, and guessing which half to believe would be how state desyncs silently.
        return event;
    }

    event.name = object.keys().first();
    event.fields = object.value(event.name);
    return event;
}

bool isKnownEventName(QStringView name) {
    for (const auto known : eventNames) {
        if (name == QLatin1StringView(known.data(), static_cast<qsizetype>(known.size()))) {
            return true;
        }
    }
    return false;
}

QStringList knownEventNames() {
    QStringList names;
    names.reserve(static_cast<qsizetype>(eventNames.size()));
    for (const auto known : eventNames) {
        names.append(QString::fromLatin1(known.data(), static_cast<qsizetype>(known.size())));
    }
    return names;
}

std::optional<quint64> idFromJson(const QJsonValue& value) {
    // QJsonValue carries doubles; an id beyond 2^53 would already have lost precision on the wire,
    // which niri cannot do either, so the double is the same value niri sent.
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double number = value.toDouble();
    if (number < 0) {
        return std::nullopt;
    }
    return static_cast<quint64>(number);
}

}  // namespace quantum::niri
