#include "niri/NiriActions.h"

#include <QJsonArray>
#include <QJsonValue>

#include <utility>

namespace quantum::niri {
namespace {

// niri's `WorkspaceReferenceArg::Index` and `LayoutSwitchTarget::Index` are both `u8`.
constexpr int maximumIndex = 255;

// A JSON number for a niri id. niri's ids are u64 and JSON has one number type, which this code holds
// as a double: exact up to 2^53. niri generates ids far below that, and the read side reads them the
// same way, but an id above 2^53 could not be addressed through a field encoded like this.
QJsonValue idValue(quint64 id) {
    return QJsonValue(static_cast<double>(id));
}

NiriActions::Result classifyReply(const Reply& reply) {
    NiriActions::Result result;
    switch (reply.kind) {
    case Reply::Kind::error:
        // Either the compositor parsed the action and declined it, or it could not parse the action at
        // all; niri answers both with an error, and the text says which.
        result.outcome = NiriActions::Result::Outcome::refused;
        result.detail = reply.text;
        return result;
    case Reply::Kind::transportError:
        result.outcome = NiriActions::Result::Outcome::notDelivered;
        result.detail = reply.text;
        return result;
    case Reply::Kind::ok:
        break;
    }

    // niri answers a performed action with `Response::Handled`, which is the bare string.
    if (reply.value.isString() && reply.value.toString() == QLatin1StringView("Handled")) {
        result.outcome = NiriActions::Result::Outcome::handled;
        return result;
    }
    result.outcome = NiriActions::Result::Outcome::unexpected;
    result.detail = reply.describe();
    return result;
}

}  // namespace

NiriActions::WorkspaceReference NiriActions::WorkspaceReference::atIndex(int index) {
    WorkspaceReference reference;
    reference.kind_ = Kind::index;
    reference.index_ = index;
    return reference;
}

NiriActions::WorkspaceReference NiriActions::WorkspaceReference::withId(quint64 id) {
    WorkspaceReference reference;
    reference.kind_ = Kind::id;
    reference.id_ = id;
    return reference;
}

NiriActions::WorkspaceReference NiriActions::WorkspaceReference::named(const QString& name) {
    WorkspaceReference reference;
    reference.kind_ = Kind::name;
    reference.name_ = name;
    return reference;
}

NiriActions::WorkspaceReference::Kind NiriActions::WorkspaceReference::kind() const {
    return kind_;
}

bool NiriActions::WorkspaceReference::isValid() const {
    switch (kind_) {
    case Kind::index:
        return index_ >= 0 && index_ <= maximumIndex;
    case Kind::id:
        return true;
    case Kind::name:
        return !name_.isEmpty();
    }
    return false;
}

QJsonObject NiriActions::WorkspaceReference::toJson() const {
    if (!isValid()) {
        return QJsonObject{};
    }
    switch (kind_) {
    case Kind::index:
        return QJsonObject{{QStringLiteral("Index"), QJsonValue(index_)}};
    case Kind::id:
        return QJsonObject{{QStringLiteral("Id"), idValue(id_)}};
    case Kind::name:
        return QJsonObject{{QStringLiteral("Name"), QJsonValue(name_)}};
    }
    return QJsonObject{};
}

QString NiriActions::WorkspaceReference::describe() const {
    switch (kind_) {
    case Kind::index:
        return QStringLiteral("workspace at index %1").arg(index_);
    case Kind::id:
        return QStringLiteral("workspace %1").arg(id_);
    case Kind::name:
        return QStringLiteral("workspace named \"%1\"").arg(name_);
    }
    return QStringLiteral("workspace");
}

NiriActions::LayoutTarget NiriActions::LayoutTarget::next() {
    return LayoutTarget{};
}

NiriActions::LayoutTarget NiriActions::LayoutTarget::previous() {
    LayoutTarget target;
    target.kind_ = Kind::previous;
    return target;
}

NiriActions::LayoutTarget NiriActions::LayoutTarget::atIndex(int index) {
    LayoutTarget target;
    target.kind_ = Kind::index;
    target.index_ = index;
    return target;
}

NiriActions::LayoutTarget::Kind NiriActions::LayoutTarget::kind() const {
    return kind_;
}

bool NiriActions::LayoutTarget::isValid() const {
    return kind_ != Kind::index || (index_ >= 0 && index_ <= maximumIndex);
}

QJsonObject NiriActions::LayoutTarget::toJson() const {
    if (!isValid()) {
        return QJsonObject{};
    }
    switch (kind_) {
    case Kind::next:
        return QJsonObject{{QStringLiteral("layout"), QJsonValue(QStringLiteral("Next"))}};
    case Kind::previous:
        return QJsonObject{{QStringLiteral("layout"), QJsonValue(QStringLiteral("Prev"))}};
    case Kind::index:
        return QJsonObject{{QStringLiteral("layout"),
                            QJsonObject{{QStringLiteral("Index"), QJsonValue(index_)}}}};
    }
    return QJsonObject{};
}

QString NiriActions::LayoutTarget::describe() const {
    switch (kind_) {
    case Kind::next:
        return QStringLiteral("the next layout");
    case Kind::previous:
        return QStringLiteral("the previous layout");
    case Kind::index:
        return QStringLiteral("the layout at index %1").arg(index_);
    }
    return QStringLiteral("a layout");
}

bool NiriActions::Result::isHandled() const {
    return outcome == Outcome::handled;
}

QString NiriActions::Result::describe() const {
    switch (outcome) {
    case Outcome::handled:
        return QStringLiteral("handled");
    case Outcome::refused:
        return QStringLiteral("refused: %1").arg(detail);
    case Outcome::notDelivered:
        return QStringLiteral("not delivered: %1").arg(detail);
    case Outcome::unexpected:
        return QStringLiteral("unexpected reply: %1").arg(detail);
    }
    return QStringLiteral("unknown outcome");
}

NiriActions::NiriActions(NiriIPC& requests, QObject* parent) : QObject(parent), requests_(requests) {}

void NiriActions::focusWorkspace(const WorkspaceReference& reference, ResultHandler handler) {
    if (refuseWithoutAWorkspace(QStringLiteral("FocusWorkspace"), reference, handler)) {
        return;
    }
    dispatch(QStringLiteral("FocusWorkspace"),
             QJsonObject{{QStringLiteral("reference"), QJsonValue(reference.toJson())}},
             std::move(handler));
}

void NiriActions::spawn(const QStringList& command, ResultHandler handler) {
    if (command.isEmpty()) {
        refuse(QStringLiteral("Spawn"), QStringLiteral("an empty command names no program to run"),
               handler);
        return;
    }
    QJsonArray arguments;
    for (const QString& argument : command) {
        arguments.append(argument);
    }
    dispatch(QStringLiteral("Spawn"),
             QJsonObject{{QStringLiteral("command"), QJsonValue(arguments)}}, std::move(handler));
}

void NiriActions::spawnSh(const QString& command, ResultHandler handler) {
    if (command.isEmpty()) {
        refuse(QStringLiteral("SpawnSh"), QStringLiteral("an empty command runs no shell"), handler);
        return;
    }
    dispatch(QStringLiteral("SpawnSh"),
             QJsonObject{{QStringLiteral("command"), QJsonValue(command)}}, std::move(handler));
}

void NiriActions::moveWindowToWorkspace(const WorkspaceReference& reference, bool focus,
                                        std::optional<quint64> windowId, ResultHandler handler) {
    if (refuseWithoutAWorkspace(QStringLiteral("MoveWindowToWorkspace"), reference, handler)) {
        return;
    }
    dispatch(QStringLiteral("MoveWindowToWorkspace"),
             QJsonObject{{QStringLiteral("reference"), QJsonValue(reference.toJson())},
                         {QStringLiteral("focus"), QJsonValue(focus)},
                         {QStringLiteral("window_id"),
                          windowId.has_value() ? idValue(*windowId) : QJsonValue()}},
             std::move(handler));
}

void NiriActions::screenshotScreen(bool writeToDisk, bool showPointer, ResultHandler handler) {
    dispatch(QStringLiteral("ScreenshotScreen"),
             QJsonObject{{QStringLiteral("write_to_disk"), QJsonValue(writeToDisk)},
                         {QStringLiteral("show_pointer"), QJsonValue(showPointer)},
                         {QStringLiteral("path"), QJsonValue()}},
             std::move(handler));
}

void NiriActions::openOverview(ResultHandler handler) {
    dispatch(QStringLiteral("OpenOverview"), QJsonObject{}, std::move(handler));
}

void NiriActions::closeOverview(ResultHandler handler) {
    dispatch(QStringLiteral("CloseOverview"), QJsonObject{}, std::move(handler));
}

void NiriActions::switchLayout(const LayoutTarget& target, ResultHandler handler) {
    if (!target.isValid()) {
        refuse(QStringLiteral("SwitchLayout"),
               QStringLiteral("a layout index must be between 0 and %1, which is what niri's `u8` "
                              "holds").arg(maximumIndex),
               handler);
        return;
    }
    dispatch(QStringLiteral("SwitchLayout"), target.toJson(), std::move(handler));
}

void NiriActions::refuse(const QString& action, const QString& reason,
                         const ResultHandler& handler) {
    // A reference or argument this build will not encode is not sent at all. The caller is told why,
    // rather than being handed the compositor's complaint about a request that was never formed here.
    Result result;
    result.outcome = Result::Outcome::notDelivered;
    result.detail = reason;
    if (handler) {
        handler(result);
    }
    emit actionFailed(action, result);
}

bool NiriActions::refuseWithoutAWorkspace(const QString& action,
                                          const WorkspaceReference& reference,
                                          const ResultHandler& handler) {
    if (reference.isValid()) {
        return false;
    }
    refuse(action,
           QStringLiteral("%1 is not a workspace this build can name: a workspace index must be "
                          "between 0 and %2, and a workspace name cannot be empty")
               .arg(reference.describe())
               .arg(maximumIndex),
           handler);
    return true;
}

void NiriActions::dispatch(const QString& action, const QJsonObject& fields,
                           const ResultHandler& handler) {
    const QJsonObject request{
        {QStringLiteral("Action"), QJsonObject{{action, QJsonValue(fields)}}}};
    requests_.sendObject(request, [this, action, handler](const Reply& reply) {
        const Result result = classifyReply(reply);
        if (handler) {
            handler(result);
        }
        if (!result.isHandled()) {
            emit actionFailed(action, result);
        }
    });
}

}  // namespace quantum::niri
