// The names QML sees, declared once.
//
// These are interface, not implementation detail: a QML file written against them keeps working only
// while the spelling holds, so a rename here is the same kind of change as renaming an IPC verb. They
// live in one file so there is a single place to read them, a single place to add one, and a single
// list for a test to compare against — `niri_service_test.cpp` mirrors the arrays below and compares
// them at compile time, so a key added to a shape without a matching decision in that test does not
// build.
//
// A key earns a place here only when a widget reads it. `NiriService`'s own header names the binding
// each property exists for; the keys are the same idea one level down.
//
// Optional keys are declared separately from always-present ones because their *absence* is a claim.
// When niri reports no active window for a workspace, or no logical output for a monitor, the service
// leaves the key out rather than reporting zero — so a widget can tell "nothing there" from a real
// reading of 0. Each optional group below names the condition under which its keys appear, and
// `NiriService.cpp` inserts them inside exactly that condition.
#pragma once

#include <array>

namespace quantum::niri::qml {

// --- one workspace in the strip: `NiriService.workspaces[i]` ---
inline constexpr auto WorkspaceId = "id";
inline constexpr auto WorkspaceIdx = "idx";
inline constexpr auto WorkspaceName = "name";
inline constexpr auto WorkspaceOutput = "output";
inline constexpr auto WorkspaceIsActive = "isActive";
inline constexpr auto WorkspaceIsFocused = "isFocused";
inline constexpr auto WorkspaceIsUrgent = "isUrgent";

// Present only when the compositor reported an active window for this workspace.
inline constexpr auto WorkspaceActiveWindowId = "activeWindowId";

inline constexpr std::array WorkspaceKeys{WorkspaceId,      WorkspaceIdx,     WorkspaceName,
                                          WorkspaceOutput,  WorkspaceIsActive, WorkspaceIsFocused,
                                          WorkspaceIsUrgent};
inline constexpr std::array WorkspaceOptionalKeys{WorkspaceActiveWindowId};

// --- the focused window: `NiriService.focusedWindow` ---
inline constexpr auto WindowId = "id";
inline constexpr auto WindowTitle = "title";
inline constexpr auto WindowAppId = "appId";
inline constexpr auto WindowIsFocused = "isFocused";
inline constexpr auto WindowIsFloating = "isFloating";
inline constexpr auto WindowIsUrgent = "isUrgent";

// Present only when the window is on a workspace, which is how niri reports a window it has placed.
// A floating window with no workspace of its own has no `workspaceId` key.
inline constexpr auto WindowWorkspaceId = "workspaceId";

inline constexpr std::array WindowKeys{WindowId,         WindowTitle,      WindowAppId,
                                       WindowIsFocused,  WindowIsFloating, WindowIsUrgent};
inline constexpr std::array WindowOptionalKeys{WindowWorkspaceId};

// --- one output: `NiriService.outputs[i]` ---
inline constexpr auto OutputName = "name";
inline constexpr auto OutputMake = "make";
inline constexpr auto OutputModel = "model";
inline constexpr auto OutputIsEnabled = "isEnabled";
inline constexpr auto OutputIsVrrEnabled = "isVrrEnabled";

// Present only when niri reports a logical output, which is how it says the monitor is enabled and
// mapped. A disabled output has no position and no scale, and inventing 0/1.0 would be a reading the
// compositor never gave.
inline constexpr auto OutputX = "x";
inline constexpr auto OutputY = "y";
inline constexpr auto OutputWidth = "width";
inline constexpr auto OutputHeight = "height";
inline constexpr auto OutputScale = "scale";
inline constexpr auto OutputTransform = "transform";

// Present only when niri reports a current mode. Millihertz, exactly as niri reports it.
inline constexpr auto OutputModeWidth = "modeWidth";
inline constexpr auto OutputModeHeight = "modeHeight";
inline constexpr auto OutputRefreshRate = "refreshRate";

inline constexpr std::array OutputKeys{OutputName, OutputMake, OutputModel, OutputIsEnabled,
                                       OutputIsVrrEnabled};
inline constexpr std::array OutputGeometryKeys{OutputX,      OutputY,        OutputWidth,
                                               OutputHeight, OutputScale,    OutputTransform};
inline constexpr std::array OutputModeKeys{OutputModeWidth, OutputModeHeight, OutputRefreshRate};

// --- the layout indicator: `NiriService.keyboardLayout` ---
inline constexpr auto KeyboardLayoutNames = "names";
inline constexpr auto KeyboardLayoutCurrentIndex = "currentIndex";
inline constexpr auto KeyboardLayoutCurrentName = "currentName";

inline constexpr std::array KeyboardLayoutKeys{KeyboardLayoutNames, KeyboardLayoutCurrentIndex,
                                              KeyboardLayoutCurrentName};

}  // namespace quantum::niri::qml
