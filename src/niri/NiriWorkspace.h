// A workspace, as niri reports it in niri-ipc v26.04's `Workspace`.
//
// Parsing is tolerant field by field: a field the compositor omits, or spells with another type,
// leaves its default behind instead of failing the whole workspace, because losing one workspace is a
// smaller failure than losing the stream. `id` is the exception — without it the workspace cannot be
// tracked across events at all, so such an object is invalid and the model refuses it.
//
// niri documents that ids need not start at 1, need not increase, and may be generated at random, so
// id 0 is never used here as a stand-in for "no workspace": validity is carried separately.
#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace quantum::niri {

class NiriWorkspace {
public:
    // An invalid workspace (isValid() == false) when the object carries no usable id.
    static NiriWorkspace fromJson(const QJsonObject& object);

    bool isValid() const;

    quint64 id() const;
    int idx() const;      // position on its output; per niri, unique only within that output
    QString name() const;   // empty when the workspace is unnamed
    QString output() const;  // empty when it is on no output
    bool isUrgent() const;
    bool isActive() const;   // currently visible on its output
    bool isFocused() const;  // the one focused workspace across all outputs
    std::optional<quint64> activeWindowId() const;

    // For log lines and test failure messages. Never shown in the UI.
    QString describe() const;

    bool operator==(const NiriWorkspace& other) const;
    bool operator!=(const NiriWorkspace& other) const;

    // The transitions NiriState applies as events arrive. They exist because the state owns these
    // changes, not as a general invitation to mutate a parsed value.
    void setActive(bool active);
    void setFocused(bool focused);
    void setUrgent(bool urgent);
    void setActiveWindowId(std::optional<quint64> windowId);

private:
    bool valid_ = false;
    quint64 id_ = 0;
    int idx_ = 0;
    QString name_;
    QString output_;
    bool urgent_ = false;
    bool active_ = false;
    bool focused_ = false;
    std::optional<quint64> activeWindowId_;
};

}  // namespace quantum::niri
