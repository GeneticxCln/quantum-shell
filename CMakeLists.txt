cmake_minimum_required(VERSION 3.31...4.4)

project(QuantumShell
    VERSION 0.1.0
    DESCRIPTION "Native Wayland desktop shell for niri"
    # C is enabled for the Wayland protocol code generator, which emits C: wayland-scanner's output for
    # a protocol is what defines the wl_interface symbols a client binds against, and it is C by
    # construction. Without this the generated .c is silently left out of its target and the shell links
    # against interface symbols nothing defines. Every source written by hand in this repository is
    # C++23; the only C in the build is generated from src/wayland/protocols/.
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(QUANTUM_SHELL_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# QUANTUM_SHELL.md § Technology Stack pins toml++ to 3.4 and describes two ways to get it: a pinned
# FetchContent, and a find_package path for distro builds. Detection is what decides which is used, so a
# build works with no flag either way; this option is for the one case detection cannot cover, where a
# distro build must be pinned to the packages it was told to build against and silently fetching a second
# copy into the tree would defeat that.
option(QUANTUM_SHELL_SYSTEM_DEPS "Require system copies of pinned dependencies instead of fetching them" OFF)

# Every target built here links this policy target. A warning is a defect that has not been paid for
# yet, so -Werror is not optional and not configurable.
add_library(quantum-shell-warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(quantum-shell-warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wshadow
        -Wcast-qual
        -Wold-style-cast
        -Wnon-virtual-dtor
        -Wmissing-declarations)
endif()

if(QUANTUM_SHELL_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "sanitizer builds need GCC or Clang")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# --- Qt -------------------------------------------------------------------------------------------
# Qt 6.11 is the project floor (SYSTEM_PROMPT.md § Version floors, QUANTUM_SHELL.md § Technology
# Stack). Qt Test ships with it, so the C++ tests need no second framework. Gui and Quick arrive with
# the bar; WaylandClient and WaylandScannerTools arrive with the layer-shell integration, which is
# Route 1 of QUANTUM_SHELL.md § Layer-Shell Implementation Strategy.
# Qt has no public layer-shell client API, so Route 1 is built on QtWaylandClient's private shell
# integration interface — the documented switch for that is QT_FIND_PRIVATE_MODULES, which makes the
# WaylandClient package also load Qt6::WaylandClientPrivate and its private headers. There is no
# public alternative: the whole point of the route is the shell surface Qt has no public way to
# create. A Qt minor that moves these headers is a compile error here rather than a silent change.
set(QT_FIND_PRIVATE_MODULES ON)
# Concurrent arrives with the configuration watcher, which parses a changed file on a worker thread rather
# than on the one drawing the bar. It is in the project's Qt list already (QUANTUM_SHELL.md § Technology
# Stack).
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network Qml Test Gui Quick Concurrent)
find_package(Qt6 6.11 REQUIRED COMPONENTS WaylandClient WaylandScannerTools)
qt_standard_project_setup()
# QTP0001 is a Qt Qml policy about where a QML module's files are found. No QML module is created
# here — the QML is compiled into the executable's resources and the C++ types are registered by name
# in main.cpp — so the policy is left at its default until a module needs it.

# --- Quantum Shell: logging -----------------------------------------------------------------------
# The shell's categories and its record format (QUANTUM_SHELL.md § Logging). A library of its own because
# every other library logs — the niri connection, the configuration and the layer-shell route all have
# something to say — and the names they say it under have to be declared once, or two libraries end up with
# two spellings that look alike in a log.
#
# Core only. Nothing here can drag a display or a socket into a library that must not have one, which is also
# why the destination is left to Qt: a terminal when there is one, the systemd journal when there is not.
add_library(quantum-shell-logging STATIC src/app/Logging.cpp)
target_include_directories(quantum-shell-logging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(quantum-shell-logging PUBLIC Qt6::Core quantum-shell-warnings)

# --- Quantum Shell: niri IPC ----------------------------------------------------------------------
# The shell's connection to the compositor: the socket, line framing, request and reply decoding, the
# version/capability detection that everything else gates features on, and the QObject service QML
# binds to. Core and Qml, no Gui and no Quick, so it builds and is tested without a session and
# without a display.
add_library(quantum-shell-niri STATIC
    src/niri/NiriActions.cpp
    src/niri/NiriEventStream.cpp
    src/niri/NiriIPC.cpp
    src/niri/NiriKeyboardLayouts.cpp
    src/niri/NiriOutput.cpp
    src/niri/NiriOutputs.cpp
    src/niri/NiriProtocol.cpp
    src/niri/NiriReconnect.cpp
    src/niri/NiriService.cpp
    src/niri/NiriState.cpp
    src/niri/NiriVersion.cpp
    src/niri/NiriWindow.cpp
    src/niri/NiriWorkspace.cpp)
target_include_directories(quantum-shell-niri PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(quantum-shell-niri
    PUBLIC Qt6::Core Qt6::Network Qt6::Qml quantum-shell-warnings
    PRIVATE quantum-shell-logging)

# --- toml++ -------------------------------------------------------------------------------------
# The configuration parser named by QUANTUM_SHELL.md § Technology Stack. The version is pinned either
# way: an installed package has to be at least 3.4, and the fetched copy is the exact tag. `toml++` is
# header-only, so nothing is linked for it — what the package supplies is the include path.
find_package(tomlplusplus 3.4 QUIET)
if(tomlplusplus_FOUND)
    message(STATUS "toml++: using the installed package")
elseif(QUANTUM_SHELL_SYSTEM_DEPS)
    message(FATAL_ERROR
        "QUANTUM_SHELL_SYSTEM_DEPS is ON, so toml++ 3.4 must come from an installed package, and none "
        "was found. Install it, or turn the option off to fetch the pinned tag instead.")
else()
    include(FetchContent)
    FetchContent_Declare(tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG v3.4.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(tomlplusplus)
    message(STATUS "toml++: fetched v3.4.0")
endif()

# --- Quantum Shell: configuration -----------------------------------------------------------------
# The schema, the QObject tree QML binds to, and the watcher that keeps the two in step with the file.
# Core and Qml so the values can be tested and the QML registration proved without a display, plus
# Concurrent for the off-thread parse. Deliberately not part of the niri library: nothing here knows the
# compositor exists, and a configuration file that could only be read while niri was running would be a
# strange thing for a shell to have.
add_library(quantum-shell-config STATIC
    src/config/Config.cpp
    src/config/ConfigSchema.cpp
    src/config/ConfigWatcher.cpp)
target_include_directories(quantum-shell-config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(quantum-shell-config
    PUBLIC Qt6::Core Qt6::Qml tomlplusplus::tomlplusplus quantum-shell-warnings
    PRIVATE Qt6::Concurrent quantum-shell-logging)

# --- Quantum Shell: local IPC ---------------------------------------------------------------------
# The socket, the frames and the frozen verb names (QUANTUM_SHELL.md § IPC), plus the command-line client's
# logic. That last part is here rather than in the binary for the reason the binary is small: everything
# decidable from strings is testable, and only the connection is not.
#
# Core and Network, no Gui and no Qml: `qsctl` links this too, and a client has no business initialising a
# display or an engine to ask the shell a question.
add_library(quantum-shell-ipc STATIC
    src/ipc/IPCProtocol.cpp
    src/ipc/IPCServer.cpp
    src/ipc/QsctlCli.cpp)
target_include_directories(quantum-shell-ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(quantum-shell-ipc
    PUBLIC Qt6::Core Qt6::Network quantum-shell-warnings
    PRIVATE quantum-shell-logging)

# What the IPC can reach in this shell: three delegations, to the QML service's values, to the schema's
# resolver and to the bar window's own visibility. It is a library rather than part of the binary so that
# `ipc-capabilities-test` can drive it against a real service and a real configuration — the half of the IPC
# that a test double in `ipc-server-test` deliberately does not cover.
add_library(quantum-shell-capabilities STATIC src/app/ShellCapabilities.cpp)
target_include_directories(quantum-shell-capabilities PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(quantum-shell-capabilities
    PUBLIC quantum-shell-ipc quantum-shell-niri quantum-shell-config Qt6::Gui
    PRIVATE quantum-shell-warnings)

# `qsctl`, the command the shell is answered by. Its own binary target (QUANTUM_SHELL.md § Project Layout)
# and its own version, so a bug report quoting a client quotes the build it came from.
add_executable(qsctl src/ipc/qsctl/main.cpp)
target_link_libraries(qsctl PRIVATE quantum-shell-ipc quantum-shell-warnings)
target_compile_definitions(qsctl PRIVATE QS_VERSION="${PROJECT_VERSION}")

# --- qs-scan: the repository real-code gate -------------------------------------------------------
# A real program, not a shell one-liner in a document, so the rules in SYSTEM_PROMPT.md can fail a
# build. It has no dependency on Qt, which keeps the gate runnable on any CI image.
add_executable(qs-scan
    tools/qs-scan/main.cpp
    tools/qs-scan/Scanner.cpp)
target_include_directories(qs-scan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/qs-scan)
target_link_libraries(qs-scan PRIVATE quantum-shell-warnings)

set(QS_SCAN_PATTERNS ${CMAKE_CURRENT_SOURCE_DIR}/tools/qs-scan/patterns.txt)

# --- Quantum Shell: layer shell -------------------------------------------------------------------
# Route 1 of QUANTUM_SHELL.md § Layer-Shell Implementation Strategy: the shell's own
# zwlr_layer_shell_v1 bindings, generated from the vendored protocol and attached to the wl_surface
# Qt's Wayland platform plugin creates. Qt ships no layer-shell client API, so the role is set here
# while Qt keeps ownership of the surface, its buffer and its rendering.
#
# Shared rather than static on purpose: two binaries handle the same QObject type. The QML application
# constructs `LayerShellWindow` and the shell-integration plugin Qt loads reads it, and with a static
# library those would be two distinct QMetaObjects whose cast between each other returns null.
#
# src/wayland/protocols/wlr-layer-shell-unstable-v1.xml is the upstream wlr-protocols file, unmodified:
# 407 lines, sha256 87e0b9c837aecd6977f76f3c47d73088b7159871f5d979dc1840f6cadb5e2ed8. It is vendored
# because nothing on a normal system installs it — Qt ships layer-shell for neither the client nor the
# compositor side — and vendoring the protocol definition is what "own protocol bindings" means.
# The generated bindings are compiled in a target of their own so they are not built under this
# project's warning policy. They are machine-written from the XML by wayland-scanner and do not survive
# -Werror: they cast qualifiers away, they use old-style casts, and they name their parameters after
# enum members of the class they are in. A separate target is used rather than per-file options because
# the flags arrive from an INTERFACE target, so their order against a `-w` set on the sources is not
# something to rely on. Every file a person writes here is still built with the full policy.
add_library(quantum-shell-layershell-protocol STATIC)
qt6_generate_wayland_protocol_client_sources(quantum-shell-layershell-protocol
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/wayland/protocols/wlr-layer-shell-unstable-v1.xml)
target_include_directories(quantum-shell-layershell-protocol PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(quantum-shell-layershell-protocol PUBLIC Qt6::Core Qt6::WaylandClientPrivate)

# zwlr_layer_surface_v1.get_popup names the xdg_popup interface, so the interface table wayland-scanner
# generates for layer-shell refers to `xdg_popup_interface`, and something has to define it or every
# consumer fails to link. Qt's client library does not export it — what exports that symbol is the
# *compositor* library, which is the wrong side of the protocol — so the xdg-shell C is generated here.
#
# Only the C is generated, never Qt's bindings for it: Qt already has QtWayland::xdg_* classes in
# libQt6WaylandClient, and a second set under the same names would be a duplicate definition. Nothing in
# this repository sends an xdg-shell request; the interface table is the entire requirement.
#
# The XML is Qt's own copy of the upstream wayland-protocols file — the same one Qt builds its xdg-shell
# client from, so it cannot disagree with the Qt this links against. The lookup fails loudly here rather
# than leaving an undefined-symbol link error for someone to decode later.
set(QS_XDG_SHELL_XML "")
foreach(candidate
        "${QT6_INSTALL_PREFIX}/share/qt6/wayland/protocols/xdg-shell/xdg-shell.xml"
        "/usr/share/qt6/wayland/protocols/xdg-shell/xdg-shell.xml")
    if(EXISTS "${candidate}")
        set(QS_XDG_SHELL_XML "${candidate}")
        break()
    endif()
endforeach()
if(NOT QS_XDG_SHELL_XML)
    message(FATAL_ERROR
        "xdg-shell.xml was not found. Qt's Wayland development files install it, normally at
"
        "         /usr/share/qt6/wayland/protocols/xdg-shell/xdg-shell.xml,\n"
        "         and zwlr_layer_surface_v1's get_popup request makes it a link requirement.")
endif()
set(QS_XDG_SHELL_HEADER ${CMAKE_CURRENT_BINARY_DIR}/wayland-xdg-shell-client-protocol.h)
set(QS_XDG_SHELL_CODE ${CMAKE_CURRENT_BINARY_DIR}/wayland-xdg-shell-protocol.c)
# No VERBATIM, and for the reason Qt's own generator macro omits it: the redirection is meant for the
# shell that runs the command, and VERBATIM would quote the angle brackets into literal arguments.
add_custom_command(
    OUTPUT ${QS_XDG_SHELL_HEADER} ${QS_XDG_SHELL_CODE}
    COMMAND Wayland::Scanner client-header < "${QS_XDG_SHELL_XML}" > "${QS_XDG_SHELL_HEADER}"
    COMMAND Wayland::Scanner private-code < "${QS_XDG_SHELL_XML}" > "${QS_XDG_SHELL_CODE}"
    DEPENDS "${QS_XDG_SHELL_XML}" Wayland::Scanner)
target_sources(quantum-shell-layershell-protocol PRIVATE ${QS_XDG_SHELL_HEADER} ${QS_XDG_SHELL_CODE})

add_library(quantum-shell-wayland SHARED
    src/wayland/LayerShellIntegration.cpp
    src/wayland/LayerShellSurface.cpp
    src/wayland/LayerShellWindow.cpp)
target_include_directories(quantum-shell-wayland PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Public, not private: the generated header is included from this library's own public headers, so
# whatever includes them needs its directory on the include path too.
target_link_libraries(quantum-shell-wayland PUBLIC quantum-shell-layershell-protocol)
target_link_libraries(quantum-shell-wayland
    PUBLIC Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
    PRIVATE Qt6::WaylandClientPrivate quantum-shell-logging quantum-shell-warnings)

# The plugin Qt's Wayland platform plugin loads for QT_WAYLAND_SHELL_INTEGRATION=quantum-shell. Qt looks
# for a shell integration in the plugin directory named after the plugin type, so it is written into
# one; running from the build tree means pointing QT_PLUGIN_PATH at that directory.
add_library(quantum-shell-layer-shell MODULE
    src/wayland/LayerShellPlugin.cpp)
target_link_libraries(quantum-shell-layer-shell
    PRIVATE quantum-shell-wayland Qt6::Core Qt6::WaylandClientPrivate quantum-shell-warnings)
set_target_properties(quantum-shell-layer-shell PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/plugins/wayland-shell-integration)

# --- quantum-shell: the application ---------------------------------------------------------------
# The composition root plus the QML it loads. The QML is compiled into the executable's resources
# rather than shipped beside it, so the bar a running shell draws cannot be a different copy from the
# one that was built.
add_executable(quantum-shell
    src/app/main.cpp)
qt_add_resources(quantum-shell "qml"
    PREFIX "/"
    FILES
        qml/Main.qml
        qml/Bar.qml
        qml/Clock.qml
        qml/Workspaces.qml)
target_link_libraries(quantum-shell
    PRIVATE quantum-shell-capabilities quantum-shell-ipc quantum-shell-niri quantum-shell-config
            quantum-shell-wayland quantum-shell-warnings)
# The build's version, compiled in: `qsctl version`, the startup record and the IPC handshake all report it,
# and none of them should be quoting a copy of the number written out by hand.
target_compile_definitions(quantum-shell PRIVATE QS_VERSION="${PROJECT_VERSION}")

# --- tests ----------------------------------------------------------------------------------------
enable_testing()

# Black-box cases over the fixture trees: detection, exclusions, required paths, exit codes.
add_test(NAME qs-scan-self-test
    COMMAND ${CMAKE_COMMAND}
        -DSCANNER=$<TARGET_FILE:qs-scan>
        -DPATTERNS=${QS_SCAN_PATTERNS}
        -DFIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/scan/run_scan_tests.cmake)

# The gate itself: the repository is clean of everything qs-scan can decide. Extend --require with
# qml in the same change that creates it; a shipped path the gate does not read is not exempt.
add_test(NAME repo-scan
    COMMAND $<TARGET_FILE:qs-scan>
        --root ${CMAKE_CURRENT_SOURCE_DIR}
        --patterns ${QS_SCAN_PATTERNS}
        --require tools
        --require CMakeLists.txt
        --require src
        --require qml
        --require tests)

# Test targets live under tests/, including the build file that compiles a test double: they are
# allowed under tests/ and nowhere else, and that applies to the file that names them too.
add_subdirectory(tests)

# --- the project's public names --------------------------------------------------------------------
#
# tests/public_names.cmake declares the test names and environment variables this project presents to
# whoever runs it, and two checks read it. This one fails the configure when a test is registered without
# being declared, and it is registered as a *deferred* call so it runs at the end of configuration rather
# than here: an `add_test` appended after this line is exactly how a new test gets added, and a check
# written inline would be blind to the one registration most likely to exist. The other check,
# public-names-test, reads that file against the documents and the build files that use the names.
include(${CMAKE_CURRENT_SOURCE_DIR}/tests/public_names.cmake)
cmake_language(DEFER CALL qs_check_all_registered_tests_are_declared)

# --- developer entry points -----------------------------------------------------------------------
add_custom_target(scan
    COMMAND $<TARGET_FILE:qs-scan>
        --root ${CMAKE_CURRENT_SOURCE_DIR}
        --patterns ${QS_SCAN_PATTERNS}
        --require tools
        --require CMakeLists.txt
        --require src
        --require qml
        --require tests
        --list
    DEPENDS qs-scan
    USES_TERMINAL
    COMMENT "Scanning the repository for work that must not ship")

add_custom_target(check
    # Parallel for the same reason the test presets set execution.jobs: the shuffled slot-order check is
    # four shards, and ctest runs them one after another without it — correct either way, and about four
    # times the wall clock. It is safe to run tests together because the two that act on the desktop take
    # a resource lock, so they still wait for each other.
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --parallel 4
    DEPENDS qs-scan
    USES_TERMINAL
    COMMENT "Running every test, including the repository-wide scan")
