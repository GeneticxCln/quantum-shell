// The QML module this library registers, declared once.
//
// A QML file is written against these three names — the import URI and its version, and the type it
// looks the singleton up by — so they are interface of the same kind as the key names in
// NiriServiceKeys.h, one level up: they decide whether `import QuantumShell 1.0` and `NiriService`
// resolve at all. Renaming any of them breaks every file already written that way, silently and at
// load time rather than at build time.
//
// They are therefore written down in one place, `registerQmlSingleton` uses these constants instead of
// repeating literals, and `niri_service_test.cpp` mirrors them and compares its copy at compile time.
// A rename cannot happen without a matching edit to that test, which is where the question "who else
// imports this" gets asked.
//
// The design intends this registration to move into a real QML module once `qml/` exists
// (QUANTUM_SHELL.md § Configuration and IPC). When it does, these constants move with it and the mirror
// in the test follows; until then a name declared here is the whole of the interface.
#pragma once

#include "QmlModule.h"

namespace quantum::niri::qml {

// The module itself is the shell's, not this service's, and its URI and version are declared once in
// `QmlModule.h` so that every service registered into it agrees with every import of it. These are
// re-exports under the names this header has always used; the test that mirrors them is unchanged.
inline constexpr auto ModuleUri = quantum::qml::ModuleUri;
inline constexpr int ModuleMajorVersion = quantum::qml::ModuleMajorVersion;
inline constexpr int ModuleMinorVersion = quantum::qml::ModuleMinorVersion;
inline constexpr auto ServiceTypeName = "NiriService";

// The second singleton in the same module, and a name of the same kind: the bar's capsules and its wheel
// call into it to focus a workspace, so a QML file written against it keeps working only while the
// spelling holds. `bar-interaction-test` mirrors it and compares its copy at compile time, for the same
// reason `niri_service_test` mirrors the one above.
inline constexpr auto ActionTypeName = "NiriActions";

}  // namespace quantum::niri::qml
