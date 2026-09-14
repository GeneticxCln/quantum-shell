# Quantum Shell — Plan

**Status:** Draft · **Last revised:** 2026-09-13 · **Target compositor:** niri ≥ 26.04 · **Baseline toolkit:** Qt 6.11 (Qt 6.12 LTS when released)

> This document is the single source of truth for the project. It is plain Markdown by design —
> if a rendered copy is needed, export it (`pandoc QUANTUM_SHELL.md -o QUANTUM_SHELL.pdf`);
> do not keep a generated copy in-tree.

**Quantum Shell** is a native Wayland desktop shell designed specifically for **niri**.

It combines a modern QML/Qt Quick interface with **Qt Quick 3D** for depth, lighting, spatial
widgets, parallax and 3D transitions. C++ is used only for infrastructure and system integration;
rendering, layout and visual behavior remain in QML.

---

## Contents

1. [Core Principles](#core-principles)
2. [Architecture](#architecture)
3. [Technology Stack](#technology-stack)
4. [C++ Responsibility](#c-responsibility)
5. [Project Layout](#project-layout)
6. [niri Integration](#niri-integration)
7. [Wayland Surfaces](#wayland-surfaces)
8. [Layer-Shell Implementation Strategy (Spike)](#layer-shell-implementation-strategy-spike)
9. [3D Architecture](#3d-architecture)
10. [Wallpaper](#wallpaper)
11. [Visual Effects](#visual-effects)
12. [Configuration](#configuration)
13. [IPC](#ipc)
14. [Plugin System](#plugin-system)
15. [Exposed QML API](#exposed-qml-api)
16. [Build Requirements](#build-requirements)
17. [Initial CMake Structure](#initial-cmake-structure)
18. [Quality Gates, Testing and CI](#quality-gates-testing-and-ci)
19. [Development Roadmap](#development-roadmap)
20. [First Implementation Target](#first-implementation-target)
21. [Risk Register](#risk-register)
22. [Project Identity](#project-identity)
23. [Non-Goals](#non-goals)

---

## Core Principles

- **niri-only** — no Hyprland, Sway, or compositor abstraction layer.
- **QML-first UI** — all visual components and layout live in QML.
- **Qt Quick 3D** — 3D is integrated into the Qt Quick scene rather than implemented as a separate legacy Qt 3D architecture.
- **C++ infrastructure only** — C++ provides system services, protocols, IPC, configuration, authentication and hardware/system integration.
- **Native Wayland** — shell surfaces are implemented as native Wayland layer-shell surfaces.
- **No rendering logic in C++.**
- **No layout logic in C++.**
- **niri is the desktop environment** — Quantum Shell integrates with niri rather than attempting to replace it.
- **Progressive cost** — anything expensive (3D scene, effects, live spectrum, animated wallpaper) is opt-in and must be able to fall back to a cheap 2D path.
- **Compositor does the compositing** — if niri can do it better and more cheaply (blur, shadows, rounded corners, scaling), we ask it to instead of re-implementing it in the shell.

---

## Architecture

```text
Quantum Shell
│
├── QML / Qt Quick
│   ├── Bar
│   ├── Dock
│   ├── Launcher
│   ├── Control Center
│   ├── Notifications
│   ├── OSD
│   ├── Lock Screen
│   ├── Desktop Widgets
│   ├── Settings
│   └── Effects
│
├── Qt Quick 3D
│   ├── 3D desktop widgets
│   ├── depth / elevation
│   ├── lighting
│   ├── perspective
│   ├── parallax
│   └── spatial transitions
│
├── niri Integration
│   ├── niri IPC (event stream + requests)
│   ├── workspaces
│   ├── windows
│   ├── focus
│   ├── outputs
│   └── keyboard / session integration
│
└── C++ Services
    ├── Wayland layer-shell
    ├── Wayland session-lock
    ├── ext-background-effect (compositor blur)
    ├── D-Bus
    ├── PipeWire
    ├── system monitoring
    ├── configuration
    ├── IPC
    ├── PAM / fingerprint
    └── process execution
```

---

## Technology Stack

| Layer | Technology | 2026 baseline |
| --- | --- | --- |
| UI | QML / Qt Quick | Qt 6.11.x (6.12 LTS once available) |
| 3D | Qt Quick 3D | Qt 6.11.x (`ExtendedSceneEnvironment`, SSGI, SSR, TAA) |
| Visual effects | Qt Quick `MultiEffect` + `ShaderEffect` (precompiled `.qsb`) | Qt 6.11.x |
| Text | Qt text engine (`Text`, `TextEdit`, `TextArea`) | Qt 6.11.x |
| Animations | Qt Quick `Behavior` / `SpringAnimation` / `Animator` | Qt 6.11.x |
| Wayland surfaces | `QQuickWindow` + native layer-shell integration | wayland 1.24+ client, wayland-protocols 1.45+ |
| Compositor | **niri** | 26.04 or newer |
| niri integration | niri IPC (JSON event stream + `niri msg` requests) | niri 26.04 |
| D-Bus | Qt D-Bus | Qt 6.11.x |
| Audio | PipeWire C API + Qt signals | PipeWire 1.6.8+, WirePlumber 0.5.17+ |
| Configuration | TOML (`toml++` 3.4) → `QObject` `Q_PROPERTY` → QML bindings | toml++ 3.4.0 |
| IPC | `QLocalServer` / `QLocalSocket` (abstract namespace) | Qt 6.11.x |
| Plugins | Qt QML module system | Qt 6.11.x |
| Build | CMake | 3.31 minimum, 4.4.x tested |
| Language | C++23 (C++26 opt-in) + QML | GCC 16 / Clang 21 |
| Editor/IDE assist | `qmllint`, `qmlformat`, `qmlcachegen`, `qmlls` | Qt 6.11.x |
| Tracing | `perf`, `sysprof`, Qt `QSG_INFO`/`QSG_RENDER_TIMING` | — |
| Packaging | Nix flake, Arch/AUR PKGBUILD, distro tarball | — |

### Version policy

- **Qt floor:** 6.11.0. The 3D layer depends on 6.11-era `ExtendedSceneEnvironment` features
  (screen-space global illumination, screen-space reflections, motion-vector-based TAA) and on the
  6.10+ accessibility and layout improvements. Do not target 6.8 LTS: it would cost the visual
  identity of the project.
- **Qt ceiling:** track the newest stable minor. Qt 6.12 is scheduled for **October 2026** and is
  expected to be the next **LTS**; when it ships, move the floor to 6.12 and treat 6.11 as
  best-effort. Pin an exact minor in CI and in packaging, never `>=6` unconstrained.
- **niri floor:** 26.04. This release is the first with `ext-background-effect` (window **and**
  layer-surface blur), which the shell relies on for frosted surfaces. Detect the niri version at
  startup and degrade gracefully (client-side `MultiEffect` blur) on older builds.
- **C++ standard:** **C++23** is the baseline — it is fully implemented by GCC 16 / Clang 21 and is
  the safest choice for a project that must build on distro toolchains. The standard-revision
  baseline is available behind a `QUANTUM_SHELL_CXX26` option that switches to **C++26** (finalized
  in 2026, most features in GCC 16.1+). C++26 is *not* a dependency of any design in this document;
  it is a build-time opt-in for experimentation (e.g. contracts, `std::execution`).
- **Wayland protocol pins:** `wayland-protocols` **staging** is required (`ext-background-effect-v1`,
  `ext-session-lock-v1`, `ext-idle-notify-v1`, `ext-data-control-v1`, `ext-foreign-toplevel-list-v1`,
  `fractional-scale-v1`, `text-input-v3`, `ext-image-copy-capture-v1`). Vendor the XML in
  `protocols/` and generate code with `wayland-scanner`, so a build never depends on the host's
  protocol package version.
- **Header-only deps** (`toml++`) are vendored via CMake `FetchContent` with a pinned tag *and* an
  `-DQUANTUM_SHELL_SYSTEM_DEPS=ON` path that uses `find_package` for distro builds.

---

## C++ Responsibility

C++ is strictly the infrastructure layer.

**C++ handles:**

- Wayland protocol bindings and generated protocol code
- layer-shell surfaces
- session-lock surface
- `ext-background-effect` (requesting compositor blur)
- niri IPC (socket, event stream parsing, request encoding)
- D-Bus services and watchers
- PipeWire / WirePlumber
- system statistics
- configuration parsing
- configuration watching and diffing
- PAM
- fingerprint authentication
- process execution
- brightness
- power management
- network / system services
- local IPC server and client
- plugin discovery and loading
- persistent storage
- clipboard history storage (data-control client)
- idle notification (`ext-idle-notify-v1`) and logind inhibit handling

**C++ does not handle:**

- visual layout
- widget positioning
- animations
- colors
- visual styling
- UI state presentation
- rendering
- 3D scene composition

QML owns those responsibilities. A useful review rule: *if a change touches pixels or geometry, it
does not belong in `src/`.*

---

## Project Layout

```text
QuantumShell/
│
├── src/
│   ├── main.cpp
│   │
│   ├── app/                     # app bootstrap, single instance, crash handler
│   │   ├── Application.h/.cpp
│   │   ├── CrashHandler.h/.cpp
│   │   └── Logging.h/.cpp
│   │
│   ├── wayland/
│   │   ├── WaylandConnection.h/.cpp     # wl_display, roundtrip, registry
│   │   ├── LayerSurface.h/.cpp
│   │   ├── SessionLock.h/.cpp
│   │   ├── BackgroundEffect.h/.cpp      # ext-background-effect requests
│   │   ├── Clipboard.h/.cpp             # ext-data-control-v1
│   │   ├── IdleNotifier.h/.cpp          # ext-idle-notify-v1
│   │   ├── FractionalScale.h/.cpp       # fractional-scale-v1 + viewporter
│   │   ├── TextInput.h/.cpp             # text-input-v3 (lock screen, launcher)
│   │   ├── Screencopy.h/.cpp            # ext-image-copy-capture / screencopy
│   │   └── protocols/                   # vendored XML + generated code
│   │
│   ├── niri/
│   │   ├── NiriProtocol.h/.cpp          # the shared wire format: socket path, lines, replies, events
│   │   ├── NiriIPC.h/.cpp               # socket, framing, request/response
│   │   ├── NiriEventStream.h/.cpp       # long-lived JSON event stream
│   │   ├── NiriState.h/.cpp             # workspaces, windows, outputs, layouts, overview
│   │   ├── NiriOutputs.h/.cpp           # keeps outputs in step: niri streams no output event
│   │   ├── NiriVersion.h/.cpp           # version detection + capability probes
│   │   ├── NiriWorkspace.h/.cpp
│   │   ├── NiriWindow.h/.cpp
│   │   ├── NiriOutput.h/.cpp            # output, modes, logical size and fractional scale
│   │   ├── NiriKeyboardLayouts.h/.cpp
│   │   ├── NiriActions.h/.cpp           # focus/workspace/window/screenshot actions
│   │   ├── NiriReconnect.h/.cpp         # keeps both connections attached across a restart
│   │   └── NiriService.h/.cpp           # the QObject service QML binds to
│   │
│   ├── config/
│   │   ├── Config.h/.cpp                 # the QObject tree QML binds to as Config
│   │   ├── ConfigWatcher.h/.cpp          # watching, off-thread parse, apply
│   │   └── ConfigSchema.h/.cpp           # the keys, their defaults, validation and schema_version
│   │
│   ├── dbus/
│   │   ├── NotificationService.h/.cpp
│   │   ├── MPRISWatcher.h/.cpp
│   │   ├── NetworkManager.h/.cpp
│   │   ├── UPower.h/.cpp
│   │   ├── BlueZ.h/.cpp
│   │   ├── Logind.h/.cpp
│   │   ├── Polkit.h/.cpp
│   │   └── SecretService.h/.cpp
│   │
│   ├── audio/
│   │   ├── PipeWireService.h/.cpp
│   │   ├── SpectrumAnalyzer.h/.cpp
│   │   └── AudioDevices.h/.cpp          # sinks/sources, profiles, default switching
│   │
│   ├── system/
│   │   ├── SysMonService.h/.cpp
│   │   ├── BrightnessService.h/.cpp
│   │   ├── WeatherService.h/.cpp
│   │   └── StorageService.h/.cpp
│   │
│   ├── auth/
│   │   ├── PamAuth.h/.cpp
│   │   ├── FingerprintAuth.h/.cpp
│   │   └── AuthCoordinator.h/.cpp       # rate limiting, backoff, timeouts
│   │
│   ├── ipc/
│   │   ├── IPCServer.h/.cpp
│   │   ├── IPCClient.h/.cpp
│   │   ├── IPCProtocol.h                # request/response types + version
│   │   └── qsctl/                       # CLI frontend (own binary target)
│   │
│   ├── theme/
│   │   ├── ThemeEngine.h/.cpp
│   │   └── TemplateEngine.h/.cpp
│   │
│   ├── plugins/
│   │   ├── PluginLoader.h/.cpp
│   │   ├── PluginRegistry.h/.cpp
│   │   └── PluginManifest.h/.cpp        # manifest.json parsing + validation
│   │
│   └── util/
│       ├── ProcessHelper.h/.cpp
│       └── NetHelper.h/.cpp
│
├── qml/
│   ├── shell.qml
│   │
│   ├── surfaces/
│   │   ├── BarWindow.qml
│   │   ├── DockWindow.qml
│   │   ├── WallpaperWindow.qml
│   │   ├── LockWindow.qml
│   │   └── NotifWindow.qml
│   │
│   ├── bar/
│   │   ├── Bar.qml
│   │   ├── CapsuleGroup.qml
│   │   ├── WidgetSlot.qml
│   │   └── widgets/
│   │       ├── Clock.qml
│   │       ├── Workspaces.qml
│   │       ├── Taskbar.qml
│   │       ├── Volume.qml
│   │       ├── Network.qml
│   │       ├── Battery.qml
│   │       ├── Media.qml
│   │       ├── Weather.qml
│   │       ├── Clipboard.qml
│   │       ├── SystemMonitor.qml
│   │       ├── AudioVisualizer.qml
│   │       ├── Screenshot.qml
│   │       ├── Text.qml
│   │       └── ExternalIP.qml
│   │
│   ├── dock/
│   │   ├── Dock.qml
│   │   └── DockItem.qml
│   │
│   ├── launcher/
│   │   ├── Launcher.qml
│   │   └── providers/
│   │       ├── AppProvider.qml
│   │       ├── EmojiProvider.qml
│   │       ├── CalcProvider.qml
│   │       └── WindowProvider.qml
│   │
│   ├── notification/
│   │   ├── Toast.qml
│   │   ├── ToastStack.qml
│   │   └── History.qml
│   │
│   ├── osd/
│   │   ├── OSDCard.qml
│   │   ├── VolumeOSD.qml
│   │   ├── BrightnessOSD.qml
│   │   ├── KeyboardOSD.qml
│   │   └── MediaOSD.qml
│   │
│   ├── lockscreen/
│   │   ├── LockScreen.qml
│   │   ├── CompactLayout.qml
│   │   ├── RegularLayout.qml
│   │   ├── PasswordInput.qml
│   │   └── LockBlur.qml
│   │
│   ├── desktop/
│   │   ├── DesktopHost.qml
│   │   ├── DesktopWidgetEditor.qml
│   │   └── widgets/
│   │       ├── DesktopClock.qml
│   │       ├── DesktopCalendar.qml
│   │       ├── DesktopMedia.qml
│   │       ├── DesktopSysMon.qml
│   │       └── DesktopVolume.qml
│   │
│   ├── wallpaper/
│   │   ├── WallpaperLayer.qml
│   │   └── WallpaperPicker.qml
│   │
│   ├── control_center/
│   │   ├── ControlCenter.qml
│   │   └── tabs/
│   │       ├── HomeTab.qml
│   │       ├── NetworkTab.qml
│   │       ├── BluetoothTab.qml
│   │       ├── AudioTab.qml
│   │       ├── DisplayTab.qml
│   │       └── PowerTab.qml
│   │
│   ├── session/
│   │   └── SessionPanel.qml
│   │
│   ├── clipboard/
│   │   └── ClipboardHistory.qml
│   │
│   ├── settings/
│   │   ├── Settings.qml
│   │   └── controls/
│   │       ├── NToggle.qml
│   │       ├── NSlider.qml
│   │       ├── NSelect.qml
│   │       ├── NInput.qml
│   │       ├── NButton.qml
│   │       ├── NColorPicker.qml
│   │       └── NKeyBind.qml
│   │
│   ├── theme/
│   │   ├── Theme.qml
│   │   └── Palette.qml
│   │
│   ├── effects/
│   │   ├── FrostedGlass.qml
│   │   ├── Glow.qml
│   │   ├── Ripple.qml
│   │   └── BarBlur.qml
│   │
│   ├── components/
│   │   ├── Panel.qml
│   │   ├── Tooltip.qml
│   │   ├── ContextMenu.qml
│   │   ├── Scrollable.qml
│   │   └── GestureArea.qml
│   │
│   └── 3d/
│       ├── QuantumScene.qml
│       ├── Desktop3D.qml
│       ├── Widget3D.qml
│       ├── ParallaxWallpaper.qml
│       └── transitions/
│
├── assets/
│   ├── fonts/
│   ├── sounds/
│   ├── templates/
│   └── translations/            # .ts sources; .qm generated at build time
│
├── protocols/                   # vendored protocol XML (source of generated code)
├── plugins/                     # in-tree example plugins
├── tools/
│   └── qs-scan/                 # the repository real-code gate: scanner + pattern table
├── tests/
│   ├── scan/                    # black-box cases for qs-scan
│   ├── fixtures/                # input trees for those cases; never compiled
│   ├── unit/                    # QTest / Catch2 for C++ services
│   ├── qml/                     # QML test cases (qmltestrunner)
│   └── integration/             # tests against a niri started for the test
│                                # (niri has no headless mode, so it is nested in a session)
├── packaging/
│   ├── nix/                     # flake.nix
│   └── arch/                    # PKGBUILD
├── CMakeLists.txt
├── CMakePresets.json
└── README.md
```

---

## niri Integration

Quantum Shell targets niri exclusively. All niri-specific functionality is isolated in `src/niri/`.

The shell communicates with niri through its **IPC socket** in two directions:

**Events (niri → shell):** one long-lived connection to the `EventStream` request, which streams one
JSON object per line, each named by a variant of niri-ipc v26.04's `Event` enum. The shell models
`WorkspacesChanged`, `WorkspaceUrgencyChanged`, `WorkspaceActivated`,
`WorkspaceActiveWindowChanged`, `WindowsChanged`, `WindowOpenedOrChanged`, `WindowClosed`,
`WindowFocusChanged`, `WindowUrgencyChanged`, `KeyboardLayoutsChanged`, `KeyboardLayoutSwitched`,
`OverviewOpenedOrClosed` and `ConfigLoaded`; the rest are reported by name rather than dropped. This
is the **primary** source of shell state: it is push-based, so the shell never polls.

Note what is deliberately absent from that list, and from any list: an outputs-changed event. niri-ipc
v26.04's `Event` enum has no output variant and the event-stream state has no outputs part, so a
subscription is answered with workspaces, windows, keyboard layouts, overview, config and casts and
nothing else — outputs are only ever available from the `Outputs` request (see `NiriOutputs` below).

**Requests (shell → niri):** JSON-line requests over the socket niri names for the session —
`$NIRI_SOCKET`, rediscovered by niri's own naming rule once that path is gone (see the restart bullet
below) — for reads such as `Workspaces`,
`Windows`, `Outputs` and `FocusedWindow`, and for actions such as `FocusWorkspace`, `Spawn`,
`MoveWindowToWorkspace` and `ScreenshotScreen`. They are spelled as the variants of niri-ipc v26.04's
`Request` enum: the kebab-case names in `niri msg --help` are CLI subcommands, and sending one of
those on the wire gets an error, not an action. A request that is a unit variant goes out as the bare
JSON string niri expects; a request that carries fields — every action — is serialised by the client
in compact form (`NiriIPC::sendObject`) rather than assembled by string concatenation, so one value
cannot put a second line on the connection and shift which reply belongs to which request.

Design rules:

- **Event-driven, never polling.** Timers are a bug. Any widget that needs to refresh is driven by
  an event or by its own service signal.
- **Two connections, one parser each.** niri stops reading requests once an `EventStream` request has
  been sent on a socket (niri-ipc v26.04), so the event stream and requests cannot share one:
  `NiriEventStream` owns the stream connection and `NiriIPC` owns the request connection. Widgets
  never talk to a socket, only to the typed signals of these two.
- **A restart is recovered from, not waited out.** niri is a session process, not a daemon, so when it
  restarts every connection to it is gone at once and the shell has to reattach on its own. Two facts
  shape how. Its IPC socket is named `niri.<wayland socket name>.<pid>.sock` and unlinked on a clean
  exit (niri v26.04 `IpcServer::start` and `socket_dir()` in `src/ipc/server.rs`), so the path a shell
  was using before a restart is not the path after it — `$NIRI_SOCKET` is trusted only while that path
  exists, and otherwise the socket is rediscovered by niri's naming rule. And a subscription is
  answered with the whole state, so reattaching rebuilds the model: `NiriState` and `NiriOutputs`
  clear on a disconnect rather than keep describing a compositor that is gone, and the burst that
  follows the new subscription repopulates them. `NiriReconnect` owns both halves — the retry policy
  (exponential backoff with jitter, capped) and finding the socket on each attempt — and re-asks the
  version on every attach, because the compositor that came back may be a different build.
- **One state model, fed by events.** `NiriState` owns the workspaces, windows, keyboard layouts,
  overview state and outputs, and QML binds to it rather than to a socket. Everything except the
  outputs is updated only by `NiriEventStream`'s typed signals; the outputs come from `NiriOutputs`,
  which is the only other writer. The events that are more than a field update follow niri's own
  rules: `WorkspacesChanged` and `WindowsChanged` replace the whole set, an opening or changed window
  that is focused unfocuses the others, and activating a workspace deactivates the others on its
  output. A change signal fires only when a value actually changed, so an event carrying no news does
  not wake the bindings above it.
- **Nothing is silently dropped.** An event of this protocol version that the build does not act on
  is reported by name; an event name absent from niri-ipc v26.04 is reported separately as protocol
  drift; an event whose payload cannot be read is reported as malformed. All three are signals,
  because a silent no-op is indistinguishable from a compositor that sent nothing.
- **Outputs are requested, never polled.** `NiriOutputs` asks for the `Outputs` request once and then
  again only on events that can carry news of the output set — a `WorkspacesChanged` whose set of
  referenced output names changed (a workspace names its output, so a monitor appearing or going away
  shows up there), and a `ConfigLoaded` (output configuration lives in the config file, so a reload
  is when scale, mode and position may have moved). Requests are coalesced: at most one in flight,
  and a trigger arriving during one causes exactly one more. The gap is stated rather than hidden: an
  output change that fires neither event — a transient `niri msg output` change, or a monitor that
  plugs in without gaining a workspace — is not noticed until one of them does.
- **Capability detection at startup.** `NiriVersion` records the niri version, and `NiriIPC` probes the
  read-only requests the shell depends on: niri answers a request it does not know with
  `{"Err":"error parsing request"}`, so support is asked for rather than assumed. Version-gated
  features are decided by the floor instead (`ext-background-effect` needs niri ≥ 26.04). A feature
  nothing has answered for stays *unknown* — never enabled by default.
- **No compositor abstraction.** There is deliberately no `CompositorBackend`, `HyprlandBackend` or
  `SwayBackend` interface. Keeping one compositor lets the shell use niri-specific behavior
  (scrollable workspaces, `niri msg` actions, layer rules) instead of a lowest-common-denominator
  API.
- **Optional standardization layers** (`ext-workspace-v1`, `wlr-foreign-toplevel-management`) may be
  consumed as an *additional* state source where niri exposes them, but must never become a
  requirement — the niri IPC stream is authoritative.

---

## Wayland Surfaces

Quantum Shell uses native Wayland layer-shell surfaces for shell components.

Typical surfaces:

| Surface | Layer | Keyboard interactivity | Namespace |
| --- | --- | --- | --- |
| Bar | top | none | `quantum-shell-bar` |
| Dock | top | none | `quantum-shell-dock` |
| Wallpaper | background | none | `quantum-shell-wallpaper` |
| Notifications | overlay | on-demand | `quantum-shell-notification` |
| OSD | overlay | none | `quantum-shell-osd` |
| Control Center | overlay | on-demand | `quantum-shell-control-center` |
| Launcher | overlay | exclusive | `quantum-shell-launcher` |
| Lock surface | session-lock (not layer-shell) | exclusive | `quantum-shell-lock` |

Stable **namespaces** are part of the public interface: users write niri `layer-rule` blocks against
them (blur, shadow, size, placement), so namespaces must be documented and never renamed casually.

Each surface is a QML window backed by the C++ Wayland integration layer. The C++ layer exposes
Wayland functionality to QML without taking ownership of visual layout — QML decides geometry,
anchors, margins and visibility; C++ pushes those to the protocol objects as properties.

Additional security/robustness rules:

- The lock surface is created **only** through `ext-session-lock-v1`, on a dedicated output-aware
  path. Nothing else may draw while the session is locked.
- `QGuiApplication` must exit cleanly when the Wayland connection drops (compositor restart) and
  reconnect on a new session.
- Scale handling uses `fractional-scale-v1` + `wp-viewporter`; never round fractional scale to an
  integer, and never assume scale 1.

---

## Layer-Shell Implementation Strategy (Spike)

This is the single highest-risk piece of Phase 0 and deserves an explicit spike before anything else
is built. Qt does **not** ship a public layer-shell client API in QtWaylandClient, so one of these
three routes has to be chosen deliberately:

1. **Own protocol bindings + QtWaylandClient private API** *(preferred)* — create the `wl_surface`
   through Qt's Wayland platform plugin and attach the `zwlr_layer_surface_v1` role using private
   QtWaylandClient headers (`QWaylandWindow`). Full control, no KDE dependency; requires a
   compile-time version guard per Qt minor and an isolated compatibility shim
   (`src/wayland/qtcompat/`).
2. **Vendor `layer-shell-qt` (or the no-KDE fork)** — proven code, but drags in KDE libraries and
   cuts against the "no abstraction layer" principle.
3. **Separate `wl_surface` + custom QPA/embedded window** — most control, most work; only worth it if
   routes 1 and 2 both fail on Qt 6.11/6.12.

**Decided: Route 1**, chosen by the user. Built in `src/wayland/`. The deliverable is met: a bar
renders on niri as a real layer surface, with anchors, exclusive zone, keyboard interactivity and
namespace all set from QML.

**Status: built and verified by running it, with no automated test.** `niri msg layers` reports the
bar on the top layer as `quantum-shell-bar`, keyboard interactivity `none`. The wire shows
`get_layer_surface(surface, nil, 2, "quantum-shell-bar")`, `set_size(0, 32)`, `set_anchor(13)`,
`set_exclusive_zone(32)`, and then `configure(1641556, 3072, 32)` acknowledged. A screenshot puts the
bar in a 40-device-pixel band (32 logical, at the output's 1.25 scale) with exactly two groups of ink:
an accent-filled workspace strip at the left margin and the clock at the right margin, three capsules
for the three workspaces niri reported.

Three things about how it is built are worth recording, because each was a build failure first:

- **QWaylandShellIntegrationTemplate is CRTP.** It binds the registry global by casting itself to its
template argument, so that argument must be the integration class itself; naming the generated
protocol class there does not compile. The generated class is inherited alongside it.
- **Two handshake steps are not optional.** `isExposed()` must be false until the compositor's initial
configure has been acknowledged — Qt paints, and therefore attaches a buffer, only while the window
reports itself exposed, and attaching before that acknowledgement is answered with
`must ack the initial configure before attaching buffer`. And once the window becomes exposed Qt must
be told, via `QWaylandWindow::updateExposure()`: `isExposed()` is read when the window is mapped and
the answer is remembered, so without that call the surface appears in the layer list and paints
nothing at all.
- **The protocol XML needed one rename.** `zwlr_layer_shell_v1.get_layer_surface` names an argument
`namespace`, which is a C++ keyword, and wayland-scanner copies it into both generated headers. The
vendored file therefore renames that one argument to `namespace_`. The wire format is unaffected:
what a protocol encodes is an argument's type and position, not its name.

Two costs of the route, stated rather than discovered later. The build needs QtWaylandClient's
**private** headers, so a Qt minor that moves them is a compile error here; that is the accepted price
of not being able to create a layer surface any other way, and the include is wrapped in one place
(`src/wayland/LayerShellProtocol.h`) so the surface area of private API is one header. And **every
window this shell creates is a layer surface** — the integration returns none for a window that is not
a `LayerShellWindow`. A settings window, when there is one, needs a toplevel path that does not exist
yet.

**Covered now by `niri-live-layershell-test`**, an opt-in live test that starts the built shell and
checks the surface it produces against two independent sources, because neither alone is enough. The
compositor's own layer list — a `Layers` request — reports the namespace, the output, the layer and the
keyboard interactivity, which is what proves the surface was accepted rather than merely sent. It
reports **nothing** about anchors or the exclusive zone, and no request reads them back, so those two
are read from the client's protocol traffic with `WAYLAND_DEBUG=1`: the test asserts `get_layer_surface`
with layer 2 and the namespace, `set_size(0, 32)`, `set_anchor(13)`, `set_exclusive_zone(32)` and
`set_keyboard_interactivity(0)`, and then the invariant this route got wrong first — that the initial
configure is acknowledged before any buffer is attached. A test reading only the wire would pass against
a compositor that refused the surface; a test reading only the list could not tell a correct exclusive
zone from a wrong one.

It maps a bar on screen, so it takes the same consent as the other acting test — `QS_NIRI_SESSION_TESTS`
as well as `$NIRI_SOCKET` — and the same `niri-desktop` resource lock. Every expectation is a literal
mirroring `qml/Main.qml` rather than a value read from it, so changing the bar's namespace, anchors or
reserved height there fails the test here.

**Writing it found a real defect**, which is the argument for the second source. Removing the right-hand
anchor from `qml/Main.qml` made the bar fail to appear at all, rather than failing the anchor assertion:
the surface asked for `set_size(0, 32)` and niri answered
`width 0 requested without setting left and right anchors`. Zero is legal on an axis only when that axis
is stretched, and the non-stretched branch was proposing the *window's* width, which is still zero while
the role is being assigned because a QML width bound to the screen has not necessarily resolved by then.
Fixed: a non-stretched axis with no size declared falls back to the output's own size — a real reading
rather than a guess — so the same configuration now proposes `set_size(3072, 32)` and maps. The shipped
bar is stretched horizontally and is unaffected, which is why nothing had caught it.

---

## 3D Architecture

Quantum Shell uses **Qt Quick 3D** for its 3D visual layer. The goal is not to turn the entire
desktop into a traditional 3D environment — it is a conventional 2D shell with a selective,
composited 3D spatial layer.

```text
Qt Quick Scene
│
├── 2D QML
│   ├── Bar
│   ├── Panels
│   ├── Widgets
│   └── Controls
│
└── Qt Quick 3D
    ├── Desktop objects
    ├── Spatial widgets
    ├── Depth
    ├── Lighting
    ├── Camera movement
    └── 3D transitions
```

Qt 6.11 features the 3D layer should exploit (`ExtendedSceneEnvironment`):

- **Screen-space global illumination (SSGI)** — grounded contact shadows for floating widgets.
- **Screen-space reflections (SSR)** — the "glass over glass" look of frosted panels.
- **Motion vectors + TAA** — stable edges during camera/parallax motion without MSAA cost.
- **Custom render passes** — post-processing (bloom, chromatic edges, glitch) applied to the 3D
  layer only, keeping the 2D UI crisp.

Non-negotiable constraints:

- **3D is a tier, not the default.** `Config.three_d.enabled` selects the path; with 3D off, the
  desktop renders with plain 2D QML and the shell must look intentional, not broken.
- **Frame budget:** ≤ 8 ms GPU on integrated graphics at 1920×1080@60 for the always-on paths
  (wallpaper + bar). 3D widget scenes are allowed to exceed this only while actively animating.
- **No continuous animation when idle.** Desktop 3D views are `renderMode: Offscreen`/paused when
  nothing changes; parallax reacts to pointer input, not to a frame timer.
- **Power first.** Every 3D feature ships with a measured idle-CPU/GPU number recorded in the PR;
  features without numbers are not merged.

### 3D Desktop Widgets

Desktop widgets can have elevation, tilt, rotation, depth, lighting, perspective, hover animation,
spatial selection and world-space positioning. The widget editor exposes these properties.

```text
DesktopWidget
├── position
├── scale
├── rotation
├── elevation
├── tilt
├── lighting
└── interaction
```

The UI controls remain QML; the 3D scene remains QML/Qt Quick 3D. Persisted widget state stays in
the shell's config/storage layer as plain data — never as serialized QML objects.

---

## Wallpaper

Quantum Shell supports both normal and 3D-assisted wallpaper.

Normal mode:

```qml
Image {
    anchors.fill: parent
}
```

Parallax mode (2D planes in a 3D scene, or a layer-moved 2D image — whichever measures cheaper):

```text
Camera
   │
   ├── foreground plane
   └── background plane
```

Cursor movement drives camera/plane displacement to produce the parallax effect.

Configuration:

```toml
[wallpaper]
mode = "parallax"      # "static" | "parallax" | "video"
parallax = true
strength = 0.15
fps_cap = 60
pause_on_battery = true
```

When disabled or when the shell is on battery with `pause_on_battery = true`, the shell falls back
to a normal image layer. Wallpaper rendering lives on the background layer-shell surface so it never
competes with the bar for compositing.

---

## Visual Effects

Effects are implemented in QML, using:

- Qt Quick `MultiEffect` (blur, shadow, mask, colorize)
- `ShaderEffect` with precompiled `.qsb` shaders
- blur / glow / ripple / color manipulation / transitions / depth-based effects

**Blur policy.** On niri ≥ 26.04, background blur is requested from the compositor via
`ext-background-effect` (`BackgroundEffect` wrapper in C++, property toggled from QML). This is
strictly cheaper and more correct than client-side blur, because niri can blur what is *behind* the
surface. `MultiEffect` blur remains the fallback for older niri or for off-screen/3D content.
`FrostedGlass.qml` abstracts the choice so widgets do not care which path is active.

C++ does not implement visual effects.

---

## Configuration

```text
TOML
  ↓
toml++ 3.4
  ↓
validated QObject property tree
  ↓
QML bindings
```

Example — and this one is the real shape of `qml/Main.qml` today, not an aspiration:

```qml
LayerShellWindow {
    layerNamespace: Config.bar.layerNamespace
    height: Config.bar.height
    exclusiveZone: Config.bar.height
}
```

Rules:

- **Schema versioning.** Every config file carries `schema_version`; unknown keys warn, missing keys
  fall back to defaults, and migrations are pure functions in `ConfigSchema`. A file from a *newer*
  schema than this build knows is refused as a whole rather than partly understood — the version has to
  equal the build's or nothing in the file is applied — and a file that omits the key is assumed to be
  the current version with a warning, because that is what a file written before the field existed is.
  Migration *functions* do not exist yet and that is deliberate: `1` is the only version that has ever
  been written, so a migration path today would be a branch nothing can take. They arrive with the
  second version, beside `parseConfig`.
- **Where the defaults live.** In the schema's own value initialisers (`BarConfig::height = 32`), not in
  an embedded default TOML file. The parse starts from those and overwrites what the file supplies,
  which is the same path a missing key takes, so there is exactly one place a default is written down
  and no second copy for it to drift from. `ConfigDefaults.cpp` in the tree below therefore does not
  exist; when a `qsctl config dump` can print the defaults for a user to copy, the text will be
  generated from the schema rather than stored beside it.
- **Include support.** `include = ["~/.config/quantum-shell/themes/foo.toml"]` with cycle detection.
  Not yet: no key reads it, and a key exists only when code reads it.
- **Defaults ship embedded**, so a missing config file is not an error. An absent file is not even worth
  a warning; a file that exists and carries no `schema_version` is, because it is a file whose author
  meant to configure something.
- **The keys that exist today**, and nothing else: `schema_version`, `bar.height` (logical pixels, at
  least 1, also the exclusive zone reserved from the tiling area) and `bar.namespace` (which must begin
  with `quantum-shell-`, the frozen prefix of AGENTS.md). The file is
  `$XDG_CONFIG_HOME/quantum-shell/config.toml` — `~/.config/quantum-shell/config.toml` by default — and
  `qml/Main.qml` binds its `height` and `exclusiveZone` to `bar.height` and its `layerNamespace` to
  `bar.namespace`.
- **A namespace is read once.** `zwlr_layer_surface_v1`'s namespace is an argument to
  `get_layer_surface`, and a surface may be given a role once, so a change to `bar.namespace` applies to
  the next start and the layer surface says so when a running shell is edited. Every other key is live:
  a change to `bar.height` resizes the surface that is on screen.
- **`ConfigWatcher` diffs** and only changed properties emit signals:

```text
config.toml changed
       ↓
ConfigWatcher (inotify on file + parent dir, to survive atomic replaces)
       ↓
parse + validate on a worker thread
       ↓
diff against previous tree
       ↓
Config.bar.heightChanged()
       ↓
QML updates automatically
```

- **No full reload on partial change.** Reloading the QML engine on a config edit is forbidden; it
  destroys state and is the classic source of shell flicker.

---

## IPC

Quantum Shell exposes a local IPC interface using `QLocalServer` / `QLocalSocket`. **Landed**, in
`src/ipc/`: the protocol and the server (`IPCProtocol`, `IPCServer`), the client's command-line logic
(`QsctlCli`) and `qsctl` itself as its own binary target.

Socket name: `\0quantum-shell` (abstract Unix socket — no filesystem entry, therefore no stale
socket file after a crash or restart). One detail of that name is a measured trap rather than a
footnote: Qt adds the abstract namespace's leading NUL itself when `AbstractNamespaceOption` is set, on
both ends, so the spelling handed to Qt is `quantum-shell` and the address bound is `\0quantum-shell`.
Passing a name that already has the NUL in it binds `@@quantum-shell` — a *different* socket, which
works perfectly and is connected to by nothing, since the client asking for the frozen name is refused
while the shell believes it is listening. So the name is verified where it exists: the live test reads it
back out of `/proc/net/unix` with the shell running, under the shell's own pid, and the same run shows it
gone once the shell exits.

Protocol: newline-delimited JSON, one request line per one response line, with every frame carrying the
protocol revision — `{"version": 1}` alone is the handshake, and is answered exactly as the `version`
verb is, so a client that knows how to send one frame still finds out it is talking to a shell that
speaks a different protocol. A frame whose version is not this build's is refused with both versions
named and the connection closed; a frame that is not a JSON object, or a line longer than 64 KiB, is
refused and closed too; an unknown verb, a missing argument or an unknown configuration key is refused
and the connection stays open, because a client that sends one bad request is entitled to send a good one
next. Nothing about a connection blocks the shell: one line in, one line out, answered from the event
loop.

The verbs exist only where a handler answers them, and they are declared once in `src/ipc/IPCProtocol.h`
(the guard for that list is § Frozen public names above):

| Verb | Answers with | Why it is real today |
| --- | --- | --- |
| `version` | `name`, `shell`, `protocol` | it is the shell's own build version, the same one its startup record carries |
| `state` | the same six values `NiriService` exposes: `workspaces`, `focusedWindow`, `outputs`, `keyboardLayout`, `overviewOpen`, `connected` | read from the service itself, so the keys are its property names and cannot be a second mapping |
| `config get <path>` | `path` and `value` | the schema resolves it; `bar.height` and `bar.layerNamespace` are the paths that exist |
| `bar toggle` | `visible` | the bar window's own visibility, and the compositor's layer list loses and regains the surface |

`qsctl` prints one line of JSON for `version`, `state` and `bar toggle`, and the bare value for
`config get` — that verb exists to be used as `x=$(qsctl config get bar.height)`, and quoting a number for
a JSON document would wrap the answer it was asked for. Its exit codes are interface, because a script
branches on them: `0` answered, `1` refused, `2` a command line it does not take, `3` no shell listening,
`4` a protocol mismatch. A verb this build has no handler for is refused as a *usage* error at the client,
not sent to the shell to be refused there — `qsctl volume up` is a command this document mentions and
there is no audio service behind it, so the honest answer is that qsctl does not take it rather than a
request the shell receives and declines, which would read as "the shell is broken".

**`subscribe`** — the event feed for external widgets — is designed here and has no handler, so the shell
refuses it as an unknown verb, naming the four it does answer. It is left out rather than declared and
answered by nothing, for the reason SYSTEM_PROMPT.md § Anti-Evasion Rules gives: a verb exists only when a
handler answers it and a test drives it, so this one joins that list in the change that implements it.

Security: the socket is per-user (abstract namespace is user-scoped by the kernel) and exposes only
the same `Shell.*` capabilities as the QML API — never arbitrary C++ entry points. That is enforced by
shape rather than by care: `IPCServer` cannot reach anything except a `Capabilities` object, and the
shell's implementation of it (`src/app/ShellCapabilities.h`) has three members — the QML service, the
configuration tree and a weak pointer to the bar window. A verb that reached further would have to be a
new method there, which is a visible change rather than a new case in a switch.

---

## Logging

**Landed**, in `src/app/Logging.h` / `Logging.cpp`. Five categories, declared once, because a category
name is interface twice over: a person filters with `QT_LOGGING_RULES="quantum.shell.ipc.debug=false"`
and a bug report quotes it. `quantum.shell` is the shell's lifecycle, and `quantum.shell.niri`,
`.config`, `.ipc` and `.wayland` are the four parts that have something to say.

What is logged is the set of things a person debugs a shell by, not a trace: the version and protocol the
process started with and the configuration path it read; every configuration value the schema refused and
every key it does not know, on the watcher's own category; the compositor attaching, being lost with the
reason, and the delay before each retry; the IPC socket it is listening on, every request it refused, and
nothing at all about the requests it answered; and each layer-surface request it had to refuse. Nothing
here is a place for a secret, and SYSTEM_PROMPT.md § Security is what makes PAM conversation content,
passwords and fingerprints unloggable rather than this file's good intentions.

Records are formatted with a pattern of the shell's own — `%{time yyyy-MM-dd HH:mm:ss.zzz} [%{type}]
%{category}: %{message}` — unless `QT_MESSAGE_PATTERN` is set, because that variable is the user's own way
to make the same decision and the shell overriding it would be taking that choice away.

Qt chooses the destination, and `Logging` installs no sink of its own: records go to the terminal when
stderr is one, and to the systemd journal when it is not. A shell started by niri has no terminal, so its
records are read with `journalctl --user _COMM=quantum-shell`. Two consequences were measured rather than
assumed. First, the pattern is what the *console* handler applies: the journal handler sends the message
alone and carries the level and the category as journald fields instead, so
`journalctl --user _COMM=quantum-shell -o json` shows `QT_CATEGORY: quantum.shell.ipc`, a `PRIORITY` of
`4` for a warning, and `CODE_FILE`/`CODE_LINE` for where it was written. Second, a shell launched with no
tty and no journald would lose its records entirely — which is why the destination is documented here
rather than left implicit, and why the shell's own diagnostics are not the only thing a user can look at:
the IPC keeps answering.

---

---

## Plugin System

Plugins are native Qt QML modules.

```text
~/.local/share/quantum-shell/plugins/my-plugin/
├── qmldir
├── manifest.json
├── BarWidget.qml
├── Panel.qml
└── Settings.qml
```

Plugins can expose bar widgets, panels, settings pages, desktop widgets and shell components. The
plugin system does **not** introduce another scripting runtime.

`manifest.json` (schema-versioned) declares:

```json
{
  "id": "com.example.my-plugin",
  "name": "My Plugin",
  "version": "1.0.0",
  "api_version": 1,
  "qt_min": "6.11",
  "entry": "BarWidget.qml",
  "capabilities": ["ipc", "system-stats", "process-exec"],
  "settings_page": "Settings.qml"
}
```

Rules:

- **Versioned plugin API.** `api_version` gates loading; a plugin built for a newer API is refused
  with a clear error rather than half-loaded.
- **Capability model is advisory, not a sandbox.** QML plugins run in-process with full QML reach;
  the capability list controls which `Shell.*` facade objects are exposed and is documented as a
  contract, not a security boundary. True isolation would need a separate process — explicitly out
  of scope for v1.
- **Failure containment.** A plugin that throws during component creation is disabled and reported
  in Settings; it must never take down the shell.
- **Discovery** is confined to the XDG plugin directory plus a config-declared extra path; no
  scanning of arbitrary filesystem locations.

---

## Exposed QML API

The shell provides controlled APIs such as:

```text
Shell.version
Shell.config
Shell.theme
Shell.ipc
Shell.storage
Shell.systemStats
Shell.execute()
```

Services are exposed as `QObject`-based QML types (singletons registered through the QML module)
rather than allowing plugins direct access to arbitrary C++ internals.

The first one exists: **`NiriService`** is the only place the niri state model is translated for QML,
and it is registered as a singleton so a component can import it and bind:

```qml
import QuantumShell 1.0
Text { text: NiriService.keyboardLayout.currentName }
```

| Property | The binding it exists for |
| --- | --- |
| `workspaces` | the workspace strip — `id` (text), `idx`, `name`, `output`, `isActive`, `isFocused`, `isUrgent`, `activeWindowId` |
| `focusedWindow` | a title widget — `id` (text), `title`, `appId`, `isFocused`, `isFloating`, `isUrgent`, `workspaceId` |
| `outputs` | per-monitor widgets — `name`, `make`/`model`, `isEnabled`, `x`/`y`/`width`/`height`, the fractional `scale`, `transform`, and the current mode with `refreshRate` in millihertz |
| `keyboardLayout` | the layout indicator — `names`, `currentIndex`, `currentName` |
| `overviewOpen` | a bar that hides or changes while the overview is up |
| `connected` | the honest empty state: with no compositor every list above is empty, and this is what says whether that means "no niri" or "no workspaces" |

The key names in that table are interface: a QML file written against them keeps working only while the
spelling holds, so they are declared once in `src/niri/NiriServiceKeys.h` rather than repeated at each
`insert`, and a rename there is the same kind of change as renaming an IPC verb. Each shape's key set is
declared as a list — the always-present keys and, separately, the optional ones with the condition under
which they appear, because an optional key's *absence* is a claim a widget reads. `niri_service_test`
mirrors those lists and compares them at compile time, so a key added to, removed from or renamed in the
service does not build until the test agrees; a test then compares the whole map the service produced
against the declared set, which is what catches a key invented as a string literal. A property added to
the service fails the same suite's meta-object check, and every property is asserted to have a NOTIFY
signal, without which a binding reads it once and never sees it change.

The module's own identity — the import URI `QuantumShell`, its version, and the type name `NiriService`
— is declared once in `src/niri/NiriQmlModule.h` by the same rule, and for a sharper reason: those three
names decide whether a QML file resolves at all, and they fail when a component loads rather than when
the shell builds. `registerQmlSingleton` uses the constants instead of spelling them, and
`niri_service_test` mirrors them with compile-time assertions, so a rename does not build until the
decision is recorded there. The mirror is what the test's own QML import line is built from, so the
binding that follows the notify signals is not also a second copy of the names. This is the one place
where the interface is registered from C++ rather than from a QML module, and it moves with the
registration once `qml/` exists.

Nothing in that service is layout, size, colour or animation: it carries values and QML decides what
they look like. Three rules hold it to the same standard as the model beneath it.

- **A notify signal means the value changed.** `NiriState`'s signals are coarser than these
  properties — every workspace event wakes `workspacesChanged` — so the service compares the value
  before emitting rather than relaying, and an event carrying no news does not wake a binding. A
  background window's title changing is the case that makes this matter: the model has news, the
  focused window QML reads does not.
- **An absent value is empty, never plausible.** `focusedWindow` is an empty map when no window is
  focused and `keyboardLayout` is empty until the compositor reports the layouts, so a widget can tell
  "nothing there" from a real reading. Values that could be mistaken for one — `id: "0"`, a title of
  `""` — are the failure this rule exists to prevent, and a real reading always has keys.
- **Ids cross the boundary as text.** niri documents that ids need not be small and may be generated
  at random; QML's numbers are doubles, so an id is passed as a string it can compare exactly.

The module URI `QuantumShell` and the type name `NiriService` are the first QML-visible names in the
project. They are registered from C++ (`NiriService::registerQmlSingleton`) because no QML module
exists to register them through yet; the first QML component decides whether those two names fit, and
the registration moves into the module in the same change that creates it.

---

## Build Requirements

| Requirement | Version / note |
| --- | --- |
| C++ compiler | GCC 14+ / Clang 18+ (C++23), GCC 16.1+ for C++26 opt-in |
| CMake | ≥ 3.31 (4.4.x tested; CMake 4 removed very old compatibility levels) |
| Qt | 6.11+: Core, Gui, Quick, Qml, QuickControls2, Quick3D, DBus, Network, Concurrent |
| Qt extra | QuickEffects (`MultiEffect`), Multimedia (optional), Svg, ShaderTools (`qsb`) |
| Wayland | `wayland-client` ≥ 1.24, `wayland-protocols` (staging) ≥ 1.45, `wayland-scanner` |
| Protocols (vendored) | `ext-background-effect-v1`, `ext-session-lock-v1`, `ext-idle-notify-v1`, `ext-data-control-v1`, `ext-foreign-toplevel-list-v1`, `fractional-scale-v1`, `wp-viewporter`, `text-input-v3`, `ext-image-copy-capture-v1`, `wlr-layer-shell-unstable-v1` |
| Audio | PipeWire ≥ 1.6 (C API), WirePlumber ≥ 0.5 (D-Bus/pipewire objects) |
| Config | toml++ 3.4.0 (fetched or system) |
| System | `xkbcommon` ≥ 1.8, linux PAM, polkit, libsecret, libsodium, libcurl, libical, libqalculate |
| Testing | Qt Test / `qmltestrunner`, Catch2 (optional), ASan/UBSan builds |
| Docs | pandoc (optional, manual export only) |

Validate the exact Qt module list against the installed Qt version before locking
`CMakeLists.txt`; module names have moved between minors.

---

## Initial CMake Structure

```cmake
cmake_minimum_required(VERSION 3.31...4.4)

project(QuantumShell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(QUANTUM_SHELL_CXX26 "Build with C++26 where supported" OFF)
option(QUANTUM_SHELL_SYSTEM_DEPS "Prefer system dependencies over FetchContent" OFF)
option(QUANTUM_SHELL_SANITIZERS "Enable ASan/UBSan" OFF)

if(QUANTUM_SHELL_CXX26)
    set(CMAKE_CXX_STANDARD 26)
endif()

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core
    Gui
    Quick
    Qml
    QuickControls2
    Quick3D
    QuickEffects
    DBus
    Network
    Concurrent
    Test
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(XKB REQUIRED xkbcommon)
pkg_check_modules(PAM REQUIRED pam)

# TOML parser: system package or pinned FetchContent
if(QUANTUM_SHELL_SYSTEM_DEPS)
    find_package(tomlplusplus REQUIRED)
else()
    include(FetchContent)
    FetchContent_Declare(tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG v3.4.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(tomlplusplus)
endif()

# Wayland protocol codegen (vendored XML -> C sources)
# wayland-scanner client-header / private-code per protocol, into the build dir.

qt_standard_project_setup()      # Qt 6.3+, handles AUTOMOC etc.
qt_policy(SET QTP0001 NEW)       # QML module paths

qt_add_executable(quantum-shell
    src/main.cpp
    src/app/Application.cpp
    src/wayland/LayerSurface.cpp
    src/wayland/WaylandConnection.cpp
    src/wayland/BackgroundEffect.cpp
    src/niri/NiriIPC.cpp
    src/niri/NiriEventStream.cpp
    src/niri/NiriState.cpp
    src/niri/NiriOutputs.cpp
    src/niri/NiriReconnect.cpp
    src/niri/NiriService.cpp
    src/niri/NiriWorkspace.cpp
    src/niri/NiriWindow.cpp
    src/niri/NiriOutput.cpp
    src/niri/NiriKeyboardLayouts.cpp
    src/config/Config.cpp
    src/config/ConfigWatcher.cpp
    src/ipc/IPCServer.cpp
)

qt_add_qml_module(quantum-shell
    URI QuantumShell
    VERSION 1.0
    QML_FILES
        qml/shell.qml
        qml/surfaces/BarWindow.qml
        qml/bar/Bar.qml
        qml/bar/CapsuleGroup.qml
        qml/bar/WidgetSlot.qml
        qml/bar/widgets/Clock.qml
        qml/bar/widgets/Workspaces.qml
        qml/theme/Theme.qml
        qml/effects/FrostedGlass.qml
        qml/components/Panel.qml
        qml/components/GestureArea.qml
)
```

Notes:

- Keep the target list small and grow it as each subsystem lands. Phase 0 must not link every
  optional dependency "just in case".
- Provide `CMakePresets.json` from day one: `dev`, `release`, `asan`, `ci` (offscreen once the shell
  renders; nothing needs a display yet).
- Generate Wayland protocol code in the build tree so generated sources never enter version control.

---

## Quality Gates, Testing and CI

Each phase exits only when the gate passes; a phase is not "done" because the code compiles.

| Gate | Requirement |
| --- | --- |
| Repository scan | `qs-scan` reports no banned pattern, no empty file, and no unread or unscanned required path — see below |
| Build | Clean `-Wall -Wextra -Werror` build on GCC and Clang, Qt 6.11 and latest stable |
| Static analysis | `qmllint` clean for `qml/**`, `clazy`/`clang-tidy` clean for `src/**` |
| Unit tests | Every C++ service has QTest coverage; config parsing/diffing has round-trip tests |
| QML tests | `qmltestrunner` cases for widgets and controls |
| Integration | A script against a compositor started for the test — niri nested in the session, because niri 26.04 has no headless mode (`niri --help`) — that starts the shell and asserts the bar surface appears with the expected namespace and anchors |
| Sanitizers | ASan + UBSan build runs the test suite on every PR |
| Performance | Idle CPU < 1% of one core, RSS < 150 MB, no polling loops, no frame timer at rest |
| Documentation | New config keys and IPC verbs documented in the same PR |

Wired today, before any surface exists: the root `CMakeLists.txt` and `CMakePresets.json` (presets
`dev`, `release`, `asan`, `ci`), the policy target every target links, which carries the warning set
with `-Werror`, the repository scan below, the C++ unit tests for `src/niri/` and `src/config/` with
their live cases, the two slot-order checks and the public-names check described next, and the CI
workflow. The remaining gates arrive with the code they check — a gate is not written into CI before it
has something to run against.

### Configuration (`config-test`, `config-watcher-test`, and a case of `niri-live-layershell-test`)

The schema is a pure function of a file's text, so every rule it has is a case in a string: a value of
the wrong type, an integer where a string belongs, a namespace outside `quantum-shell-`, a height of
zero, a file from a schema version this build does not know, a file that is not TOML at all. That is
`config-test`, which also drives the QObject tree directly to show that a changed property emits its own
signal and an unchanged one emits nothing, and evaluates a `Config.bar.height` binding in a real QML
engine — the same import line and the same property path `qml/Main.qml` uses, so a module URI or a
property name that moves fails here rather than in the bar.

`config-watcher-test` is the half that needs a filesystem, and it is given a scratch directory of its
own rather than the one the person running it has. Its cases are the ones that are easy to get wrong and
invisible when they are: a file that is already there when the shell starts; one written afterwards; one
written in a directory that did not exist yet, which is what watching the nearest existing ancestor is
for; a file unlinked and recreated, where the claim is that nothing was announced in between *and* that
the watch survived the replace; a file removed, which falls back to the defaults; and a file that cannot
be parsed, which must leave the last good values standing rather than reset the bar on a typo.

The end-to-end case is in `niri-live-layershell-test`: it runs the built shell against a `config.toml`
of its own naming a height of 44 and a namespace of its own, and reads both back — the namespace out of
the compositor's layer list, and `set_size(0, 44)` with `set_exclusive_zone(44)` off the protocol
traffic. The rest of that file runs the shell with an empty `XDG_CONFIG_HOME`, so a configuration its
operator happens to have cannot change what its assertions mean.

### Slot independence (`slot-order-independence`, and four `slot-order-randomised-shard` tests)

A test whose `QTRY` wait is already satisfied by state an earlier test left behind does not wait at all:
the assertions after it read the earlier test's data, and the suite is green in the order it was written
in while testing something other than it claims to. Five cases of this were found in `niri-service-test`:
three by the deterministic check — two waits that a previous test's leftovers satisfied, and one
test that dropped the test double's connections for every test declared after it — and two more by the
shuffled one, both preconditions that assumed the state was not already what the slot was about to set
it to, so the events they counted as changes moved nothing.

Both checks read a binary's slot list from the binary itself (`-functions`) rather than from its source,
and a binary that lists no slots at all is an error rather than a pass, because a check that runs
nothing and reports success is the same silent hole these checks exist to find.

The first runs every slot of every unit test binary **on its own**, in a fresh process, and each
binary's whole list once in **reverse declaration order**. Both are needed: a slot that only passes
after another has run has a dependency with no room to hide in the first order, and the second is where
leftover state shows up as a failure rather than as a satisfied wait. QtTest runs the slots named on its
command line in the order given, so neither order asks anything of the tests themselves.

The second runs the same binaries in a **seeded shuffle**, ninety-six passes each, because reverse order
is one particular reordering: a dependency that only shows up when the two slots land a certain way round
is invisible to it, and the two cases it found were reached only by an order neither check derives — both
passed alone, in declaration order and in reverse.

It runs as **four ctest tests** rather than one, which is what its pass count was raised to fit. The test
presets set `execution.jobs 4`, so ctest runs the four at once and the wall clock of the check is the
slowest shard's rather than the sum: measured here, 97 s, 99 s, 97 s and 87 s against 116 s for the
single twenty-four-pass test it replaced, so four times the orders cost less wall clock than before.
Pinned to four cores to model a CI runner, the slowest of the four took 110 s. Without `execution.jobs`
ctest runs the four one after another: still correct, and four times the wall clock — 400 s against 100 s
measured here — which is why the presets and the `check` target set it. It is safe to run them together
because the two tests that act on the desktop take a resource lock, so no two of them are ever changing
what is on screen at once.

The shards split the binaries, **not the passes**, and that is the part that keeps the report honest. How
a binary's slots interact is a property of that binary's own orderings, so all of a binary's passes belong
in one process: split by pass, every shard would hold part of each measurement and could report only a
fraction of a figure whose whole is a union of shards that no single shard can see. Split by binary, every
figure a shard prints is the figure for the binaries it holds, and the run's figures are those added up.
Which binary goes where is decided at configure time from a measured cost per pass — greedy, heaviest
first — because the binaries differ by two orders of magnitude in what a pass costs them, and an
unbalanced split hands back the wall clock the sharding was done to save. Over the twelve binaries here
the shards cost 925, 913, 974 and 904 ms a pass, so the slowest is 7% above the quickest, and the
configure prints those figures: a measurement that has gone stale shows up there as a lopsided split. What
is balanced is the cost and not the count of binaries, which is why one shard here holds a single
binary — `niri-ipc-test` — and another six cheap ones: the wall clock is the cost, and the number of
binaries per shard follows from it. A
target of the check with no measured cost fails the configure rather than being placed on a guess, and the
partition is checked too — every binary in exactly one shard, no shard empty — because a binary in no
shard would be ordered by nothing while the rest of the check stayed green.

A lead the raised pass count had left open is now identified, and it was never about the order. Of twelve
parallel runs of the whole suite in the Clang tree — Debug, no optimisation — two ended with a single shard
failing, both times `slot-order-randomised-shard-3`, which holds `niri-service-test`,
`niri-event-stream-test` and `niri-live-stream-test`. Nothing reproduced it for a while: twenty-four
concurrent shard runs over the Clang binaries and twenty-four over the dev ones — 6,912 shuffled
executions each — were clean, as were 2,640 direct executions of the six wait-heavy binaries run four at a
time. What settled it was the failure text rather than a repetition. The surviving log names the binary,
the pass and the order — `niri-live-stream-test` pass 79 of 96, seed 1981657122 — and the slot that
failed, `theModelAgreesWithTheCompositorWindowSnapshot`, which compares the state the model built from the
event stream against a snapshot read straight from the compositor. The two window titles it printed were
`"Full video 👇 - YouTube - Google Chrome"` and `"I Was NOT Expecting That 😍 #couples - YouTube - Google
Chrome"`: one browser window, renamed because whoever was using the desktop changed the page it was on.
So the failure is a live test racing the live desktop, not a slot depending on the order it ran in — and
re-running that shard with the seed from the log, which reproduces every one of its orders exactly, passed.
The consequence for reading this check's output is that a shard failing this way says nothing about slot
order: the assertion cannot tell a model that has missed an event from one that has been overtaken by a
change made a moment ago, which is why it is rare (the window is small) and why it fires under whichever
shard holds that binary. Making it race-tolerant means teaching the assertion to distinguish being behind
from being overtaken — which is a later change, recorded below.

A later run showed it is not confined to that one method. The same shard failed again while the
layer-shell bar was being built, this time in `theModelAgreesWithTheCompositorWorkspaceSnapshot`, which
compares the same model against the compositor's workspace list: the model held `is_active` true for a
workspace a fresh snapshot reported false, because the focused workspace changed between the model's last
event and the read. It passed in 101 s when the shard was re-run alone. So the live read-only tests carry
this hazard wherever they compare the model to a fresh read rather than to a value from the event stream
itself, and any fix would have to be applied to each of those comparisons rather than to the one method
that failed first.

A third sighting, in the dev tree rather than the Clang one, and with the two discriminations that were
missing on the first two: `slot-order-randomised-shard-2` — where the dev tree's measured cost table puts
`niri-live-stream-test`, confirmed by reading that shard's own command line rather than inferred from its
name — failed once in four full runs of the suite; the seed from that run, `1077806203`, then passed in
126 s when the shard was re-run alone, which is the same result the earlier sightings gave; and the
sixteen unit-test binaries the shards then held, the four added with the IPC and the logging among them,
came through two hundred concurrent invocations run five at a time without a single failure. The second
figure is the one worth keeping, because it is what rules out the standing alternative reading of any
future shard failure — that a unit test has become flaky under load rather than that a live test was
overtaken by a change on the desktop — and it costs a couple of minutes to repeat.

The hazard the three sightings describe is now handled rather than only diagnosed. Every
model-versus-snapshot comparison in `niri-live-stream-test`, and the comparison of `qsctl state` against
the compositor's workspace list in `niri-live-layershell-test`, reads through
`tests/support/SnapshotReconcile.h`, which takes readings until the two sides agree and stops at a budget.
The two sides are named by the caller rather than by the header, so the stream test's failures say `the
model` and the layer-shell test's say `the shell`, which is the process a reader has to go and look at in
each case. What made this more than a retry is what the two sides leave behind: each reading
carries a stamp of the compositor's *own* answer, so the two ways the readings can straddle a change are
told apart after the fact. When the budget runs out while the compositor has been answering the same
thing, a stable reading existed and the model did not reach it — that is a model which has missed an
event, and the failure says so. When the compositor was still answering something new every time, no
stable reading existed for the model to have missed, and the failure says that instead; a reader has to
get to the readings to know which of the two they are looking at, and now does not have to.

Two things keep that from being a way to pass without agreeing, and both were checked rather than argued.
Exhausting the budget fails the test, and the model sitting behind a working retry still fails: with
`NiriState::applyWindowsChanged` made to ignore its event, the live window comparison failed on 57
readings over 1991 ms, reported that the compositor had answered identically on the last 57 of them, and
named it a missed event — `the model holds 0, the compositor reports 5` — rather than a moving desktop.
And the mechanism works in the direction the race actually takes, against the running compositor rather
than a script: with the first reading forced to disagree while every later one is a real request, the run
passed and reported `agreed after 2 reading(s) over 35 ms`, so a disagreement was resolved by a second
reading of the desktop. `snapshot-reconcile-test` pins the contract without a compositor — scripted
readings for each case, including that exhaustion fails and that the two diagnoses do not read alike —
because the race itself needs a window renamed between two readings and cannot be produced on demand on
someone else's desktop. What no live comparison can close is stated in the header: a change that is later
superseded can still make a model that dropped an event agree by luck, which was equally true of the
comparison that never retried.

Turning the same mechanism on the layer-shell test — whose `qsctl state` comparison had its own hand-rolled
retry, ten attempts and a bare diff on failure — found a race in the *test* rather than in the shell.
`startShell` waited for the bar to reach the compositor's layer list, and every `qsctl` assertion after it
assumed the IPC socket existed. Nothing orders those two, and the order turns out not to be a coin toss: the
surface is listed first on **21 of 21 shell starts** across three runs of the binary, with the socket following
about 3 ms later, which each start now reports as `the shell's surface was listed before its IPC socket
existed; the socket followed by 3 ms`. The other cases passed only because they do something slower in the gap
— a `Layers` request, or spawning `qsctl` — and the one that asks `qsctl version` first is the exposed one: one
run lost it, and `qsctl` exited 3, `unreachable`, which the test reported as `Compared values are not the same:
Actual (exitCode): 3, Expected (0)`. Read at face value that is a shell answering wrongly; it was a shell that
had not finished starting. `startShell` now waits for the socket as well and names that step in its failure.

The initial-configure case had the same shape from the other side. It slept a fixed 1500 ms before judging
whether a buffer had ever been attached — an assertion about a machine's speed standing in for a claim about
the order of two requests — so it now waits for the attach within a budget and reports how long it took. Across
runs the attach is already in the transcript when the wait begins or arrives within 202 ms, which is 7 to 15
times less than the sleep, and the sleep would not have been enough on a slower machine. One run failed here
with no attach in 10 s, and the first explanation for it — that re-parsing the whole transcript every 100 ms
was quadratic, holding the event loop long enough that the shell's own stderr stopped being drained, so the
shell blocked writing to a full pipe and never painted — **did not survive being tested**. Restoring that
exactly as it had been and running the whole binary three times passed 3/3, painting after 202, 101 and 0 ms.
So that failure's cause is unidentified rather than fixed, and the wait is kept on its own terms: waiting for
the fact is right whether or not it was what that run needed, and the tail-scanning version written on the
strength of the disproven explanation was reverted rather than left in with a reason that had been tested and
found false.

The comparison's own falsification, against the running shell rather than in a script: with the shell's state
reply made to drop one workspace from the list its own model holds, the case failed after 18 readings over
2917 ms, reported that the compositor had answered identically on all 18 of them, and named the shell as the
side that had missed an event — so the retry cannot hide a defect there either. A first reading forced to
disagree, with every later reading a real `qsctl` call, was reconciled in 170 ms and the run said so. That
falsification also had to be fixed once: building only the test binary leaves the *shell* binary stale, and a
defect injected into the shell then looks like a comparison that does not bite.

Falsifying it turned up something worse than a flaky test, and two core dumps named the cause rather than a
guess: a reply delivered after its caller had given up, written through a frame that had gone. Every live
test's request helper waited for its reply with a deadline and then returned empty, but `NiriIPC::send`
offers no way to withdraw a handler — the client holds it until a reply arrives or the connection ends — so a
late reply still called it, and the helper's storage therefore had to outlive the call. All three helpers now
hold it in a `std::make_shared`, which is the whole of the fix. The backtrace is the evidence, not the absence
of later crashes: `#0 QString::operator=` ← `#1 Reply::operator=` ← `#2 optional<Reply>::operator=` ← the
helper's lambda ← `NiriIPC::deliver`, one abort at `__stack_chk_fail` inside GLib's main-context iteration and
one segmentation fault. `deliversAReplyAfterTheSendingScopeHasEnded` in niri-ipc-test pins the contract those
helpers rest on: with the compositor's end of the socket withholding the reply, the caller's deadline expires
and the reply
arriving afterwards still reaches the handler, after which the next request is still matched to the next
reply. That slot's falsifier runs under AddressSanitizer, because a dangling stack write is not something to
detect by hoping — pointing the handler at a frame that has gone produced `ERROR: AddressSanitizer:
stack-use-after-scope` while the slot as shipped passed clean. A timeout forced by shortening the deadline
proved nothing, and is recorded because it looked like it should: `QTest::qWaitFor` runs one event-loop round
before it consults a zero timeout, which is long enough for a local socket reply to arrive, so both the broken
and the fixed helper passed until the write itself was made to target dead storage.

Randomness that cannot be repeated would make a failure unexplainable, so the seed is printed whether
or not anyone chose it, and `QS_TEST_ORDER_SEED=<seed>` repeats that exact run; the same seed was
verified to produce byte-identical orders and a different seed to differ in every one of them. It is
drawn from the whole range the generator is exact over, and a seed outside that range is refused rather
than quietly producing a different kind of sequence.

A second lead, found when the four-slot measure above was added and not yet acted on. That measure is the
first here able to see that the shuffle's draws are not uniform, because the pair and triple figures
saturate whether the orders are uniform or not. Replaying the shuffle outside the check: for one four-slot
subset its twenty-four relative orders come out with probabilities between 0.028 and 0.084 rather than
`1/24 = 0.042` — a factor of three — and the two directions of a pair between 0.451 and 0.549 rather than
0.5, the same whether the seeds are independent random values or the consecutive states this check draws
them from, so it is the generator rather than the seed sequence. The cause is the shape of the recurrence:
it is linear congruential, whose low bits are its weakest part, and the shuffle takes `draw % count`, which
is exactly those low bits. The cost in coverage is small — a subset's ordering is still near-uniform, and
an order drawn 0.028 of the time still appears in 94% of ninety-six-pass runs — and it does not undermine
the enforced pair floor, whose expected misses at this pass count stay more than twenty orders of magnitude
below the allowance, though the bound that floor prints is derived under the same uniform assumption and so
is optimistic by a few thousand at worst. What it does mean is that some slot orders are explored three
times less often than others, which is the kind of weakness this project would rather fix than document.
Changing the generator is a separate change with a visible consequence: every printed seed would produce a
different run, so the measured figures above would be re-measured and the replay updated with it. The
candidate fix is to take the pick from the high bits of the draw rather than the low ones, or to replace the
recurrence with one whose low bits are sound, and to measure the induced-order spread before and after: that
spread is the falsifier, and it is cheap to run outside the check.

How many passes is a question with a measurable answer, and the answer is not the share of orderings
visited. A binary of n slots has n! of them, so the passes visit a share of the twenty-one-slot one that
is too small to write down, and that figure says nothing about whether the check is any good. What an
order dependence needs is a few slots landing the wrong way round, which gives three measures, each
reaching further than the last. For a **pair**, one slot to another, the covered share after k uniformly
drawn passes is `1 - 2^(1-k)`: 75% at three, 99.2% at eight, 99.95% at twelve. For a **triple** — one slot
sets up state, another reads it, a third clears it — a particular one of the six relative orders is needed,
and the coupon-collector sum gives 43.8% of triples complete at twelve passes, 92.5% at twenty-four and
99.15% at thirty-six. For **four slots at a time** the sum runs over twenty-four orders and gives 0%
complete at twenty-four passes, 1.75% at forty-eight and 65.6% at ninety-six. That last one is the measure
the others cannot stand in for: those four slots can hold any three of themselves in every order there is
while the four are still wrong, because the order a triple of them takes does not say which of the
twenty-four the four take, so a run whose pairs and triples are both complete says nothing about it at all.

The pass count is **derived from that last measure**, and ninety-six is no longer written down anywhere in
the check. A run is asked to reach a share of the four-slot interaction space — `QS_TEST_ORDER_COVERAGE`,
98.3% by default — and the smallest pass count whose model reaches it is the count that runs, which for
98.3% is ninety-six. The four-slot share is the request rather than the pair or triple share because it is
the only one of the three that is still improving at that price: the pair share is finished by twelve passes
and the triple share by about ninety, whereas the four-slot share goes 4.2% after one pass, 63.9% after
twenty-four, 98.31% after ninety-six and 99.99% at the model's ceiling on 264 passes — so it is the figure
that can say what another pass buys. The derivation is printed by every run beside its seed, and the walk is
over the same printed model the report shows, so it can be checked against the output rather than taken on
trust. A request for the whole space is refused, with the ceiling and its pass count in the message, because
the model floors its figure and never claims a space is fully exercised; `QS_TEST_ORDER_PASSES` names a count
directly for the cases where that is what is wanted — reproducing a printed seed, or timing a change — and
the run then reports the count its request would have derived instead.

Ninety-six is where the **triple** findings stop: the expected number of triples
left short of their six orders, over every triple in the suite — 3545 of them, each short with probability
about `6 * (5/6)^k` — is about 268 at twenty-four passes, 3.4 at forty-eight and 0.0005 at ninety-six, so
past roughly ninety-three passes a clean run has no short triple to report at all.

The four-slot measure is the other way round, and that is why it was added: at ninety-six passes it is
**never** complete, so its findings section is the one a run actually reports. The twelve binaries have
13,373 four-slot subsets between them — `C(n,4)` summed over slot counts of four to twenty-one — and
320,952 relative orders. Measured on this checkout at ninety-six passes, a whole run reached 314,744 of
those orders (98.07%) and exercised 8,209 of the subsets in all twenty-four of theirs (61.38%), against the
uniform model's 98.31% and 65.63%. Completing this measure is not reachable at the price of this check:
half of the subsets complete needs 85 passes, 95% of them needs 145 and 99.9% needs 237. So unlike the pair
floor it is **reported, not enforced** — and that is not a slack decision. The pair floor is enforceable
because Markov's inequality bounds the chance a correct run falls below it against an allowance of misses;
counting four-slot misses that way would need their correlation, since one permutation of twenty-one slots
decides the order of every subset it contains at once.

Both measured figures come out below the uniform model beside them, and the gap is the generator's rather
than the recording's. The model is the expectation for passes that draw each of a subset's twenty-four
orders equally often; replaying this script's own shuffle outside the check, the twenty-one-slot binary
averages 141,020 distinct four-slot orders against the model's 141,225 and 3,788 of its 5,985 subsets
complete against the model's 3,928, while the run recorded 141,054 and 3,819 — inside that spread, so what
the check counts is what it claims to count. The reason the shuffle falls short is that its draws are close
to uniform but measurably not uniform, and by Jensen's inequality spreading the same total probability over
unequal orders can only lower the expected number of distinct orders, `1 - (1-p)^k` being concave in p.
The completeness figure also varies with the seed — replayed over eight seeds the same binary came out
between 3,588 and 4,026 — so a run's figure is a sample of the shuffle's rather than the shuffle's
expectation, and the one place to read it is beside the model and the pass count.

What the measure costs is the recording, since there are four times as many four-slot subsets as triples:
574,560 records for the twenty-one-slot binary over ninety-six passes, measured at 7.5 s, with the
read-back enumerating each subset's twenty-four orders in under half a second, and the self-checking
permutation table it reads them through failing the run rather than under-counting if it is edited wrong.
The slowest shard went from 99 s to 110 s against the 900 s each is registered with, and the request moves
the whole thing up or down with it — past the ceiling of 99.99% there is nothing to derive, and a request
short of what the enforced pair floor tolerates is refused by that floor's arithmetic rather than honoured.

Pair coverage is **enforced**, not merely printed, and it can be without flakiness because the slack is
derived rather than chosen. A pair is missed exactly when all k passes agree on its two slots, which
happens with probability `2^(1-k)` — below anything this report can write in billionths at ninety-six
passes — and with the misses the floor allows, Markov's inequality bounds the chance of a correct run
falling below it at below one run in a billion. At this pass count both measures come out complete in a
correct run, so what the floor catches is a mechanism that has stopped working — a shuffle not shuffling,
a recording or reading side that has lost pairs — rather than bad luck.

That bound is arithmetic on the pass count and the number of pairs, both known before a single test is
run, so the check settles them first and **refuses a combination flakier than one run in a hundred
thousand**, with the figure in the message: a 95% floor over six passes would fail a correct run about
one run in two, so it is refused and nothing is run. That refusal is the difference between an enforced
minimum and a threshold that fires at random, and it is the only reason the floor can be enforced at all.
`QS_MIN_PAIR_COVERAGE=0` reports the coverage without asserting it, for a run short on time.

Triple completeness stays reported, and so does the share of orderings. The triples are not a figure a
floor would add anything to at this pass count: they come out complete, so a threshold over them would be
a second restatement of the pair floor's job, which is catching a mechanism that has stopped working. The
part of the triple side a following run acts on is the list of names below, not a percentage.

Reported, and named. A count of short triples says how much of the three-slot interaction space was
reached, which is not something a following run can act on; a name is. So the run ends by writing out the
triples left short of their six relative orders, least explored first — a triple exercised in four of
them is the better lead than one exercised in five — up to `QS_WORST_TRIPLES_SHOWN` per binary (ten by
default, 0 to keep the counts and drop the names). Each is one line, labelled with the number of orders
it was exercised in and written as its three slots in declared order, followed by how many more were left
at least as well explored. The four-slot subsets are named the same way under `QS_WORST_QUADS_SHOWN`, and
that section is the one that is never empty: at the default a whole run named 5,164 short subsets across
the twelve binaries, capped at ten each, the worst of them exercised in 19 of 24 orders. A name there is a
lead for a following run rather than a property of the subset — replayed over twenty-four seeds the
per-subset mean ranged from 22.9 to 23.9 of 24, the correlation between one half of the seeds and the
other was 0.09, and none of the hundred worst was short in all twenty-four — which is the same standing
the triple names have. Measured on this checkout at ninety-six passes over four seeds, nothing is
short: all 3545 triples came out with all six relative orders, so a default run has no findings section
at all, and the cap below matters only for a run asked to do fewer passes. At twenty-four passes the same
report named 239 of the 3545 short — 6 of them in four of their six orders and 233 in five, capped at ten
per binary — which is what the pass count was raised to stop doing. Two checks keep it honest: every label is held to the
band it was filed under, and the bands' total is held to the completeness figure printed above it, so a
report that named a different set of triples from the counts would fail the run rather than read
plausibly. Like every figure this check prints, the report is test output: a green `ctest` run hides it,
so reading it means `ctest --preset dev -R slot-order-randomised -V`.

What is asserted about the shuffle itself is below, on fixed seeds, before any of it is used to judge
anything else — a shuffle that returned its input, or that produced the same order whatever the seed,
would otherwise have the coverage report confidently describing orders that never varied. And the counts
the report is made of are checked from both ends: what the passes wrote down is compared with what the
reading side found, so a recording loop that quietly missed pairs or triples fails the run instead of
lowering a percentage.

Measured on this checkout at ninety-six passes, across four seeds: 100% of the 763 slot pairs exercised
in both orders, and 100% of the 3545 slot triples with all six relative orders, against the 99.99% and
100.00% the models expect for that many passes — both at their ceiling, which is exactly why neither could
say anything about the four-slot measure above, and why the run's findings are there.

The two read-only live tests are covered too, since a slot of either only reads the compositor. The two
that act on the session are deliberately not, because a reordered run of those would change the desktop
more times than their opt-in asks for; their slots are audited by reading instead. The list of binaries
comes from the targets `qs_unit_test` records, and a target in `tests/unit` that did not come from it
fails the configure — a check against silent coverage holes must not have one itself.

Tests are read-only unless asked otherwise. The two live tests read the running compositor; the third
— `niri-live-action-test`, which opens and closes the overview on screen and asks niri to switch the
keyboard layout — is built always but registered only when `QS_NIRI_SESSION_TESTS` is set as well as
`$NIRI_SOCKET`, because a test run must not rearrange the desktop of whoever started it. It restores
the overview and the layout index in its cleanup and reports both, rather than assuming it did.

`niri-live-layershell-test` takes the same opt-in and the same resource lock, and acts on the desktop in
a smaller way: it starts the built shell, which maps a bar and reserves the top of the output for a few
seconds. It proves what § Layer-Shell Implementation Strategy describes — the surface against the
compositor's layer list and against the shell's own protocol traffic — and it is the only test here that
exercises `src/wayland/` at all, since a shell integration cannot be loaded without a compositor to load
it against.

The last, `niri-live-restart-test`, needs `QS_NIRI_RESTART_TESTS` as well as `$NIRI_SOCKET` — the
two opt-ins are independent — and it is separate for a bigger reason: it starts a niri of its own nested in the session, kills it, starts another, and checks that
the connection layer found the new socket, reattached and rebuilt its state without being told. That
is the only way to prove rediscovery, because a real restart moves the socket — niri puts its own
process id in the name — and a test with a fixed path cannot fail on that. A window on the screen for
a few seconds is the price. It skips, with the reason, when there is no niri on `PATH`, no Wayland
session to nest in, or no `$NIRI_SOCKET` to tell its own compositor apart from the session's.

### Public names (`public-names-test`)

The names this project asks a person to type are interface, and two ways of changing one used to pass
unnoticed. `ctest -R <name>` that matches nothing prints `No tests were found!!!` and **exits 0**, so a
test renamed without the documents following leaves every documented command reporting success while
running nothing — the same silent hole the slot checks exist to find, one level up in the documentation.
And an environment variable renamed in the build file while the document keeps the old spelling turns an
opt-in into a no-op with no symptom at all: `QS_NIRI_SESSION_TESTS` is what keeps an ordinary test run
read-only, so a stale spelling is a different test run that still looks green.

So the test names and the environment variables are declared once, in `tests/public_names.cmake`, and
two checks read that file. `tests/CMakeLists.txt` fails the configure if a test is registered without
being declared there — the rename that can reach a build tree — and refuses to pass if it finds no
registered tests at all, because a guard that checks nothing reports success. `public-names-test` then
compares the declared lists with reality in both directions: every `-R` name a document tells the reader
to run must be a declared test, every environment variable a document names or a build file reads must
be declared, and every declared name must appear in a document and be read by a build file — a name
declared that nothing reads or documents is a claim about the project that has stopped being true.

The first name in that list is `public-names-test` itself, so the check is subject to its own rule.

The other names the interface freeze covers — the layer-shell namespaces, the abstract socket, the IPC
verbs, the config keys and `api_version` — are declared here only as far as the code reads them. None of
them was declared before something read it, because a name declared with nothing behind it is a claim
about the project that has stopped being true, which is what the repository gate refuses. Each is
declared once and guarded in the same change that first makes the shell read it, with the same two
halves: a constant in `src/` that the code uses, and a mirror in the test that will not build until the
name is written down there.

- **The abstract socket and the IPC verbs** are declared in `src/ipc/IPCProtocol.h` and guarded twice over.
  `ipc-protocol-test` mirrors the verb list and pins the protocol revision, so renaming a verb or raising
  the version does not build until this document's list and that test's mirror agree. The socket name is
  mirrored in `niri-live-layershell-test`, which asserts the address a person types — `@quantum-shell`,
  with the one leading NUL the kernel prints as `@` — and then reads the same name back out of
  `/proc/net/unix` with the shell running, because the name is the one interface here that fails quietly:
  a spelling with the NUL already in it binds `@@quantum-shell`, a different socket that works and is
  connected to by nothing.
- **The config keys** are declared in `src/config/ConfigSchema.h` as the paths `qsctl config get` takes,
  mirrored in `config-test` and `ipc-capabilities-test`, and the second of those asserts each declared path
  actually resolves.
- **The layer-shell namespaces** exist as `ConfigSchema.h`'s defaults and the prefix the integration
  refuses, and the live test mirrors the default one; `api_version` does not exist yet, so nothing here
  declares it.

### Repository scan (`qs-scan`)

`tools/qs-scan/` implements the detector for the patterns banned in `SYSTEM_PROMPT.md` § Forbidden
Patterns as a real program rather than a shell snippet, so a violation fails the build. It links no
Qt, which keeps the gate runnable on any CI image.

- `tools/qs-scan/patterns.txt` is the rule table: one rule per line, tab separated —
  `id <TAB> severity <TAB> scope <TAB> extended-regular-expression`. `severity` is `error` (fails the
  gate) or `warn`; `scope` is `all` or `outside-tests`, the latter allowing explicitly named test
  doubles under `tests/` (`SYSTEM_PROMPT.md` § Testing Rules). Changing a rule is a one-line edit to
  the table, not a code change.
- Matching is line by line, so an expression cannot span two lines: a failure handler whose braces
  start on the following line is not caught. `patterns.txt` records that limit, and lists the banned
  things that are deliberately not machine-checked because a reasonable expression would produce
  false positives on real code.
- What a pattern cannot decide is not left silently unchecked: an empty or whitespace-only file, a
  required path that is missing or excluded from the scan, a file that cannot be read, and a symlink
  whose target would have to be followed all fail the gate. Every skipped path is reported with its
  reason (`--list`), so no exclusion is invisible.
- Exit codes: `0` clean, `1` violations or structural problems, `2` usage or setup error.
- `--require` names the paths the gate must have scanned. This is the guard against satisfying the
  gate by deleting or renaming the code it was meant to check. `tools`, `CMakeLists.txt`, `src` and
  `tests` are on that list; `qml` joins it in the same change that creates it.
- The cases in `tests/scan/run_scan_tests.cmake` drive the built scanner over the trees in
  `tests/fixtures/`. Those trees are input data for the gate — never compiled, never part of the
  build — and cover detection, the `tests/` exemption, an explicit ignore, a missing required path, a
  symlink, build-output exclusion, and the exit-2 setup path. A case that stops holding is a broken
  gate, and the test fails loudly.

### CI

`.github/workflows/ci.yml` runs the presets on a `g++` / `clang++` matrix, and the `asan` preset in a
second job. The runner's CMake can predate the 3.31 floor, so the workflow pins and installs CMake
4.4.3 instead of inheriting the image's. Both jobs configure, build and run `ctest`, which includes
the repository scan; the matrix job prints the scan on its own first, so a violation is readable in
the job log rather than surfacing only as a failing test.

The workflow has not run yet: the repository has no remote and no commit, so there is nothing to push
yet.

Still in scope, to be added with the code each one checks: `qmllint` and `clang-tidy`/`clazy` (both
can now read `src/`, neither is wired in yet), the QML test suite, the integration script that starts
a niri of its own, translation extraction (`lupdate`), `.qsb` shader compilation verification, and a
packaging smoke build (Nix flake + PKGBUILD) at release tags.

---

## Development Roadmap

Each phase lists its goal and its exit criteria. Phases 0–2 are strictly sequential; 3+ may overlap.

### Phase 0 — Foundation

CMake project, Qt application, QML module, niri detection/connection, first layer-shell surface,
configuration loader, configuration watcher, local IPC server, basic logging.

**Status:** the shell runs. Landed and verified against niri 26.04: the niri connection — socket,
line framing, requests including the ones that carry fields, version and capability detection, the
typed event stream, the workspace/window/output/keyboard-layout/overview state model it feeds, the
request-driven output refresh that niri's protocol requires, and recovery from a compositor restart
(the socket rediscovered, both connections re-established with backoff, the state cleared and rebuilt)
— with its unit tests, its two read-only live tests, the one live test that acts on a session, and the
one that starts a nested niri of its own and restarts it. The state is exposed to QML through
`NiriService`, checked by a test that loads a real binding in a real QML engine with no display. Then
the application: the layer-shell route built (Route 1, § Layer-Shell Implementation Strategy), a
`QGuiApplication` that joins the niri stack to a `QQmlApplicationEngine`, and a `qml/` bar whose
workspace strip and clock are bindings onto `NiriService`. It is a real layer surface on a live niri.

The configuration loader landed too: `src/config/` reads
`$XDG_CONFIG_HOME/quantum-shell/config.toml` through toml++ 3.4, checks `schema_version`, warns about
every unknown key by path, and refuses a value it cannot use in favour of the default while naming both.
Its values reach QML as the `Config` singleton, whose nested objects carry one notify signal per
property, so an edit emits only for what changed. `ConfigWatcher` watches the file *and* its directory —
and the nearest ancestor that exists when `~/.config/quantum-shell` does not — parses a change on a
worker thread and applies it on the GUI thread, and reads the file synchronously once before the QML
engine loads, so a configured bar is built with its configured height and namespace rather than
corrected after it appears. The bar's `layerNamespace`, `height` and `exclusiveZone` are bindings onto
it. The one thing a config change cannot do is rename a surface's namespace: the protocol assigns a
surface's role once, so a live change is reported as needing a restart instead of being quietly
ignored.

The IPC server and the logging landed with it, and they are described where the design is: § IPC for the
socket, the frames and the verbs, and § Logging for what a record is and where it goes. The exit criteria
below are therefore unmet on one count rather than three — the shell displays a bar on niri, reloads config
without restarting, and responds to `qsctl`; what is missing is that restart survival is proven for the
connection layer, by `niri-live-restart-test` starting and restarting a nested niri, and not yet through a
running shell, whose bar and state would have to come back after a compositor it was attached to went away.
The bar's route is in that list no longer either: `niri-live-layershell-test` covers it — the surface in
the compositor's layer list, the anchors and the exclusive zone on the wire, the configure acknowledged
before any buffer, and, since the IPC landed, three cases that are the shell answering `qsctl` while the
compositor watches. The config reload has three tests of its own — `config-test` and
`config-watcher-test`, neither of which is compositor-facing, and the configured case of
`niri-live-layershell-test`, which runs the shell against a `config.toml` naming a height and namespace
of its own and reads both back off the wire and out of the compositor's layer list.

**Exit criteria:** Quantum Shell displays a basic QML bar on niri, responds to `qsctl`, reloads
config without restarting, and survives a compositor restart.

### Phase 1 — Core Bar

Bar layout, capsule groups, workspace indicator, clock, system status, audio, network, battery,
media, gesture handling, niri workspace interaction.

**Exit criteria:** a fully functional daily-driver bar; no polling; workspace changes are instant
and event-driven; idle CPU budget met.

### Phase 2 — Shell Components

Launcher, notifications (with history and D-Bus service), OSD, control center, panels, media
controls, system controls.

**Exit criteria:** replaces the major pieces normally provided by a standalone desktop shell;
notification daemon passes `notify-send`-based smoke tests.

### Phase 3 — Quantum 3D Layer

Qt Quick 3D scene, 3D desktop widgets, depth/elevation, lighting, widget transforms, widget editor,
parallax wallpaper, spatial transitions.

**Exit criteria:** the distinctive Quantum Shell 3D environment, fully optional, with measured
performance numbers and a clean 2D fallback.

### Phase 4 — Lock & Session

Lock screen, authentication, PAM, fingerprint support, session controls, logout/reboot/shutdown,
idle handling.

**Exit criteria:** secure session-lock integration; the lock surface cannot be bypassed; auth
failures are rate-limited and timed out safely.

### Phase 5 — Plugin System

QML module plugins, manifest format, plugin discovery, lifecycle, capability model, settings
integration.

**Exit criteria:** a third-party plugin can add a bar widget and a settings page without touching
shell source.

### Phase 6 — Remaining Desktop Features

Dock, clipboard history, screenshot tooling, calendar, Bluetooth, cellular, display controls, power
profiles, advanced media controls.

### Phase 7 — Theme System

Theme templates, palette system, dynamic colors, typography, shell-wide spacing, animation presets,
theme import/export.

**Exit criteria:** a complete theme is a single file plus optional assets, and switching themes does
not restart the shell.

### Phase 8 — Hardening

ASan/UBSan, automated tests, crash handling, performance profiling, memory profiling, Nix flake,
Arch/AUR packaging, release builds.

**Exit criteria:** reproducible packages for at least one distro, a released version, and a
documented upgrade path between shell versions.

---

## First Implementation Target

The first milestone is deliberately small.

```text
main.cpp
    ↓
Qt application
    ↓
QML engine
    ↓
niri IPC connection (event stream + version detection)
    ↓
Wayland layer-shell (the Phase 0 spike)
    ↓
BarWindow.qml
    ↓
Bar.qml
    ↓
Clock + Workspaces
```

Do **not** start with:

- 3D widgets
- plugin store
- lock screen
- dozens of D-Bus services
- complex effects
- theme engine

First prove that Quantum Shell can reliably create a native QML layer-shell surface on niri and
communicate with niri. Once that foundation works, everything else can be built incrementally.

---

## Risk Register

| Risk | Impact | Mitigation |
| --- | --- | --- |
| No public layer-shell API in QtWaylandClient | Blocks everything | Dedicated spike before Phase 0 coding; isolate Qt-version-specific code in one shim |
| Qt private API breakage between minors | Build breaks on upgrade | Pin Qt minor in CI; compile-time guards; keep shim < ~200 lines |
| Qt 6.12 LTS changes 3D/effects APIs | Rework of 3D layer | Keep 3D layer thin and behind a config tier; track Qt release notes per minor |
| Qt Quick 3D power draw on laptops | Battery complaints | Opt-in tier, on-battery profiles, idle pause, measured budgets |
| niri IPC event stream format changes | Shell state desync | Version detection + tolerant JSON parsing; log unknown events instead of crashing |
| Plugins crashing the shell | Reliability | Contained component creation with error reporting; disable-on-failure |
| Session-lock bugs | Security | Small, audited surface; dedicated auth tests; no plugin code on the lock surface |
| Fractional scaling artifacts | Visual quality | Test matrix at 1.0/1.25/1.5/2.0 from Phase 0 |
| Config semantics drift across versions | User-visible breakage | Schema version + migrations + "unknown key" warnings |

---

## Project Identity

| | |
| --- | --- |
| **Name** | Quantum Shell |
| **Target** | niri |
| **UI** | QML / Qt Quick |
| **3D** | Qt Quick 3D |
| **Backend** | C++23 (C++26 opt-in) |
| **Display protocol** | Wayland |
| **Design philosophy** | 2D shell + integrated 3D spatial layer |
| **Primary goal** | A complete, modern, highly customizable niri desktop shell with a distinctive 3D visual system, without putting rendering or layout logic into C++ |

---

## Non-Goals

Quantum Shell will **not**:

- support Hyprland
- support Sway
- support multiple compositors
- become a compositor itself
- replace niri
- implement UI layout in C++
- implement rendering in C++
- depend on Electron
- depend on a separate JavaScript runtime
- use a legacy Qt 3D architecture when Qt Quick 3D provides the required functionality
- reload the QML engine to apply configuration changes
- ship a plugin sandbox (documented as a capability contract instead)
