// A toplevel window, as niri reports it in niri-ipc v26.04's `Window`.
//
// Two fields of the wire struct are deliberately not parsed: `layout` and `focus_timestamp`. Nothing
// the shell does yet reads window geometry or the MRU timestamp, and parsing what nothing uses is how
// a field ends up believed rather than exercised. They are ignored, not rejected: a window carrying
// them parses exactly the same as one without.
//
// As with workspaces, an object without a usable `id` is invalid, and validity never rides on the id
// being non-zero.
#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace quantum::niri {

class NiriWindow {
public:
    static NiriWindow fromJson(const QJsonObject& object);

    bool isValid() const;

    quint64 id() const;
    QString title() const;   // empty when the window sets none
    QString appId() const;   // empty when the window sets none
    std::optional<qint32> pid() const;
    std::optional<quint64> workspaceId() const;
    bool isFocused() const;
    bool isFloating() const;
    bool isUrgent() const;

    // For log lines and test failure messages: identity without content. The title is left out on
    // purpose, because a title can name a document and diagnostics end up in logs.
    QString describe() const;

    bool operator==(const NiriWindow& other) const;
    bool operator!=(const NiriWindow& other) const;

    // The transitions NiriState applies as events arrive.
    void setFocused(bool focused);
    void setUrgent(bool urgent);
    void setWorkspaceId(std::optional<quint64> workspaceId);

private:
    bool valid_ = false;
    quint64 id_ = 0;
    QString title_;
    QString appId_;
    std::optional<qint32> pid_;
    std::optional<quint64> workspaceId_;
    bool focused_ = false;
    bool floating_ = false;
    bool urgent_ = false;
};

}  // namespace quantum::niri
