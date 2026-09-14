// The configured keyboard layouts, from niri-ipc v26.04's `KeyboardLayouts`: the XKB names plus the
// index of the active one.
//
// This does arrive as an event — `KeyboardLayoutsChanged` carries the whole struct, and
// `KeyboardLayoutSwitched` carries only the new index, which is why the index is applied to the names
// already held rather than parsed from a fresh object.
//
// `currentIndex` is kept exactly as reported. When it names nothing — no layouts reported yet, or an
// index past the end of the list — currentName() answers with an empty string rather than the first
// layout or the last one, because either would be a layout the compositor did not name.
#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace quantum::niri {

class NiriKeyboardLayouts {
public:
    // Invalid when the object carries no `names` array at all. An empty array is a real, parsed state:
    // no layouts are configured, and there is no name to point at.
    static NiriKeyboardLayouts fromJson(const QJsonObject& object);

    bool isValid() const;

    QStringList names() const;
    int size() const;
    int currentIndex() const;
    // The active layout's name, or an empty string when the index names nothing.
    QString currentName() const;
    bool isEmpty() const;

    // The index niri reports after a `KeyboardLayoutSwitched`. Out-of-range indices are stored, not
    // clamped: clamping would silently point at a different layout than the compositor did.
    void setCurrentIndex(int index);

    // For log lines and test failure messages. Never shown in the UI.
    QString describe() const;

    bool operator==(const NiriKeyboardLayouts& other) const;
    bool operator!=(const NiriKeyboardLayouts& other) const;

private:
    bool valid_ = false;
    QStringList names_;
    int currentIndex_ = 0;
};

}  // namespace quantum::niri

// Layouts travel through signals, so they can be queued across threads later.
Q_DECLARE_METATYPE(quantum::niri::NiriKeyboardLayouts)
