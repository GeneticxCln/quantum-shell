#include "niri/NiriState.h"

#include "niri/NiriEventStream.h"
#include "niri/NiriOutputs.h"

#include <algorithm>
#include <utility>

namespace quantum::niri {

NiriState::NiriState(QObject* parent) : QObject(parent) {}

void NiriState::observe(NiriEventStream& stream) {
    connect(&stream, &NiriEventStream::workspacesChanged, this, &NiriState::applyWorkspacesChanged);
    connect(&stream, &NiriEventStream::windowsChanged, this, &NiriState::applyWindowsChanged);
    connect(&stream, &NiriEventStream::windowOpenedOrChanged, this, &NiriState::applyWindowOpenedOrChanged);
    connect(&stream, &NiriEventStream::windowClosed, this, &NiriState::applyWindowClosed);
    connect(&stream, &NiriEventStream::windowUrgencyChanged, this, &NiriState::applyWindowUrgencyChanged);
    connect(&stream, &NiriEventStream::focusedWindowChanged, this, &NiriState::applyFocusedWindowChanged);
    connect(&stream, &NiriEventStream::workspaceActivated, this, &NiriState::applyWorkspaceActivated);
    connect(&stream, &NiriEventStream::workspaceUrgencyChanged, this, &NiriState::applyWorkspaceUrgencyChanged);
    connect(&stream, &NiriEventStream::workspaceActiveWindowChanged, this,
            &NiriState::applyWorkspaceActiveWindowChanged);
    connect(&stream, &NiriEventStream::keyboardLayoutsChanged, this,
            &NiriState::applyKeyboardLayoutsChanged);
    connect(&stream, &NiriEventStream::keyboardLayoutSwitched, this,
            &NiriState::applyKeyboardLayoutSwitched);
    connect(&stream, &NiriEventStream::overviewOpenedOrClosed, this,
            &NiriState::applyOverviewOpenedOrClosed);
    connect(&stream, &NiriEventStream::configLoaded, this, &NiriState::applyConfigLoaded);
    connect(&stream, &NiriEventStream::disconnected, this, &NiriState::clear);
}

void NiriState::observeOutputs(NiriOutputs& outputs) {
    connect(&outputs, &NiriOutputs::outputsChanged, this, &NiriState::applyOutputsChanged);
    // The compositor is gone: the outputs this model holds came from it, so they go with it. The
    // stream's own disconnect covers the case where only the request connection dropped, because
    // NiriOutputs clears its list and reports it.
}

QList<NiriWorkspace> NiriState::workspaces() const {
    QList<NiriWorkspace> list = workspaces_.values();
    std::sort(list.begin(), list.end(), [](const NiriWorkspace& left, const NiriWorkspace& right) {
        if (left.output() != right.output()) {
            return left.output() < right.output();
        }
        if (left.idx() != right.idx()) {
            return left.idx() < right.idx();
        }
        return left.id() < right.id();
    });
    return list;
}

QList<NiriWorkspace> NiriState::workspacesOn(const QString& output) const {
    QList<NiriWorkspace> list;
    for (const NiriWorkspace& workspace : workspaces()) {
        if (workspace.output() == output) {
            list.append(workspace);
        }
    }
    return list;
}

NiriWorkspace NiriState::workspace(quint64 id) const {
    return workspaces_.value(id);
}

QList<NiriWindow> NiriState::windows() const {
    QList<NiriWindow> list = windows_.values();
    std::sort(list.begin(), list.end(),
              [](const NiriWindow& left, const NiriWindow& right) { return left.id() < right.id(); });
    return list;
}

QList<NiriWindow> NiriState::windowsOn(quint64 workspaceId) const {
    QList<NiriWindow> list;
    for (const NiriWindow& candidate : windows()) {
        if (candidate.workspaceId() == workspaceId) {
            list.append(candidate);
        }
    }
    return list;
}

NiriWindow NiriState::window(quint64 id) const {
    return windows_.value(id);
}

NiriWorkspace NiriState::focusedWorkspace() const {
    for (const NiriWorkspace& candidate : workspaces()) {
        if (candidate.isFocused()) {
            return candidate;
        }
    }
    return NiriWorkspace{};
}

NiriWindow NiriState::focusedWindow() const {
    for (const NiriWindow& candidate : windows()) {
        if (candidate.isFocused()) {
            return candidate;
        }
    }
    return NiriWindow{};
}

QList<NiriOutput> NiriState::outputs() const {
    QList<NiriOutput> list = outputs_.values();
    std::sort(list.begin(), list.end(),
              [](const NiriOutput& left, const NiriOutput& right) {
                  return left.name() < right.name();
              });
    return list;
}

NiriOutput NiriState::output(const QString& name) const {
    return outputs_.value(name);
}

NiriKeyboardLayouts NiriState::keyboardLayouts() const {
    return keyboardLayouts_;
}

bool NiriState::isOverviewOpen() const {
    return overviewOpen_;
}

bool NiriState::configLoadFailed() const {
    return configLoadFailed_;
}

void NiriState::clear() {
    // Each part is reported only if it had something to forget, so a consumer bound to one of these
    // signals is not woken by a clear that did not touch its part.
    const bool hadWorkspaces = !workspaces_.isEmpty();
    const bool hadWindows = !windows_.isEmpty();
    const bool hadOutputs = !outputs_.isEmpty();
    const bool hadLayouts = keyboardLayouts_.isValid();
    const bool hadOverview = overviewOpen_;
    const bool hadConfigFailure = configLoadFailed_;
    if (!hadWorkspaces && !hadWindows && !hadOutputs && !hadLayouts && !hadOverview
        && !hadConfigFailure) {
        return;
    }

    workspaces_.clear();
    windows_.clear();
    outputs_.clear();
    keyboardLayouts_ = NiriKeyboardLayouts{};
    overviewOpen_ = false;
    configLoadFailed_ = false;

    if (hadWorkspaces) {
        emit workspacesChanged();
    }
    if (hadWindows) {
        emit windowsChanged();
        emit focusedWindowChanged();
    }
    if (hadOutputs) {
        emit outputsChanged();
    }
    if (hadLayouts) {
        emit keyboardLayoutsChanged();
    }
    if (hadOverview) {
        emit overviewChanged();
    }
    if (hadConfigFailure) {
        emit configLoadFailedChanged();
    }
}

NiriWorkspace* NiriState::editableWorkspace(quint64 id) {
    auto found = workspaces_.find(id);
    return found == workspaces_.end() ? nullptr : &(*found);
}

NiriWindow* NiriState::editableWindow(quint64 id) {
    auto found = windows_.find(id);
    return found == windows_.end() ? nullptr : &(*found);
}

void NiriState::applyWorkspacesChanged(const QList<NiriWorkspace>& workspaces) {
    const QList<NiriWorkspace> before = this->workspaces();

    QHash<quint64, NiriWorkspace> replacement;
    for (const NiriWorkspace& workspace : workspaces) {
        if (workspace.isValid()) {
            replacement.insert(workspace.id(), workspace);
        }
    }
    workspaces_ = std::move(replacement);

    // Windows are deliberately left alone: a window can reference a workspace that has just been
    // removed, and inventing a fix for that here would hide the inconsistency rather than show it.
    if (this->workspaces() != before) {
        emit workspacesChanged();
    }
}

void NiriState::applyWindowsChanged(const QList<NiriWindow>& windows) {
    const QList<NiriWindow> before = this->windows();
    const NiriWindow focusedBefore = focusedWindow();

    QHash<quint64, NiriWindow> replacement;
    for (const NiriWindow& window : windows) {
        if (window.isValid()) {
            replacement.insert(window.id(), window);
        }
    }
    windows_ = std::move(replacement);

    if (this->windows() != before) {
        emit windowsChanged();
    }
    if (focusedWindow() != focusedBefore) {
        emit focusedWindowChanged();
    }
}

void NiriState::applyWindowOpenedOrChanged(const NiriWindow& window) {
    if (!window.isValid()) {
        return;
    }

    const QList<NiriWindow> before = this->windows();
    windows_.insert(window.id(), window);

    // niri documents this event as authoritative about focus: a focused window means no other window
    // is focused.
    if (window.isFocused()) {
        applyFocus(window.id(), true);
    }

    if (this->windows() != before) {
        emit windowsChanged();
    }
}

void NiriState::applyWindowClosed(quint64 windowId) {
    if (windows_.isEmpty()) {
        return;
    }

    const QList<NiriWindow> before = this->windows();
    const NiriWindow closed = windows_.take(windowId);
    if (!closed.isValid()) {
        return;
    }

    // The focused window is gone; focus is nowhere until an event says otherwise. Even if another
    // focus event is already on its way, claiming a vanished window is focused would be wrong now.
    if (closed.isFocused()) {
        applyFocus(0, false);
    }

    if (this->windows() != before) {
        emit windowsChanged();
    }
}

void NiriState::applyWindowUrgencyChanged(quint64 windowId, bool urgent) {
    NiriWindow* window = editableWindow(windowId);
    if (window == nullptr || window->isUrgent() == urgent) {
        return;
    }
    window->setUrgent(urgent);
    emit windowsChanged();
}

void NiriState::applyFocusedWindowChanged(quint64 windowId, bool hasWindow) {
    applyFocus(hasWindow ? windowId : 0, hasWindow);
}

void NiriState::applyWorkspaceActivated(quint64 workspaceId, bool focused) {
    NiriWorkspace* activated = editableWorkspace(workspaceId);
    if (activated == nullptr) {
        return;
    }

    const QList<NiriWorkspace> before = workspaces();
    const QString output = activated->output();

    for (NiriWorkspace& workspace : workspaces_) {
        if (workspace.id() == workspaceId) {
            continue;
        }
        if (workspace.output() == output) {
            workspace.setActive(false);
        }
        if (focused && workspace.isFocused()) {
            workspace.setFocused(false);
        }
    }
    activated = editableWorkspace(workspaceId);
    if (activated == nullptr) {
        return;
    }
    activated->setActive(true);
    if (focused) {
        activated->setFocused(true);
    }

    if (workspaces() != before) {
        emit workspacesChanged();
    }
}

void NiriState::applyWorkspaceUrgencyChanged(quint64 workspaceId, bool urgent) {
    NiriWorkspace* workspace = editableWorkspace(workspaceId);
    if (workspace == nullptr || workspace->isUrgent() == urgent) {
        return;
    }
    workspace->setUrgent(urgent);
    emit workspacesChanged();
}

void NiriState::applyWorkspaceActiveWindowChanged(quint64 workspaceId,
                                                  quint64 activeWindowId,
                                                  bool hasWindow) {
    NiriWorkspace* workspace = editableWorkspace(workspaceId);
    if (workspace == nullptr) {
        return;
    }
    const std::optional<quint64> active = hasWindow ? std::optional<quint64>{activeWindowId}
                                                    : std::optional<quint64>{};
    if (workspace->activeWindowId() == active) {
        return;
    }
    workspace->setActiveWindowId(active);
    emit workspacesChanged();
}

void NiriState::applyOutputsChanged(const QList<NiriOutput>& outputs) {
    const QList<NiriOutput> before = this->outputs();

    QHash<QString, NiriOutput> replacement;
    for (const NiriOutput& output : outputs) {
        if (output.isValid()) {
            replacement.insert(output.name(), output);
        }
    }
    outputs_ = std::move(replacement);

    if (this->outputs() != before) {
        emit outputsChanged();
    }
}

void NiriState::applyKeyboardLayoutsChanged(const NiriKeyboardLayouts& layouts) {
    if (!layouts.isValid() || keyboardLayouts_ == layouts) {
        return;
    }
    keyboardLayouts_ = layouts;
    emit keyboardLayoutsChanged();
}

void NiriState::applyKeyboardLayoutSwitched(int index) {
    if (!keyboardLayouts_.isValid() || keyboardLayouts_.currentIndex() == index) {
        return;
    }
    keyboardLayouts_.setCurrentIndex(index);
    emit keyboardLayoutsChanged();
}

void NiriState::applyOverviewOpenedOrClosed(bool isOpen) {
    if (overviewOpen_ == isOpen) {
        return;
    }
    overviewOpen_ = isOpen;
    emit overviewChanged();
}

void NiriState::applyConfigLoaded(bool failed) {
    if (configLoadFailed_ == failed) {
        return;
    }
    configLoadFailed_ = failed;
    emit configLoadFailedChanged();
}

void NiriState::applyFocus(quint64 windowId, bool hasWindow) {
    const NiriWindow focusedBefore = focusedWindow();

    bool changed = false;
    for (NiriWindow& window : windows_) {
        const bool shouldBeFocused = hasWindow && window.id() == windowId;
        if (window.isFocused() != shouldBeFocused) {
            window.setFocused(shouldBeFocused);
            changed = true;
        }
    }

    // A focus event for a window this model does not have still cleared every other window, because
    // the compositor said only that one is focused. If nothing changed there is nothing to report.
    if (changed || focusedWindow() != focusedBefore) {
        emit windowsChanged();
        emit focusedWindowChanged();
    }
}

}  // namespace quantum::niri
