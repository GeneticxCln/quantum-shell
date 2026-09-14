#include "niri/NiriWindow.h"

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

NiriWindow NiriWindow::fromJson(const QJsonObject& object) {
    NiriWindow window;

    const std::optional<quint64> id = idFromJson(object.value(QStringLiteral("id")));
    if (!id.has_value()) {
        return window;
    }

    window.valid_ = true;
    window.id_ = *id;
    window.title_ = stringField(object, QLatin1StringView("title"));
    window.appId_ = stringField(object, QLatin1StringView("app_id"));
    window.workspaceId_ = idFromJson(object.value(QStringLiteral("workspace_id")));
    window.focused_ = boolField(object, QLatin1StringView("is_focused"));
    window.floating_ = boolField(object, QLatin1StringView("is_floating"));
    window.urgent_ = boolField(object, QLatin1StringView("is_urgent"));

    const QJsonValue process = object.value(QStringLiteral("pid"));
    if (process.isDouble()) {
        window.pid_ = static_cast<qint32>(process.toInt());
    }
    return window;
}

bool NiriWindow::isValid() const {
    return valid_;
}

quint64 NiriWindow::id() const {
    return id_;
}

QString NiriWindow::title() const {
    return title_;
}

QString NiriWindow::appId() const {
    return appId_;
}

std::optional<qint32> NiriWindow::pid() const {
    return pid_;
}

std::optional<quint64> NiriWindow::workspaceId() const {
    return workspaceId_;
}

bool NiriWindow::isFocused() const {
    return focused_;
}

bool NiriWindow::isFloating() const {
    return floating_;
}

bool NiriWindow::isUrgent() const {
    return urgent_;
}

QString NiriWindow::describe() const {
    if (!valid_) {
        return QStringLiteral("window <invalid: no id>");
    }
    QStringList flags;
    if (focused_) {
        flags << QStringLiteral("focused");
    }
    if (urgent_) {
        flags << QStringLiteral("urgent");
    }
    if (floating_) {
        flags << QStringLiteral("floating");
    }
    return QStringLiteral("window %1 (app %2, workspace %3%4)")
        .arg(id_)
        .arg(appId_.isEmpty() ? QStringLiteral("<none>") : appId_,
             workspaceId_.has_value() ? QString::number(*workspaceId_) : QStringLiteral("<none>"),
             flags.isEmpty() ? QString{} : QStringLiteral(", %1").arg(flags.join(QLatin1Char('+'))));
}

bool NiriWindow::operator==(const NiriWindow& other) const {
    return valid_ == other.valid_ && id_ == other.id_ && title_ == other.title_ &&
           appId_ == other.appId_ && pid_ == other.pid_ && workspaceId_ == other.workspaceId_ &&
           focused_ == other.focused_ && floating_ == other.floating_ && urgent_ == other.urgent_;
}

bool NiriWindow::operator!=(const NiriWindow& other) const {
    return !(*this == other);
}

void NiriWindow::setFocused(bool focused) {
    focused_ = focused;
}

void NiriWindow::setUrgent(bool urgent) {
    urgent_ = urgent;
}

void NiriWindow::setWorkspaceId(std::optional<quint64> workspaceId) {
    workspaceId_ = workspaceId;
}

}  // namespace quantum::niri
