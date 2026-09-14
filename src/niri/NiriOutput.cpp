#include "niri/NiriOutput.h"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>

namespace quantum::niri {
namespace {

QString stringField(const QJsonObject& object, QLatin1StringView key) {
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : QString{};
}

// The wire says u16 and u32, so these cannot be out of range in practice; the clamps exist only so a
// nonsensical value cannot turn into undefined behaviour in the conversion.
quint16 toUint16(const QJsonValue& value) {
    const double number = value.toDouble(0.0);
    return static_cast<quint16>(std::clamp(number, 0.0, 65535.0));
}

quint32 toUint32(const QJsonValue& value) {
    const double number = value.toDouble(0.0);
    return static_cast<quint32>(std::clamp(number, 0.0, 4294967295.0));
}

QList<NiriOutputMode> parseModes(const QJsonArray& array) {
    QList<NiriOutputMode> modes;
    modes.reserve(array.size());
    for (const QJsonValue& value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        NiriOutputMode mode;
        mode.width = toUint16(object.value(QStringLiteral("width")));
        mode.height = toUint16(object.value(QStringLiteral("height")));
        mode.refreshRate = toUint32(object.value(QStringLiteral("refresh_rate")));
        mode.isPreferred = object.value(QStringLiteral("is_preferred")).toBool(false);
        modes.append(mode);
    }
    return modes;
}

std::optional<NiriLogicalOutput> parseLogical(const QJsonValue& value) {
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = value.toObject();
    NiriLogicalOutput logical;
    logical.x = static_cast<qint32>(object.value(QStringLiteral("x")).toInt(0));
    logical.y = static_cast<qint32>(object.value(QStringLiteral("y")).toInt(0));
    logical.width = toUint32(object.value(QStringLiteral("width")));
    logical.height = toUint32(object.value(QStringLiteral("height")));
    logical.scale = object.value(QStringLiteral("scale")).toDouble(0.0);
    logical.transform = stringField(object, QLatin1StringView("transform"));
    return logical;
}

}  // namespace

bool NiriOutputMode::operator==(const NiriOutputMode& other) const {
    return width == other.width && height == other.height && refreshRate == other.refreshRate
           && isPreferred == other.isPreferred;
}

bool NiriOutputMode::operator!=(const NiriOutputMode& other) const {
    return !(*this == other);
}

bool NiriLogicalOutput::transformIsKnown() const {
    return NiriOutput::knownTransforms().contains(transform);
}

bool NiriLogicalOutput::operator==(const NiriLogicalOutput& other) const {
    return x == other.x && y == other.y && width == other.width && height == other.height
           && scale == other.scale && transform == other.transform;
}

bool NiriLogicalOutput::operator!=(const NiriLogicalOutput& other) const {
    return !(*this == other);
}

QStringList NiriOutput::knownTransforms() {
    return {QStringLiteral("Normal"),   QStringLiteral("_90"),        QStringLiteral("_180"),
            QStringLiteral("_270"),     QStringLiteral("Flipped"),    QStringLiteral("Flipped90"),
            QStringLiteral("Flipped180"), QStringLiteral("Flipped270")};
}

NiriOutput NiriOutput::fromJson(const QJsonObject& object) {
    NiriOutput output;

    const QJsonValue name = object.value(QStringLiteral("name"));
    if (!name.isString() || name.toString().isEmpty()) {
        return output;
    }

    output.valid_ = true;
    output.name_ = name.toString();
    output.make_ = stringField(object, QLatin1StringView("make"));
    output.model_ = stringField(object, QLatin1StringView("model"));

    const QJsonValue modes = object.value(QStringLiteral("modes"));
    if (modes.isArray()) {
        output.modes_ = parseModes(modes.toArray());
    }

    // An absent or null index means niri reports no current mode, which is what a disabled output
    // looks like. An index outside the mode list is kept as reported and simply does not resolve.
    const QJsonValue currentMode = object.value(QStringLiteral("current_mode"));
    if (currentMode.isDouble() && currentMode.toDouble() >= 0.0) {
        output.currentModeIndex_ = static_cast<int>(currentMode.toDouble());
    }

    output.vrrSupported_ = object.value(QStringLiteral("vrr_supported")).toBool(false);
    output.vrrEnabled_ = object.value(QStringLiteral("vrr_enabled")).toBool(false);
    output.logical_ = parseLogical(object.value(QStringLiteral("logical")));
    return output;
}

bool NiriOutput::isValid() const {
    return valid_;
}

QString NiriOutput::name() const {
    return name_;
}

QString NiriOutput::make() const {
    return make_;
}

QString NiriOutput::model() const {
    return model_;
}

QList<NiriOutputMode> NiriOutput::modes() const {
    return modes_;
}

std::optional<int> NiriOutput::currentModeIndex() const {
    return currentModeIndex_;
}

std::optional<NiriOutputMode> NiriOutput::currentMode() const {
    if (!currentModeIndex_.has_value() || *currentModeIndex_ < 0
        || *currentModeIndex_ >= modes_.size()) {
        return std::nullopt;
    }
    return modes_.at(*currentModeIndex_);
}

bool NiriOutput::isVrrSupported() const {
    return vrrSupported_;
}

bool NiriOutput::isVrrEnabled() const {
    return vrrEnabled_;
}

bool NiriOutput::isEnabled() const {
    return logical_.has_value();
}

std::optional<NiriLogicalOutput> NiriOutput::logical() const {
    return logical_;
}

QString NiriOutput::describe() const {
    if (!valid_) {
        return QStringLiteral("invalid output");
    }
    const QString label = model_.isEmpty() ? name_ : QStringLiteral("%1 %2").arg(name_, model_);
    if (!logical_.has_value()) {
        return QStringLiteral("%1 (disabled)").arg(label);
    }

    QString refresh = QStringLiteral("no current mode");
    if (const std::optional<NiriOutputMode> mode = currentMode(); mode.has_value()) {
        refresh = QStringLiteral("%1x%2@%3Hz")
                      .arg(mode->width)
                      .arg(mode->height)
                      .arg(QString::number(mode->refreshRate / 1000.0, 'f', 2));
    }

    QString transform = logical_->transform;
    if (!logical_->transformIsKnown()) {
        transform = QStringLiteral("%1 (unknown transform)").arg(transform);
    }
    // Example: "DP-3 MO32U 3072x1728 logical (3840x2160@119.88Hz), scale 1.25, transform Normal, vrr"
    // — the logical size, then the mode behind it, then the two readings that must never be assumed.
    return QStringLiteral("%1 %2x%3 logical (%4), scale %5, transform %6%7")
        .arg(label)
        .arg(logical_->width)
        .arg(logical_->height)
        .arg(refresh)
        .arg(QString::number(logical_->scale, 'g', 6))
        .arg(transform)
        .arg(vrrEnabled_ ? QStringLiteral(", vrr") : QString{});
}

bool NiriOutput::operator==(const NiriOutput& other) const {
    return valid_ == other.valid_ && name_ == other.name_ && make_ == other.make_
           && model_ == other.model_ && modes_ == other.modes_
           && currentModeIndex_ == other.currentModeIndex_ && vrrSupported_ == other.vrrSupported_
           && vrrEnabled_ == other.vrrEnabled_ && logical_ == other.logical_;
}

bool NiriOutput::operator!=(const NiriOutput& other) const {
    return !(*this == other);
}

}  // namespace quantum::niri
