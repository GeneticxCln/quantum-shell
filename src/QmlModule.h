// The one QML module the shell's C++ registers its singletons into.
//
// Everything the shell exposes to QML lives under a single import — `import QuantumShell 1.0` — so the
// URI and its version belong to the shell rather than to any one service. They are declared here, once,
// for the same reason the key names in `NiriServiceKeys.h` are: they are interface. A QML file is
// written against them, and a second copy of the string in a second service is a copy that can drift —
// silently, because two services registered under different URIs still build and only fail when a QML
// file imports one of them.
//
// `NiriQmlModule.h` re-exports these under its own namespace, which is where the names were first
// written down, so nothing that already reads them changes.
#pragma once

namespace quantum::qml {

inline constexpr auto ModuleUri = "QuantumShell";
inline constexpr int ModuleMajorVersion = 1;
inline constexpr int ModuleMinorVersion = 0;

}  // namespace quantum::qml
