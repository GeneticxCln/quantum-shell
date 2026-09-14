#include "niri/NiriWorkspace.h"

#include "niri/NiriProtocol.h"

#include <QJsonValue>

namespace quantum::niri {
namespace {

QString stringField(const QJsonObject& object, QLatin1StringView key) {
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : QString{};
}

bool boolField(const QJsonObject& object, QLatin1StringView key) {
    return object.value(key).toBool(false);
}

}  // namespace

NiriWorkspace NiriWorkspace::fromJson(const QJsonObject& object) {
    NiriWorkspace workspace;

    const std::optional<quint64> id = idFromJson(object.value(QStringLiteral("id")));
    if (!id.has_value()) {
        return workspace;
    }

    workspace.valid_ = true;
    workspace.id_ = *id;
    workspace.idx_ = object.value(QStringLiteral("idx")).toInt(0);
    workspace.name_ = stringField(object, QLatin1StringView("name"));
    workspace.output_ = stringField(object, QLatin1StringView("output"));
    workspace.urgent_ = boolField(object, QLatin1StringView("is_urgent"));
    workspace.active_ = boolField(object, QLatin1StringView("is_active"));
    workspace.focused_ = boolField(object, QLatin1StringView("is_focused"));
    workspace.activeWindowId_ = idFromJson(object.value(QStringLiteral("active_window_id")));
    return workspace;
}

bool NiriWorkspace::isValid() const {
    return valid_;
}

quint64 NiriWorkspace::id() const {
    return id_;
}

int NiriWorkspace::idx() const {
    return idx_;
}

QString NiriWorkspace::name() const {
    return name_;
}

QString NiriWorkspace::output() const {
    return output_;
}

bool NiriWorkspace::isUrgent() const {
    return urgent_;
}

bool NiriWorkspace::isActive() const {
    return active_;
}

bool NiriWorkspace::isFocused() const {
    return focused_;
}

std::optional<quint64> NiriWorkspace::activeWindowId() const {
    return activeWindowId_;
}

QString NiriWorkspace::describe() const {
    if (!valid_) {
        return QStringLiteral("workspace <invalid: no id>");
    }
    QStringList flags;
    if (active_) {
        flags << QStringLiteral("active");
    }
    if (focused_) {
        flags << QStringLiteral("focused");
    }
    if (urgent_) {
        flags << QStringLiteral("urgent");
    }
    return QStringLiteral("workspace %1 (idx %2%3%4%5)")
        .arg(id_)
        .arg(idx_)
        .arg(name_.isEmpty() ? QString{} : QStringLiteral(", name \"%1\"").arg(name_),
             output_.isEmpty() ? QString{} : QStringLiteral(", output %1").arg(output_),
             flags.isEmpty() ? QString{} : QStringLiteral(", %1").arg(flags.join(QLatin1Char('+'))));
}

bool NiriWorkspace::operator==(const NiriWorkspace& other) const {
    return valid_ == other.valid_ && id_ == other.id_ && idx_ == other.idx_ &&
           name_ == other.name_ && output_ == other.output_ && urgent_ == other.urgent_ &&
           active_ == other.active_ && focused_ == other.focused_ &&
           activeWindowId_ == other.activeWindowId_;
}

bool NiriWorkspace::operator!=(const NiriWorkspace& other) const {
    return !(*this == other);
}

void NiriWorkspace::setActive(bool active) {
    active_ = active;
}

void NiriWorkspace::setFocused(bool focused) {
    focused_ = focused;
}

void NiriWorkspace::setUrgent(bool urgent) {
    urgent_ = urgent;
}

void NiriWorkspace::setActiveWindowId(std::optional<quint64> windowId) {
    activeWindowId_ = windowId;
}

}  // namespace quantum::niri
