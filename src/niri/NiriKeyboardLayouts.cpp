#include "niri/NiriKeyboardLayouts.h"

#include <QJsonArray>
#include <QJsonValue>

namespace quantum::niri {

NiriKeyboardLayouts NiriKeyboardLayouts::fromJson(const QJsonObject& object) {
    NiriKeyboardLayouts layouts;

    // Called with the value of the `keyboard_layouts` field; the caller passes the object itself for
    // the test that reads one straight from the wire.
    const QJsonValue names = object.value(QStringLiteral("names"));
    if (!names.isArray()) {
        return layouts;
    }

    // Every entry has to be a string, or the object is refused outright. Skipping a bad entry would
    // shift every index after it, and `current_idx` would then point at a different layout than the
    // compositor reported — a wrong answer, where an unreadable list is merely an absent one.
    const QJsonArray list = names.toArray();
    for (const QJsonValue& name : list) {
        if (!name.isString()) {
            return NiriKeyboardLayouts{};
        }
    }

    layouts.valid_ = true;
    for (const QJsonValue& name : list) {
        layouts.names_.append(name.toString());
    }
    const QJsonValue index = object.value(QStringLiteral("current_idx"));
    if (index.isDouble() && index.toDouble() >= 0.0) {
        layouts.currentIndex_ = static_cast<int>(index.toDouble());
    }
    return layouts;
}

bool NiriKeyboardLayouts::isValid() const {
    return valid_;
}

QStringList NiriKeyboardLayouts::names() const {
    return names_;
}

int NiriKeyboardLayouts::size() const {
    return static_cast<int>(names_.size());
}

int NiriKeyboardLayouts::currentIndex() const {
    return currentIndex_;
}

QString NiriKeyboardLayouts::currentName() const {
    if (currentIndex_ < 0 || currentIndex_ >= names_.size()) {
        return QString{};
    }
    return names_.at(currentIndex_);
}

bool NiriKeyboardLayouts::isEmpty() const {
    return names_.isEmpty();
}

void NiriKeyboardLayouts::setCurrentIndex(int index) {
    currentIndex_ = index;
}

QString NiriKeyboardLayouts::describe() const {
    if (!valid_) {
        return QStringLiteral("no keyboard layouts reported");
    }
    if (names_.isEmpty()) {
        return QStringLiteral("no keyboard layouts configured");
    }
    const QString active = currentName();
    return QStringLiteral("%1 of %2 (%3)")
        .arg(currentIndex_)
        .arg(names_.join(QLatin1Char(',')))
        .arg(active.isEmpty() ? QStringLiteral("index names no layout") : active);
}

bool NiriKeyboardLayouts::operator==(const NiriKeyboardLayouts& other) const {
    return valid_ == other.valid_ && names_ == other.names_ && currentIndex_ == other.currentIndex_;
}

bool NiriKeyboardLayouts::operator!=(const NiriKeyboardLayouts& other) const {
    return !(*this == other);
}

}  // namespace quantum::niri
