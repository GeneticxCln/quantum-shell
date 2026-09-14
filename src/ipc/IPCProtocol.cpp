#include "ipc/IPCProtocol.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include <optional>

namespace quantum::ipc {
namespace {

// The field names on the wire, written once. They are not public names on their own — a client sends them
// and this shell reads them — but they are spelled here rather than at each use, because a frame written
// with the wrong spelling decodes as a missing field rather than as a typo.
constexpr auto VersionField = "version";
constexpr auto VerbField = "verb";
constexpr auto PathField = "path";
constexpr auto OkField = "ok";
constexpr auto DataField = "data";
constexpr auto ErrorField = "error";

// One line as a JSON object, or the reason it is not one. Shared by both decoders so a request and a
// response are held to the same standard: a frame that is not an object is refused by name, never
// dereferenced as an empty one.
std::optional<QJsonObject> parseObject(const QByteArray& line, QString* error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = QStringLiteral("not JSON: %1").arg(parseError.errorString());
        return std::nullopt;
    }
    if (!document.isObject()) {
        *error = QStringLiteral("not a JSON object");
        return std::nullopt;
    }
    return document.object();
}

}  // namespace

QString decodeRequest(const QByteArray& line, Request* request)
{
    *request = Request{};

    QString error;
    const std::optional<QJsonObject> object = parseObject(line, &error);
    if (!object.has_value())
        return error;

    const QJsonValue version = object->value(QLatin1StringView(VersionField));
    if (!version.isDouble())
        return QStringLiteral("no \"%1\" field; every request carries the protocol version")
            .arg(QLatin1StringView(VersionField));
    request->version = version.toInt();

    const QJsonValue verb = object->value(QLatin1StringView(VerbField));
    if (verb.isUndefined()) {
        // The handshake frame as QUANTUM_SHELL.md writes it: a version and nothing else. It is decoded as
        // the `version` request rather than as a special case, so there is one handler for both and the
        // handshake cannot drift away from the verb that reports the same thing.
        request->verb = QString::fromLatin1(verb::Version);
    } else if (verb.isString()) {
        request->verb = verb.toString();
    } else {
        return QStringLiteral("\"%1\" is not a string").arg(QLatin1StringView(VerbField));
    }

    const QJsonValue path = object->value(QLatin1StringView(PathField));
    if (path.isString())
        request->path = path.toString();
    else if (!path.isUndefined())
        return QStringLiteral("\"%1\" is not a string").arg(QLatin1StringView(PathField));

    return QString();
}

QByteArray encodeRequest(const Request& request)
{
    QJsonObject object;
    object.insert(QLatin1StringView(VersionField), request.version);
    object.insert(QLatin1StringView(VerbField), request.verb);
    if (!request.path.isEmpty())
        object.insert(QLatin1StringView(PathField), request.path);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray encodeResponse(const Response& response)
{
    QJsonObject object;
    object.insert(QLatin1StringView(VersionField), ProtocolVersion);
    object.insert(QLatin1StringView(OkField), response.ok);
    if (response.ok) {
        // Present even when empty: a client reads `data` on success without checking whether the key is
        // there, and a verb that reports nothing would otherwise be the one shape that breaks that.
        object.insert(QLatin1StringView(DataField), response.data);
    } else {
        // Never empty: a refusal that says nothing is indistinguishable from a bug, so a caller that
        // forgot to write one gets a sentence saying so rather than a blank error.
        object.insert(QLatin1StringView(ErrorField),
                      response.error.isEmpty() ? QStringLiteral("refused without a reason") : response.error);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString decodeResponse(const QByteArray& line, Response* response)
{
    *response = Response{};

    QString error;
    const std::optional<QJsonObject> object = parseObject(line, &error);
    if (!object.has_value())
        return error;

    const QJsonValue version = object->value(QLatin1StringView(VersionField));
    if (!version.isDouble())
        return QStringLiteral("no \"%1\" field; every response carries the protocol version")
            .arg(QLatin1StringView(VersionField));
    response->version = version.toInt();

    const QJsonValue ok = object->value(QLatin1StringView(OkField));
    if (!ok.isBool())
        return QStringLiteral("no \"%1\" field; every response says whether it succeeded")
            .arg(QLatin1StringView(OkField));
    response->ok = ok.toBool();

    if (response->ok) {
        response->data = object->value(QLatin1StringView(DataField)).toObject();
    } else {
        response->error = object->value(QLatin1StringView(ErrorField)).toString();
        if (response->error.isEmpty())
            return QStringLiteral("a refusal with no \"%1\" message").arg(QLatin1StringView(ErrorField));
    }

    return QString();
}

QList<QByteArray> takeLines(QByteArray& buffer, QString* error)
{
    QList<QByteArray> lines;
    qsizetype newline = buffer.indexOf('\n');
    while (newline >= 0) {
        QByteArray line = buffer.left(newline);
        buffer.remove(0, newline + 1);
        if (line.endsWith('\r'))
            line.chop(1);

        if (line.size() > MaxLineBytes) {
            // Refused rather than kept: the caller answers nothing for this line and closes, because the
            // alternative is holding a client's bytes until it decides to stop sending them.
            *error = QStringLiteral("a line longer than %1 bytes was refused").arg(MaxLineBytes);
            buffer.clear();
            return lines;
        }

        lines.append(line);
        newline = buffer.indexOf('\n');
    }

    if (buffer.size() > MaxLineBytes) {
        *error = QStringLiteral("a line longer than %1 bytes was refused").arg(MaxLineBytes);
        buffer.clear();
    }

    return lines;
}

}  // namespace quantum::ipc
