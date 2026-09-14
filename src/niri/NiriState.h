// What the shell renders from: the workspaces, windows, keyboard layouts and overview state niri
// currently has, plus the connected outputs and whether niri's config last loaded. Everything except
// the outputs is kept in step by the events that NiriEventStream decodes; outputs are request answers
// (see NiriOutputs.h).
//
// One source of truth per fact, so nothing can drift out of sync. Focus is a flag on the focused
// window rather than a second id kept beside it; which workspace is active is a flag on the workspace.
// Change signals fire only when a value actually changed, so an event that arrives and changes nothing
// does not wake every binding above it.
//
// The events that are more than a field update, as niri documents them:
//   - WorkspacesChanged and WindowsChanged replace the whole set: anything missing from them is gone.
//   - an opening or changed window that is focused makes every other window unfocused.
//   - activating a workspace deactivates the others on the same output, and if it is focused, it is
//     the only focused workspace across all outputs.
//
// An event naming something this model does not have is ignored rather than guessed at; the stream is
// where surprises are reported.
#pragma once

#include "niri/NiriKeyboardLayouts.h"
#include "niri/NiriOutput.h"
#include "niri/NiriWindow.h"
#include "niri/NiriWorkspace.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

namespace quantum::niri {

class NiriEventStream;
class NiriOutputs;

class NiriState : public QObject {
    Q_OBJECT

public:
    explicit NiriState(QObject* parent = nullptr);

    // Applies every typed event of `stream`, and forgets the state if the compositor goes away. One
    // call, so a consumer cannot half-wire the two and get a model that is quietly stale.
    void observe(NiriEventStream& stream);

    // Applies the output snapshots of `outputs`. Separate from observe() because outputs are the one
    // part of this model that niri does not send events for (NiriOutputs.h).
    void observeOutputs(NiriOutputs& outputs);

    // Ordered for display: by output, then by the workspace's index on that output, then by id, so
    // the order is stable for a given compositor state and never depends on hash order.
    QList<NiriWorkspace> workspaces() const;
    QList<NiriWorkspace> workspacesOn(const QString& output) const;
    NiriWorkspace workspace(quint64 id) const;  // invalid when unknown

    QList<NiriWindow> windows() const;  // ordered by id
    QList<NiriWindow> windowsOn(quint64 workspaceId) const;
    NiriWindow window(quint64 id) const;  // invalid when unknown

    NiriWorkspace focusedWorkspace() const;  // invalid when none
    NiriWindow focusedWindow() const;        // invalid when none

    QList<NiriOutput> outputs() const;             // ordered by connector name
    NiriOutput output(const QString& name) const;  // invalid when unknown

    // Invalid until the compositor has reported them. niri sends the configured layouts on connect.
    NiriKeyboardLayouts keyboardLayouts() const;
    bool isOverviewOpen() const;
    // Whether the last config load niri reported had failed. niri always sends that report on connect,
    // so a false here means a successful load or nothing reported yet — never a guess.
    bool configLoadFailed() const;

    // Forgets everything, which is the truth when the compositor is gone.
    void clear();

    // The model's inputs. Public because they are the model's whole interface, not because a caller
    // should normally use them: NiriState::observe wires them to a stream. They are also how the tests
    // drive transitions without a socket.
    void applyWorkspacesChanged(const QList<NiriWorkspace>& workspaces);
    void applyWindowsChanged(const QList<NiriWindow>& windows);
    void applyWindowOpenedOrChanged(const NiriWindow& window);
    void applyWindowClosed(quint64 windowId);
    void applyWindowUrgencyChanged(quint64 windowId, bool urgent);
    void applyFocusedWindowChanged(quint64 windowId, bool hasWindow);
    void applyWorkspaceActivated(quint64 workspaceId, bool focused);
    void applyWorkspaceUrgencyChanged(quint64 workspaceId, bool urgent);
    void applyWorkspaceActiveWindowChanged(quint64 workspaceId, quint64 activeWindowId, bool hasWindow);
    void applyOutputsChanged(const QList<NiriOutput>& outputs);
    void applyKeyboardLayoutsChanged(const NiriKeyboardLayouts& layouts);
    void applyKeyboardLayoutSwitched(int index);
    void applyOverviewOpenedOrClosed(bool isOpen);
    void applyConfigLoaded(bool failed);

signals:
    void workspacesChanged();
    void windowsChanged();
    void focusedWindowChanged();
    void outputsChanged();
    void keyboardLayoutsChanged();
    void overviewChanged();
    void configLoadFailedChanged();

private:
    NiriWorkspace* editableWorkspace(quint64 id);
    NiriWindow* editableWindow(quint64 id);
    void applyFocus(quint64 windowId, bool hasWindow);

    QHash<quint64, NiriWorkspace> workspaces_;
    QHash<quint64, NiriWindow> windows_;
    QHash<QString, NiriOutput> outputs_;
    NiriKeyboardLayouts keyboardLayouts_;
    bool overviewOpen_ = false;
    bool configLoadFailed_ = false;
};

}  // namespace quantum::niri
