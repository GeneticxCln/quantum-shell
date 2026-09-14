#include "niri/NiriService.h"

#include "niri/NiriEventStream.h"
#include "niri/NiriKeyboardLayouts.h"
#include "niri/NiriOutput.h"
#include "niri/NiriQmlModule.h"
#include "niri/NiriServiceKeys.h"
#include "niri/NiriState.h"
#include "niri/NiriWindow.h"
#include "niri/NiriWorkspace.h"

#include <QQmlEngine>

#include <optional>
#include <utility>

namespace quantum::niri {
namespace {

// `QVariant::fromValue` on a quint64 would reach QML as a large number QML's double cannot hold exactly
// above 2^53. niri documents that ids may be generated at random, so they are passed as strings, which
// QML compares and displays exactly.
QString idText(quint64 id) {
    return QString::number(id);
}

QVariantMap workspaceMap(const NiriWorkspace& workspace) {
    QVariantMap map;
    map.insert(qml::WorkspaceId, idText(workspace.id()));
    map.insert(qml::WorkspaceIdx, workspace.idx());
    map.insert(qml::WorkspaceName, workspace.name());
    map.insert(qml::WorkspaceOutput, workspace.output());
    map.insert(qml::WorkspaceIsActive, workspace.isActive());
    map.insert(qml::WorkspaceIsFocused, workspace.isFocused());
    map.insert(qml::WorkspaceIsUrgent, workspace.isUrgent());
    const std::optional<quint64> activeWindow = workspace.activeWindowId();
    if (activeWindow.has_value()) {
        map.insert(qml::WorkspaceActiveWindowId, idText(*activeWindow));
    }
    return map;
}

QVariantMap windowMap(const NiriWindow& window) {
    if (!window.isValid()) {
        // An invalid window means there is none — no focused window, or a focus event for a window the
        // model does not have. Building a map from its defaults would hand QML `id: "0"` and an empty
        // title, which a widget cannot tell from a real window: an empty map can be told apart.
        return {};
    }
    QVariantMap map;
    map.insert(qml::WindowId, idText(window.id()));
    map.insert(qml::WindowTitle, window.title());
    map.insert(qml::WindowAppId, window.appId());
    map.insert(qml::WindowIsFocused, window.isFocused());
    map.insert(qml::WindowIsFloating, window.isFloating());
    map.insert(qml::WindowIsUrgent, window.isUrgent());
    if (window.workspaceId().has_value()) {
        map.insert(qml::WindowWorkspaceId, idText(*window.workspaceId()));
    }
    return map;
}

QVariantMap outputMap(const NiriOutput& output) {
    QVariantMap map;
    map.insert(qml::OutputName, output.name());
    map.insert(qml::OutputMake, output.make());
    map.insert(qml::OutputModel, output.model());
    map.insert(qml::OutputIsEnabled, output.isEnabled());
    map.insert(qml::OutputIsVrrEnabled, output.isVrrEnabled());

    // A disabled or unmapped output carries no logical output, so these keys are absent rather than
    // zero: a widget reading `width` gets nothing instead of a size that would be a fabrication.
    if (const std::optional<NiriLogicalOutput> logical = output.logical(); logical.has_value()) {
        map.insert(qml::OutputX, logical->x);
        map.insert(qml::OutputY, logical->y);
        map.insert(qml::OutputWidth, logical->width);
        map.insert(qml::OutputHeight, logical->height);
        // Fractional and left as niri reported it: 1.25 on the machine this was verified against.
        map.insert(qml::OutputScale, logical->scale);
        map.insert(qml::OutputTransform, logical->transform);
    }
    if (const std::optional<NiriOutputMode> mode = output.currentMode(); mode.has_value()) {
        map.insert(qml::OutputModeWidth, mode->width);
        map.insert(qml::OutputModeHeight, mode->height);
        // Millihertz, exactly as niri reports it, so nothing here rounds 59.997 Hz to 60.
        map.insert(qml::OutputRefreshRate, mode->refreshRate);
    }
    return map;
}

QVariantMap keyboardLayoutMap(const NiriKeyboardLayouts& layouts) {
    if (!layouts.isValid()) {
        // Nothing has been reported yet. An empty map is the honest answer; a layout named "us" here
        // would be a reading the compositor never gave.
        return {};
    }
    QVariantMap map;
    map.insert(qml::KeyboardLayoutNames, layouts.names());
    map.insert(qml::KeyboardLayoutCurrentIndex, layouts.currentIndex());
    map.insert(qml::KeyboardLayoutCurrentName, layouts.currentName());
    return map;
}

QVariantList workspaceList(const QList<NiriWorkspace>& workspaces) {
    QVariantList list;
    list.reserve(workspaces.size());
    for (const NiriWorkspace& workspace : workspaces) {
        list.append(workspaceMap(workspace));
    }
    return list;
}

QVariantList outputList(const QList<NiriOutput>& outputs) {
    QVariantList list;
    list.reserve(outputs.size());
    for (const NiriOutput& output : outputs) {
        list.append(outputMap(output));
    }
    return list;
}

}  // namespace

NiriService::NiriService(NiriState& state, NiriEventStream& stream, QObject* parent)
    : QObject(parent), state_(state), stream_(stream) {
    // Seeded from the model rather than left empty: a service created after the compositor has already
    // reported its state must not look like one created before it did.
    workspaces_ = workspaceList(state_.workspaces());
    focusedWindow_ = windowMap(state_.focusedWindow());
    outputs_ = outputList(state_.outputs());
    keyboardLayout_ = keyboardLayoutMap(state_.keyboardLayouts());
    overviewOpen_ = state_.isOverviewOpen();
    connected_ = stream_.isStreaming();

    connect(&state_, &NiriState::workspacesChanged, this, &NiriService::refreshWorkspaces);
    connect(&state_, &NiriState::windowsChanged, this, &NiriService::refreshFocusedWindow);
    connect(&state_, &NiriState::focusedWindowChanged, this, &NiriService::refreshFocusedWindow);
    connect(&state_, &NiriState::outputsChanged, this, &NiriService::refreshOutputs);
    connect(&state_, &NiriState::keyboardLayoutsChanged, this, &NiriService::refreshKeyboardLayout);
    connect(&state_, &NiriState::overviewChanged, this, &NiriService::refreshOverview);

    // `streaming` is the subscription being acknowledged, not the socket being up: a connection that
    // has not been acknowledged has delivered no state, so it is not a connection QML can rely on.
    connect(&stream_, &NiriEventStream::streaming, this, &NiriService::refreshConnected);
    connect(&stream_, &NiriEventStream::disconnected, this, &NiriService::refreshConnected);
}

QVariantList NiriService::workspaces() const {
    return workspaces_;
}

QVariantMap NiriService::focusedWindow() const {
    return focusedWindow_;
}

QVariantList NiriService::outputs() const {
    return outputs_;
}

QVariantMap NiriService::keyboardLayout() const {
    return keyboardLayout_;
}

bool NiriService::overviewOpen() const {
    return overviewOpen_;
}

bool NiriService::connected() const {
    return connected_;
}

void NiriService::registerQmlSingleton(NiriService& service) {
    // Uncreatable from QML on purpose: this object is owned by the shell's C++, and a second one would
    // be a second view of the compositor that nothing updates. The names come from NiriQmlModule.h
    // rather than being spelled here, so there is one place they are written down.
    qmlRegisterSingletonInstance(qml::ModuleUri, qml::ModuleMajorVersion, qml::ModuleMinorVersion,
                                 qml::ServiceTypeName, &service);
}

void NiriService::refreshWorkspaces() {
    QVariantList list = workspaceList(state_.workspaces());
    if (list == workspaces_) {
        return;
    }
    workspaces_ = std::move(list);
    emit workspacesChanged();
}

void NiriService::refreshFocusedWindow() {
    QVariantMap map = windowMap(state_.focusedWindow());
    if (map == focusedWindow_) {
        return;
    }
    focusedWindow_ = std::move(map);
    emit focusedWindowChanged();
}

void NiriService::refreshOutputs() {
    QVariantList list = outputList(state_.outputs());
    if (list == outputs_) {
        return;
    }
    outputs_ = std::move(list);
    emit outputsChanged();
}

void NiriService::refreshKeyboardLayout() {
    QVariantMap map = keyboardLayoutMap(state_.keyboardLayouts());
    if (map == keyboardLayout_) {
        return;
    }
    keyboardLayout_ = std::move(map);
    emit keyboardLayoutChanged();
}

void NiriService::refreshOverview() {
    if (overviewOpen_ == state_.isOverviewOpen()) {
        return;
    }
    overviewOpen_ = state_.isOverviewOpen();
    emit overviewOpenChanged();
}

void NiriService::refreshConnected() {
    if (connected_ == stream_.isStreaming()) {
        return;
    }
    connected_ = stream_.isStreaming();
    emit connectedChanged();
}

}  // namespace quantum::niri
