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
`Windows`, `Outputs` and `FocusedWindow`, and for actions such as `FocusWorkspace`,
`FocusWorkspaceUp`, `FocusWorkspaceDown`, `Spawn`, `MoveWindowToWorkspace` and `ScreenshotScreen`. They are spelled as the variants of niri-ipc v26.04's
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
  least 1, also the exclusive zone reserved from the tiling area), `bar.layerNamespace` (which must begin
  with `quantum-shell-`, the frozen prefix of AGENTS.md), `[bar.system]`'s four —
  `sample_interval_ms` (milliseconds between the readout's two `/proc` readings, at least 10 and at most
  `INT_MAX`, defaulting to 2000), `show_cpu` and `show_memory` (booleans, both true, each saying whether
  that readout is drawn) and `memory_format` (one of `used_of_total`, `used`, `available`, `percent`) —
  and `[bar.audio]`'s four: `show_volume` (a boolean, true, whether the volume readout is drawn),
  `volume_scale` (which of the two numbers the readout is drawn in, `percent` or `decibel`, defaulting to
  `percent` — and the same token decides which of the two step keys a notch applies, so what is read and
  how far a notch moves cannot disagree), `step_percent` (percentage points one wheel notch moves the
  volume in the percentage unit, at least 1, defaulting to 5) and `step_decibels` (decibels one notch moves
  it in the decibel unit, a number at least 0.1 — the readout's own resolution, since a shorter notch would
  change nothing a person can see — defaulting to 1.0). Neither step has a ceiling, because a step larger
  than the range is a request that one notch reach the end of it.
  — and `[bar.network]`'s three: `show_status` (a boolean, true, whether the network readout is drawn at
  all), `show_name` (a boolean, true, whether the name of the network it is on is drawn beside the signal)
  and `show_strength` (a boolean, true, whether the signal is drawn). Three booleans rather than a strength
  threshold or a list of networks, because the shell decides none of those: what it knows is what the daemon
  told it, and a key that filtered it would be a second opinion about the desktop. The file is
  `$XDG_CONFIG_HOME/quantum-shell/config.toml` —
  `~/.config/quantum-shell/config.toml` by default — and `qml/Main.qml` binds its `height` and
  `exclusiveZone` to `bar.height` and its `layerNamespace` to `bar.layerNamespace`.

```toml
schema_version = 1

[bar]
height = 32
namespace = "quantum-shell-bar"

[bar.system]
sample_interval_ms = 2000       # how far apart the system readout's two /proc readings are
show_cpu = true                 # whether each of the two readouts is drawn at all
show_memory = true
memory_format = "used_of_total"  # used_of_total | used | available | percent

[bar.audio]
show_volume = true              # whether the volume readout is drawn at all
volume_scale = "percent"        # percent | decibel — the unit the number is drawn in, and the one a notch
                                # is measured in: it selects which of the two steps below applies
step_percent = 5                # percentage points one wheel notch moves the volume, in the percent unit
step_decibels = 1.0             # decibels one wheel notch moves the volume, in the decibel unit

[bar.network]
show_status = true              # whether the network readout is drawn at all
show_name = true                # whether the name of the network it is on is drawn
show_strength = true            # whether the signal quality is drawn
```

  `bar.system` is a table rather than a set of compound keys such as `system_sample_interval_ms` because
  the widget that owns the settings owns a table in the file, which is the shape the readouts still to come
  will take — and the QML tree mirrors the file's tables, so `Config.bar.system.memoryFormat` is where a
  binding writes what `bar.system.memory_format` is where a person writes. The cadence is also the one key
  today that no QML file binds to: it is applied by the composition root, which hands the value to
  `SysMonService` before the engine loads and follows later edits through the tree's own change signal.

  Which readouts are drawn is two flags rather than one `readings = ["cpu", "memory"]` list, and that is a
  decision with a reason: a list decides an *order*, and the order two readouts appear in is arrangement,
  which this document gives to `qml/` — a list here would be a second place the bar's own layout is written
  down. The form the memory readout takes *is* a token, because a token is a choice the file makes and the
  widget renders; there is no `cpu_format` beside it, because the only honest form for a CPU percentage is a
  percentage and a key whose every legal value draws the same thing is a key that does nothing. The token is
  validated here rather than in QML for the reason the rest of this file exists: a widget handed a form it
  has no branch for would silently draw the default, and a configuration value that does nothing is the
  failure the schema reports instead.  `config-test` holds the schema's four tokens against its own copy at
  compile time, and `bar-interaction-test` drives each of them through the shipped widget, so a token renamed
  on either side fails a build or a test rather than reaching a screen.

  `bar.audio` is the second table of the same shape, and it differs from `bar.system` in exactly one respect
  worth stating: both of its steps are read by a *service* rather than by a widget. The distance a wheel notch
  moves is a distance in a volume the daemon owns, so the composition root hands the two values to
  `PipeWireService` before the engine loads and follows later edits — the same wiring the cadence has, for the
  same reason. `show_volume` is the widget's, and like the system status's two flags it hides the readout
  rather than switching off the reading: the service follows the default sink whether or not anything is drawn
  from it. `volume_scale` is both halves at once, and it is the counterpart the memory form has and the CPU
  readout does not: a volume has two honest numbers — the desktop's own convention, which is the cube root of
  the daemon's linear factor and what `wpctl get-volume` prints, and the physical gain in decibels, which is
  what `pactl list sinks` prints beside it — so a token decides which one is drawn, where a CPU percentage has
  a single form there is no honest alternative to. The same token decides which step a notch applies, because
  a step is a distance in the unit a person is reading and the two units are not proportional: a percentage
  point is worth 0.26 dB at the top of the range and 18.06 dB at the bottom of it, so a person reading
  decibels and moved by points would be moved by a number that depended on where they already were — by a
  factor of thirty-six across the range. Two keys rather than one whose meaning depends on another key: a
  schema where the same word means two things is a schema that lies about itself. See § Audio for the
  arithmetic, the measurement behind it and what a notch from silence does.
- **A namespace is read once.** `zwlr_layer_surface_v1`'s namespace is an argument to
  `get_layer_surface`, and a surface may be given a role once, so a change to `bar.layerNamespace` applies to
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
| `config get <path>` | `path` and `value` | the schema resolves it; the paths that exist are its own `KeyPaths` list — `bar.height`, `bar.layerNamespace`, `bar.system.sample_interval_ms`, `bar.system.show_cpu`, `bar.system.show_memory`, `bar.system.memory_format`, `bar.audio.show_volume`, `bar.audio.volume_scale`, `bar.audio.step_percent`, `bar.audio.step_decibels`, `bar.network.show_status`, `bar.network.show_name`, `bar.network.show_strength`, each asserted against the resolver in `ipc-capabilities-test` |
| `bar toggle` | `visible` | the bar window's own visibility, and the compositor's layer list loses and regains the surface |

`qsctl` prints one line of JSON for `version`, `state` and `bar toggle`, and the bare value for
`config get` — that verb exists to be used as `x=$(qsctl config get bar.height)`, and quoting a number for
a JSON document would wrap the answer it was asked for. Its exit codes are interface, because a script
branches on them: `0` answered, `1` refused, `2` a command line it does not take, `3` no shell listening,
`4` a protocol mismatch. A verb this build has no handler for is refused as a *usage* error at the client,
not sent to the shell to be refused there — `qsctl volume up` is a command this document mentions for the
volume widget, and it is still not a verb this shell answers, so the honest answer remains that qsctl does
not take it rather than a request the shell receives and declines, which would read as "the shell is
broken". The reason for the refusal changed with the volume module and the reason text had to change with
it: the claim is about the verb's existence, not about whether a service is there to answer it.

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

**Landed**, in `src/app/Logging.h` / `Logging.cpp`. Seven categories, declared once, because a category
name is interface twice over: a person filters with `QT_LOGGING_RULES="quantum.shell.ipc.debug=false"`
and a bug report quotes it. `quantum.shell` is the shell's lifecycle, and `quantum.shell.niri`,
`.config`, `.ipc`, `.wayland`, `.system` and `.audio` are the six parts that have something to say — the
last two because a reading that stops arriving is otherwise invisible: the widget draws its empty state
either way, so the category is where the reason for the empty state is. `.audio` carries the daemon it
attached to, the sink it resolved as the default, every param it refused to read as a volume, every write
it refused to make, and the backoff between attempts after a daemon went away — a volume that stops moving
is otherwise indistinguishable from a volume that did not change.

What is logged is the set of things a person debugs a shell by, not a trace: the version and protocol the
process started with and the configuration path it read; every configuration value the schema refused and
every key it does not know, on the watcher's own category; the compositor attaching, being lost with the
reason, and the delay before each retry; every action the shell asked the compositor to perform that did
not happen, with niri's own reason, because the caller that left the handler out is QML and cannot hold
one; the IPC socket it is listening on, every request it refused, and nothing at all about the requests
it answered; and each layer-surface request it had to refuse. Nothing
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
binding that follows the notify signals is not also a second copy of the names. The module those names belong to is
declared once in `src/QmlModule.h` — the URI and both version numbers — and `NiriQmlModule.h`
re-exports them under the names it has always used, so the one place the URI is spelled is a file that
is not this service's.

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
project, and they are registered from C++ (`NiriService::registerQmlSingleton`) into the module
`QmlModule.h` declares, which is what every `import QuantumShell 1.0` in `qml/` resolves against and what
`qmltestrunner`-style tooling would read. The second singleton below was added to the same module rather
than to one of its own, for the reason a group arrives with the widget that belongs in it.

### The actions QML performs

The bar does more than read: a click on a capsule focuses the workspace that capsule names, and the
wheel over the strip moves to the workspace below or above. Both are niri actions, so they are the
action layer the shell already had — `NiriActions` — registered into the same module as a second
singleton instead of being wrapped in a third object that would be a second view of them to keep in
step with the first.

| Method | The gesture it exists for |
| --- | --- |
| `focusWorkspaceById(idText)` | a click on a capsule — `FocusWorkspace` with the id the model reported for that capsule |
| `focusWorkspaceDown()` | a wheel tick down — niri's `FocusWorkspaceDown` |
| `focusWorkspaceUp()` | a wheel tick up — niri's `FocusWorkspaceUp` |

The type name `NiriActions` is interface by the same rule as `NiriService`: it is what a QML file
calls, so it is declared once in `src/niri/NiriQmlModule.h` and mirrored, with a compile-time
comparison, in both `niri_service_test` and `bar-interaction-test`.

Three rules, each with a reason that is a real behaviour rather than a preference:

- **The id crosses as text and is parsed back exactly.** niri documents that workspace ids need not be
  small and may be generated at random, and QML's numbers are doubles — the same reason
  `NiriService` reports ids as text. It is parsed back into a `u64` by checking the digits rather than
  by asking Qt's number parser, which also accepts a sign and surrounding space, and an id above the
  largest value the request encoding can hold exactly is refused with the reason instead of being
  rounded into a request that names a different workspace.
- **The gesture is niri's, not the strip's.** `focus-workspace-down` and `focus-workspace-up` are what
  niri's own default config binds to the same wheel gesture (`Mod+WheelScrollDown cooldown-ms=150 {
  focus-workspace-down; }`), so which workspace is below is the compositor's answer and not an index
  computed from the strip. The bar does not walk its own model to decide where the wheel goes. One gap,
  stated rather than hidden: niri rate-limits its bind with `cooldown-ms=150` and the bar's handler acts
  on every wheel event it is given. A mouse wheel sends one event per notch, so the two agree there; a
  continuous touchpad scroll is the case where a cooldown belongs once the bar has one.
- **A click on the already-focused capsule asks for nothing.** niri resolves the reference and then
  switches to it, and with `workspace-auto-back-and-forth` — which this project's own session sets —
  switching to the workspace already focused lands on the previously focused one. A bar that moved a
  person off the workspace they are on when they click the workspace they are on would be worse than one
  that does nothing, so the click is dropped in the component.

A QML caller cannot hold a result handler, so nothing the bar asks for fails silently: every action
that is refused, or whose answer never comes, is both a signal on `NiriActions` and a record on the
compositor's category (§ Logging).

The gestures are covered by `bar-interaction-test`, which loads the shipped `qml/Bar.qml` into a real
engine with the platform plugin set to offscreen — no display, no session — and delivers real click and
wheel events through the groups the widgets are declared in, asserting what reached the compositor's end
of the socket. That is the only place the wiring can be checked: what a click means is a property of the
component, and a capsule that draws the right workspace and does nothing when clicked passes every
assertion a C++ test can make. The two action names are confirmed against a running niri 26.04 by
`niri-live-action-test`, which steps down a workspace and back up and watches the model follow.

### How the bar is arranged

A widget is not anchored into place. The bar is a small set of **named groups** — `qml/CapsuleGroup.qml`,
one per region the bar declares — and a widget is placed by being declared inside the group it belongs
to. The widget carries no coordinate, no anchor and no offset of its own: the group decides the order and
the spacing of the widgets in it, and the bar decides where a group sits and how tall it is. So
"where does the next widget go" is a line inside one of those groups, and moving a widget between regions
is a move between groups rather than a rewrite of its geometry.

Two things the group owns, and they are the reason it is a component rather than a bare `Row`:

- **The height convention.** Every widget of a group is given the group's height, and a widget draws what
  it has to draw in the middle of the height it was given. Widgets of different natural sizes therefore
  line up along one line without any of them knowing how tall its neighbour is — which is what a bar of
  hand-anchored items gets wrong the first time somebody adds a widget with a different font size. It is
  enforced in the component rather than left as an instruction each widget is trusted to follow, because
  the widget that chooses its own height is the widget that breaks the row. The clock is the one widget
  that had to change for it: a `Text` given more height than its glyphs centres its own text, since the
  group has stopped anchoring it.
- **The name.** A group is named by the bar — `left`, `right` — and that name is the object's name in
  Qt's own tree, so anything outside the component can address the group a widget landed in without
  counting the bar's children to work out which one it has hold of.

Three groups, and no fourth: there is a left one, a centre one and a right one, holding the workspace
strip, the system status and the clock. The centre group arrived the same way the other two did — with the
widget that belongs in it — and a group still arrives that way, so the media readout the roadmap lists will
bring its own or join one of these rather than finding an empty region waiting. A group also does not push
another one aside: the centre group is centred on the bar, so a bar too narrow for its side groups would
overlap them rather than shrink them, and that decision belongs with the group that has to give way.

`bar-interaction-test` is where the arrangement is checked, on the same components the application packs.
It loads `qml/Bar.qml` rather than one widget out of it — so the click and the wheel are delivered
*through* the groups — and reads back which group each widget landed in, that each widget is its group's
height and sits at that group's leading edge, and that the bar's three widgets are in the groups they were
declared in, with the centre one on the bar's own middle rather than on the room the side groups left. The
group is also loaded on its own and given widgets of three different natural heights, which is the claim
the bar itself cannot make: its widgets are already the height they are given, which would not tell "the
group sized them" apart from "they happened to be that size".

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
with `-Werror`, the repository scan below, the C++ unit tests for `src/niri/`, `src/config/` and
`src/system/` with their live cases, the two slot-order checks and the public-names check described next,
and the CI workflow. The remaining gates arrive with the code they check — a gate is not written into CI before it
has something to run against.

### System statistics (`sysmon-test`, and four slots of `bar-interaction-test`)

The bar's middle group reads the machine's own CPU and memory, and the reading is the shell's second one
with no event source. The reason is sharper than the clock's and it is what the module is shaped around:
`/proc/stat`'s aggregate line is *cumulative jiffies since boot*, so a CPU percentage is not a value the
kernel holds at all — it is the difference between two readings, and a difference needs two moments.
Nothing else offers a subscription: the kernel's pressure files report stall time rather than occupancy,
and this machine has no `/proc/pressure/memory` at all, which was read rather than assumed.

So `src/system/SysMonService.cpp` takes the directory it reads from as a constructor argument, for the
reason `ConfigWatcher` takes its path, and splits into two halves. The parsing is pure functions of the
text — the aggregate `cpu` line rather than a per-core one; the eight counters the kernel documents summed,
because the guest counters are already counted inside user and nice and counting them again would count
that time twice; `MemTotal` and `MemAvailable`, where "used" is the subtraction because no such field
exists; and a refusal rather than a value for every shape of input that is not those files.

Around that half sits the cadence, and it is the person's rather than a constant: `bar.system.sample_interval_ms`
is handed to the service by the composition root at startup and again on every edit to it, so a file's value
reaches a running shell through the same setter the build's default does. The service writes one record each
time it accepts a cadence — `sampling /proc/stat and /proc/meminfo every N ms while the bar is visible` —
because the interval is otherwise invisible from outside the process, and that record is what the live test
reads back out of a running shell to prove the configured value got there.

What the widget *draws* from those readings is the configuration's too, and the split is where the boundary
is drawn: the schema owns which forms exist and refuses anything else by name, the widget owns what a form
looks like. So `show_cpu` and `show_memory` say whether each readout is drawn (the row, label and number, not
just the number) and `memory_format` picks one of four renderings of the same reading — used of total, used
alone, what is still available, or a percentage. Hiding a readout does not switch off its reading: the service
takes both halves of every sample in one pass whatever is drawn, so a hidden readout costs no wake-up and a
shown one is never a number waiting for a flag to change. All four memory quantities come from the one
reading the kernel gives — `MemAvailable` is published as the field it is (`memoryAvailableKb`) rather than
left for a widget to recompute by subtracting `memoryUsedKb` from `memoryTotalKb`.

Three properties of the cadence are decisions rather than defaults. A reading
that could not be taken is not guessed at and is not left standing: the flags go false, which is what the
widget's dash is driven by, and the reason is recorded on `quantum.shell.system`. Nothing is sampled while
the bar is off screen — `active` is the window's own visibility, wired in the composition root, so a hidden
bar costs no wake-ups at all. And deactivating forgets the reading rather than keeping it: a CPU percentage
is an average over the sampling interval, and one carried across a gap nobody was watching would be a number
that is real but is not the one it is labelled as.

This is the one place the shell departs from SYSTEM_PROMPT.md § Event-driven, which bans a `QTimer`
re-reading state. The exception was chosen by the user over the two alternatives — readings that need no
delta at all, refreshed only when looked at, or sampling only while the pointer is over the widget — and the
dated line recording it belongs in § Waivers there, which only the user writes. The gate reports the five
`polling-timer` hits in `src/system/` as warnings for a human to read; this paragraph is that reading.

What the readout draws from those four settings is checked in the two places that can each see one half of it.
`config-test` is the schema's: each of the four tokens is accepted, a token outside the list is refused with
the list named, a flag is a boolean or it is reported, the tree announces each setting on its own signal, and
a compile-time mirror holds the schema's token list to the test's own copy of the four — so a token renamed in
`src/` stops the build until someone decides what it means. `bar-interaction-test` is the widget's: it writes
each token into a real configuration file, parses it through the schema, applies the validated values to a
real `Config`, and compares the text the shipped `qml/SystemMonitor.qml` then draws — which is the only place
the token becomes a string a person reads. The same file's other slot turns each flag off and asserts the
readout as a whole (its row, so label and number together) is no longer drawn, that the other one still is,
and that with both off the widget takes no width at all rather than keeping a gap.

`sysmon-test` is the module's own test: the lines and their refusals, driven by files it writes into a
directory of its own so every shape of input can be shown, including the ones the kernel does not produce,
with two slots reading the real `/proc` as well — because a parser only ever handed this test's own strings
agrees with this test and nothing else. It also pins the cadence: nothing published while inactive, memory
the moment the service is activated and a CPU percentage one interval later, an interval of zero refused,
no signal at all when a reading did not change, and the record accepted rather than refused ones write —
once per change, and not at all when the value it was given is the one it already had.

The configuration's half of the same claim is `config-test`: the interval's default, its floor and its
ceiling, a value of the wrong type and a key inside the table that this build does not read, each reported
by its full path; and a compile-time guard that compares the schema's default and floor with the constants
`SysMonService` refuses outside of, so the two spellings cannot drift apart silently. The end-to-end half is
the case in `niri-live-layershell-test` described under § Layer shell, which is the only place the wiring
from a file to a running service is exercised, because the composition root is where it lives. `bar-interaction-test` carries the other half: the
widget in the centre group, and its two readouts drawing their empty state rather than a zero before a
reading exists — compared against the service's own value once there is one, which is what tells a binding
that reads the service apart from a string that happens to look like a reading.

Those slots were the last ones in the suite whose expectations depended on something the test does not
control, and they no longer do. The service is constructed against a `/proc` of the test's own — a `/proc/stat`
and a `/proc/meminfo` in a scratch directory of its own, written and rewritten by the test — so the four memory
forms, the empty state and the two percentages are compared with literals that are the answer for numbers the
test wrote, and nothing about the machine under the test can move them. The counters in that fixture advance on
every write, which is a rule rather than tidiness: a real `/proc` only ever goes up, so any two readings of it
are an interval, and a fixture whose line stood still would be provoking the service's refusal — a record
`sysmon-test` asserts on purpose, and one this file was in fact producing twice a run before the writes were
made to advance. Determinism is the whole claim of those slots, and it is reachable only because the service
takes the root it reads as a constructor argument; the composition root hands it `/proc`, and a test hands it
something it owns.

One slot still reads the machine's files, because a fixture proves the parsing and the binding and nothing
fixes that nothing would then prove the reading is real. Keeping it meant making its claim sharper rather than
weaker: it asserts that the *first* reading of the machine is a baseline and not a percentage, which is what
says the point the second one is measured against is the machine's. That assertion is load-bearing, which was
established by removing the baseline clearing above it and watching it fail — the pair is not refused, because
the machine's counters are the larger and it therefore does not go backwards; what gets published instead is
the busy share of the whole boot, 12.07% on this machine against the interval's few per cent. A slot that read
the machine *and* remembered a fixture point would have reported the boot's average as the state of the last
200 ms and passed. Falsifying the rest: restoring the pre-restore order in the memory-form slot fails (`50%`
where the form it expects is `8.0/16G` — the loop leaves its last token applied, so the comparison was
silently about the form), a fixture whose counters do not advance fails, and a live slot that keeps either its
fixture baseline or its own machine baseline after the last assertion fails on its own closing check.

That slot was also the last place this file was producing a record nobody asserted, and it was its own doing:
a second sample taken immediately after the first, there to have a memory reading to compare, which is two
readings microseconds apart on a `/proc/stat` the machine is writing. The service refuses that pair and says so
on its category — correctly, since a percentage over no interval is not a measurement — so the slot was
provoking a refusal about its own sampling. It was found by marking each of the slot's call sites and running
it alone: the record appeared in three runs of six, and the marker named the line. The second sample is gone,
because the first one publishes memory anyway — a value needs no interval — and the refusal cannot be provoked
by the slot any more. This is the same rule the fixture's advancing counters keep, applied to the slot's own
readings: a test may assert a refusal, and it may not create one and leave it in somebody else's transcript.

### Audio (`audio-test`, `audio-live-test`, and three slots of `bar-interaction-test`)

The bar's volume readout is the counter-example to the system status above: it is a reading with a real event
source, and the module is shaped around using it rather than asking. `src/audio/PipeWireService.cpp` attaches
to the PipeWire daemon, follows the sink the `default` metadata names under `default.audio.sink`, and
subscribes to that node's `SPA_PARAM_Props` — which the installed header documents as "Automatically emit
param events for the given ids when they are changed" — alongside the node's own `info` event, whose
`change_mask` carries `PW_NODE_CHANGE_MASK_PARAMS` and names the ids that moved. So a volume changed by
anything else on the desktop — `wpctl`, a mixer, a headset's own button — arrives as a callback, and nothing
in `src/audio/` asks the daemon anything on a schedule. Nothing here needs the waiver the system status does.

**The percentage is a convention, and it is the desktop's rather than this shell's.** WirePlumber's own
`wpctl get-volume` prints `0.35` for a sink whose Props param reports `channelVolumes: [0.042872, 0.042872]`,
and 0.042872 is 0.35 cubed — so the number a person reads is the *cube root* of the linear factor, and taking
the linear value for the percentage would put the bar at 4% where every other volume control says 35%. That
measurement is in the module's comment beside the function that applies it, and `audio-test` pins it:
`percentFromLinear(0.042872) == 35`. Nothing is converted in QML — a widget that did its own arithmetic would
be a second place the convention is written down.

What the module reads and what it refuses are the same rules as everywhere else. A Props param that is not a
Props object, or one whose `channelVolumes` is not an array of floats — four-byte elements of the wrong type
are the case a reader that skipped the element type would turn into a volume nothing sent — is a refusal
rather than a default, and the last reading stands with the reason on `quantum.shell.audio`. An unnamed
default sink is `available == false` and a dash, never a fallback device: a per-sink control is the control
centre's, and the bar has one volume to show. The Props param is the only place the reading exists — the
sink's own property dict carries `audio.channels` and `audio.position` and *not* `audio.volume`, which was
read off this session rather than assumed.

The module's pure half is `src/audio/AudioVolume.h` / `AudioVolume.cpp`, for the reason `src/system/`'s
parsers are separate: the Props parse, the cube-root mapping, the wheel's arithmetic, the metadata value's
JSON object and the pod a write is are all functions of bytes, so every shape of input a daemon can send is a
case in `audio-test` — including the shapes a healthy daemon never sends. No daemon, no socket and no
display. The pod the module *writes* is read back by the same reader that reads the daemon's, which is what
holds the two halves of the module to one spelling of the protocol.

Three of the module's settings are the configuration's rather than constants, and the split follows the
system status's: `bar.audio.show_volume` says whether the readout is drawn, `bar.audio.volume_scale` says
which of the two numbers it is drawn in, and `bar.audio.step_percent` says how far one wheel notch moves the
volume. The step is read by the *service* rather than by the widget, because the distance a notch moves is a
distance in a value the daemon owns — a widget that computed the new level itself would be a second place the
volume is decided. A step of zero is refused by name in both places that have an opinion about it, and the
schema's copy and the service's are compared at compile time. There is no ceiling on the step: a step larger
than the range is a person asking for one notch to reach the end of it, which `steppedPercent` clamps to.

**The unit is a choice the file makes because a volume has two honest numbers, and this is where it differs
from the CPU readout, which has one.** `percent` is the desktop's own convention and the shell's default: the
cube root of the daemon's linear factor, which is what `wpctl get-volume` prints, so a bar showing it agrees
with every other volume control on the machine. `decibel` is the physical gain the same factor represents,
`20 * log10`, and it is not a spelling of the percentage: measured on this session, the sink whose Props
carry `0.074087` is reported by `wpctl get-volume` as `0.42` and by `pactl list sinks` as
`27525 /  42% / -22,61 dB` — 42% is the cube root, -22.61 dB is the gain, and the linear factor times a
hundred (7.4) is a third number that no tool on the desktop shows a person. Both are computed in the service
from the *same* factor, on the PipeWire thread, and cross to QML together, so a readout can never show a
percentage and a decibel value that came from two different readings. Silence is `-INFINITY` rather than a
floor — pulse's own printer passes `-INFINITY` through, so a zero volume prints as `-inf dB` there — which
the widget draws as `-∞ dB`; a floor would be a volume the sink is not playing at.

**The unit selects the step as well as the readout, and that was a change of mind worth recording.** It
began as a readout-only setting on the argument that a unit which re-scaled the wheel would make one gesture
mean two things. The measurement says the opposite is true. A percentage step is a *growing* distance in gain:
`60 * log10((p + s) / p)`, because the percentage is the cube root of the factor, so the default five points
is 1.28 dB at 99% and **46.69 dB at 1%** — thirty-six times as much at the bottom of the range as at the top,
which `audio-test` pins as arithmetic rather than prose. A person reading decibels was therefore being moved by
a number that depended on where they already were, which is the one thing a readout in a physical unit promises
not to do. So the token now decides both what is drawn and which of the two step keys a notch applies:
`step_percent` when the readout is in percent, `step_decibels` when it is in decibels, and the service holds
both because how far a notch moves is a distance in a value the daemon owns rather than in a setting the widget
could be trusted with. Nothing moves a different amount for the same reason any more — the step and the
reading are the same unit — and the two keys are separate rather than one rescaled value, because the same
key meaning two things depending on a third is a schema that lies about itself.

The decibel step is a *ratio*, not a subtraction: the factor the daemon holds is multiplied by
`10^(dB/20)`, and the shell writes that factor rather than a rounded percentage — a percentage-rounded write
would be 0.26 dB at the top of the range, a quarter of the decibel that was asked for. A step shorter than the
readout can draw is refused by name in both places that have an opinion, and the floor is the readout's own
resolution rather than a judgement about hearing: the bar draws decibels to one decimal place, so 0.1 dB is
the shortest notch a person could see, exactly as one point is the shortest the percentage readout can show.
Both ends of the range are handled by what the arithmetic actually does rather than by exception: past full
scale the result clamps, because a notch past the end of the range is a notch that is already there; a quieter
notch never *reaches* silence from a factor that is not silence, because a ratio does not reach zero — a
hundred-decibel notch from a factor of a ten-thousandth is `1e-9`, ten orders of magnitude below the quietest
level the percentage unit writes, and that is the honest thing to write to a daemon; and a louder notch from a
factor that really is zero moves to the quietest level the shell writes, one percent, because a ratio from
nothing has no starting point and inventing an audible one would be a level nobody asked for. A wheel notch
over the mute is the same case it always was: it changes the volume and leaves the sink muted, as
`wpctl set-volume` does.

**`audio-live-test` is the proof, and it is opt-in behind `QS_AUDIO_TESTS`.** It writes the distribution's
own daemon configuration into a scratch directory, patches the core name so its socket is a path the
session's shell can never reach, adds two null sinks and a `default` metadata object, starts the daemon
itself and stops it afterwards. Everything the shell is asked whether it noticed is done by `pw-cli` and
`pw-metadata` and read back by `pw-dump` — three other programs, so the writer, the reader and the code under
test are three things and not one agreeing with itself. What it pins: the reading arrives at all and matches
the daemon's own; an external volume change, an external mute, and a default-sink switch in both directions
all arrive as events (and the sink no longer followed does not); the shell's own write reaches the sink as
the cube of the percentage on *every* channel; the configured step and the clamp at full scale; a notch in the
decibel unit measured on the *daemon's* factor as a `10^(1/20)` ratio rather than on the shell's own number,
with the unit switched at run time to show which step key applies, and the dB floor and an unknown unit token
both refused with the previous value kept; that stepping
while muted changes the volume and leaves it muted, exacty as `wpctl set-volume` does; and that a daemon
going away withdraws the reading and a daemon coming back is reattached to — with no reading until the new
daemon names a default sink, because the module refuses to guess one.

Falsifying it turned up the module's one honest redundancy. Removing the `Props` subscription leaves the
suite green; removing the `info` re-enumeration leaves it green; removing *both* makes every external change
go unnoticed. Neither mechanism is redundant in general — they are two documented routes to the same param,
and in this daemon either alone is enough — so the pair is what is load-bearing rather than either half, and
that is written where the code is rather than left to be discovered by whoever removes one of them.

`audio-test` is the hermetic half described above. `bar-interaction-test` carries the widget: the readout
sitting in the trailing group after which the clock still keeps the corner, its dash while the service has no
reading, both halves of `show_volume` false (not drawn *and* taking no width, which a visibility check alone
would not see, because a hidden row keeps its own width), and the two gestures — a real click and a real
wheel delivered as window events, observed through the records the service writes while refusing to act with
no daemon behind it, which is the only place the bar's QML can be seen to call it. The gestures' *effects* are
`audio-live-test`'s, against a daemon.

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
traffic. A second case of that file — `theConfiguredSystemSettingsAreWhatTheRunningShellUses` — covers the
table whose values reach a *service* and a *widget* rather than a surface, since the composition root is
where that wiring lives and no unit test can call it: the shell is run against a file naming
`bar.system.sample_interval_ms = 750`, `show_cpu = false` and `memory_format = "percent"`; the running
shell is asked for each of those keys over the IPC, which is what makes the values attributable to the file
— and one key is deliberately *absent* from the file, because what an omitted key gives back is worth
reading on a shell and not only in a parse function; and then the record `SysMonService` writes when it
accepts a cadence is read out of the shell's own output, which is what shows the service was told rather
than left at the build's 2000 ms. The same case edits the file to 1000 while the shell runs and waits for
the second record, which is the other path — the watcher noticing, the schema re-reading and the service
being told again. What each setting then *draws* is not asserted here and could not be: this case reads a
socket and a transcript, and a pixel is `bar-interaction-test`'s, offscreen and per token. The rest of that
file runs the shell with an empty `XDG_CONFIG_HOME`, so a configuration its operator happens to have cannot
change what its assertions mean.

Reading those records is why that file starts its shell with `QT_FORCE_STDERR_LOGGING=1`: Qt sends records
to the journal when stderr is a pipe, as it is under `QProcess`, so without the variable the shell's own
account of what it did would not be in the transcript beside libwayland's protocol traffic. Removing it
was one of the falsifiers, and the case fails without it.

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
slowest shard's rather than the sum: measured here in one invocation of the split as it stands,
200.9 s, 207.3 s, 214.6 s and 226.6 s, where the four add up to about 849 s in sequence — the figures it
replaced, 188/196/210/214 s, belonged to the split before this landing corrected a cost-table entry that had
been carried over across two landings, and re-measuring a cost moves binaries between shards by design. Without `execution.jobs` ctest runs them one after another: still correct, and the
sum's wall clock rather than the slowest shard's, which is why the presets and the `check` target set it.
The balance those figures show is built from a measured pass cost per binary, and three of those measurements
were corrected in this landing — which is the argument for keeping the figures and the table side by side,
because a stale table produces an unbalanced split that fails nothing. `niri-event-stream-test` had grown to
883 ms a pass from the 359 ms recorded when it had fewer slots, `niri-ipc-test` to 1028 from 925,
`config-watcher-test` to 724 from 625 and `niri-actions-test` to 630 from 467; the split built from the stale
figures put 1,651 ms of work in one shard and 1,689 ms in another and then took 259 s and 187 s to run them,
where the re-measured table predicts 1,884/1,887/1,887/1,881 ms and the shards come out at 188–214 s.
`bar-interaction-test` was corrected twice, for two different reasons: it used to inherit
`QT_QPA_PLATFORM=wayland` from the session it was run in — the shell's own choice, not the test's — so a pass
cost 498 ms under a plugin it never meant to use and 345 ms once it forces the offscreen one, and then its own
configuration slots took it to 445 ms. Every one of those numbers is beside its entry in
`tests/CMakeLists.txt`.
`sysmon-test` and `bar-interaction-test` are what those figures are mostly made of, and they are the two
most expensive binaries there are — a pass of each costs 800 ms and 445 ms measured here, where most
binaries' passes cost tens of milliseconds. The reasons differ. `bar-interaction-test` builds a window and
loads the bar's QML into an engine before it can assert anything, so a single slot on its own, which is not
what the check does but is what the `-o` figures look like, costs 170–235 ms, most of it that same window
and engine: that is the price of a check covering what a gesture does rather than what a function returns.
`sysmon-test` waits instead — its cadence slots run real intervals in milliseconds, and its two slots that
read the real `/proc` wait for the kernel's own counters to advance — and a wait cannot be shortened below
the thing being waited for. Both are paid once per pass of the shard that holds the binary rather than by
all of them: the costs in `tests/CMakeLists.txt` are what the split is balanced from, and a binary without
a measured cost fails the configure rather than being placed on a guess. When a slot is added to either,
the shard holding it grows by that slot's own cost times the ninety-six passes rather than by a new window
— the arrangement's two slots cost their shard 5 s that way. It is safe to run them
together because the two tests that act on the desktop take a resource lock, so no two of them are ever
changing what is on screen at once.

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

A third kind of finding, and the first one here that was neither an order dependence nor the desktop
racing a live read. A full run of the suite failed once with `slot-order-randomised-shard-1`, naming
`BarInteractionTest::theVolumeReadoutFollowsItsConfiguration` and reporting a group width of 78.765625
against the 78.296875 the slot had expected. That delta is 30/64 px, and it was not a layout defect: the
slot hides the volume readout, puts it back, and asked whether the trailing group's width had come back
to the number it had remembered at the top — while the group it asked about holds the clock as well.
`Inter` does not resolve to a tabular-figure font on this machine, so `HH:mm` is a string whose width
depends on the time: measured over every minute of a day it runs from 28.28125 px at "11:11" to 37.0625
px at "06:06", no two consecutive minutes have the same width, one minute's change is anywhere from a
quarter of a pixel to 2.3 px, and the set of those changes contains 30/64 — the figure the failing run
printed. The assertion was therefore a claim that the clock had stood still across the round trip, false
whenever a minute boundary fell inside it, and the shuffle is what made it visible: a quiet pass takes
0.7 s and almost always misses the boundary, where the failing pass took 15.7 s — so that pass had bought
the tick along with the order. It reproduces from the printed seed only sometimes, which is the honest
description of a race against the clock rather than against the desktop — and unlike the earlier two
sightings, the mechanism was established by measurement rather than by reading the failure text.

The fix is in the assertion, not in the clock. Every group width in that slot is now compared with what
the group holds *at the moment of the check* — the widget's own width, the group's spacing, and the
clock's width read in the same expression, so the clock cannot tick between them — and while the widget
is hidden the assertion is that the group is exactly as wide as the clock, which is what a positioner
does with an invisible child and what a hidden readout depends on. The honest limit of that is written
where it is used: a positioner's width is its contents, so the comparison asks whether the widget is
counted among them, which is the fact the flag controls. Three falsifiers say it still bites: the tick
against the assertion as it was fails 10 runs in 10, and the widget staying visible when hidden, and the
widget keeping its width while hidden, each still fail. The clock is moved on by a minute by the slot
itself, so the property is exercised on every run instead of waiting for a boundary: a test whose
clock-independence is only shown by the absence of a rare tick is the same hole that let this in. The
96 orders the failing seed gives that binary were then replayed four at a time, 96/96, and the full suite
ran green twice — once with `QS_TEST_ORDER_SEED=184021293` pinned and once with a fresh seed.

Because the defect was a *kind* of assertion rather than a widget, the kind was swept for rather than fixed
where it was found. The class is: a value read from a live source at one moment and compared, for equality,
after a delay that lets the source move. The sweep was mechanical first — every local bound to a call and
then compared after a `QTRY`, a `qWait` or a loop — and then read one hit at a time, because a hop from a
value to an assertion is not the same thing as a hazard. Two more instances turned up, both in
`bar-interaction-test` and both here for the same reason the system status needs its waiver:
`SysMonService` really samples `/proc` on the cadence the configuration gives it, so its readings move
between two lines of a slot. The memory-form slot built its four expectations once and compared them across
a loop spanning samples of a 30 ms timer; the empty-state slot built the memory text with
`QString::number` rather than the widget's rule. The hazard was measured the way the clock's was: allocating
and touching 0.68 GiB inside the slot moves `MemAvailable` by 0.6687 GiB — seven steps of the
tenth-of-a-GiB digit the readout draws — the remembered comparison fails on it, and the fixed one passes.
Expectations for these readouts are now functions of the service, computed at the comparison, in one place
for both slots.

That slot had one more thing to teach, and it is a fact about Qt Quick rather than about the clock: a
positioner's own size is not a binding. The trailing group's width follows its children's widths when it next
lays out, which is the next frame, and its `implicitWidth` lags with it — measured by moving the clock's text on
a minute and reading at once, the clock widened immediately (33.671875 against 33.515625, 10/64 px) while the
group and its implicit width stayed at 78.09375 through `qWait(1)`, and both were 78.25 once a frame had gone
through. So the one comparison in the slot that read the group synchronously — the other three already waited —
asserted that no frame was pending between the clock's text changing and that line, which is false on a pass
slow enough to cross a minute boundary. It failed at 79 against 79.2344 in the shuffled check, one minute's
change at a different hour. Every group comparison is now waited for, and the property is exercised on every
run rather than left to the wall clock: with the clock moved on immediately before the comparison the naked form
fails and the waited one passes, both deterministic, where the shard's failure needed a boundary to fall inside
a slow pass. Waiting does not weaken the claim — the group agrees with its contents once the layout has settled,
which is the fact a hidden readout depends on — and the seed that failed re-runs green.


The third instance was not in a unit binary. `niri-live-action-test`'s case for an action the compositor
cannot parse remembered the overview state, waited 150 ms and compared — and this session's default config
binds the overview to a key, so a person pressing it in that window failed the case and named the shell for
something the person did. It now asks the compositor when the two differ, which is the discriminating
question rather than a relaxation: the refused request is the only thing the shell sent, so a model that
agrees with a compositor that moved was told about the change, and a model that disagrees has moved on its
own and still fails. That is where this class's boundary is drawn: the acting cases whose *claims* need the
desktop to hold still — that the overview opened because the shell asked it to — are left as they are,
because reporting a desktop that moved instead of failing would let a shell that did nothing pass, and the
opt-in flag and the resource lock are what make those runs meaningful. The counts that assert nothing
arrives after a stop, the test double the unit tests talk to, `audio-live-test`'s private daemon and
`config-watcher-test`'s own files were each read and left, and the reasons are in the report rather than in
a comment apiece.

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

The last two, `niri-live-restart-test` and `niri-live-shell-restart-test`, need `QS_NIRI_RESTART_TESTS`
as well as `$NIRI_SOCKET` — the two opt-ins are independent — and they are separate for a bigger reason:
they start a niri of their own nested in the session, kill it, and start another. The first checks that
the connection layer found the new socket, reattached and rebuilt its state without being told. The
second drives the built shell binary across the same shape of outage, and it exists because restart
survival for a Wayland client cannot mean reattaching a display: a Wayland connection that loses its
compositor is finished, so the design's rule that `QGuiApplication` exits when the display drops is the
shell's half of recovery, and a fresh start into the new session is the other half. The test proves that
end to end — a bar confirmed in one nested compositor's layer list and its `qsctl state` reporting that
session; the compositor stopped and the shell's actual death observed rather than assumed; a second
compositor, which by niri's socket-naming rule cannot reuse the path; and a second shell started the way
a session manager would start it, whose bar is listed in the new compositor and whose `qsctl state`
agrees with the new session's own workspaces answer.

What the death assertion pins is measured, not hoped for: the shell ends within milliseconds of the
compositor stopping (20–23 ms across four runs), is killed by no signal, and its abstract IPC socket is
released with it — a corpse holding the frozen name would otherwise answer the next session's `qsctl`.
The exit status is 1, and the test asserts that on purpose rather than tolerating it: a session
supervisor configured to restart on failure brings the shell back because it reported failure, while a
shell that reported success would be lying about its session. These are the facts from the runs, and
which display connection initiates the exit — Qt's own or the GTK platform theme's, whose
`Gdk-Message: Error reading events from display: Broken pipe` was in the first run's stderr — is not
pinned, because the observable contract is what the shell owes. It skips, with the reason, when there
is no niri on `PATH`, no Wayland session to nest in, or no `$NIRI_SOCKET` to tell its own compositor
apart from the session's; it takes the same resource lock as the other tests that change the desktop.

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

### The engineering spec's numbers (`spec-values-test`)

`ENGINEERING_SPEC.md` is derived from the code, which is what makes it useful and what makes it go stale:
every value in it — a version floor, a default, a bound, a backoff, a cap, a test count — is a claim about
something else, and a claim that is wrong is the "file that lies" this repository's rules are written
against. Its names are guarded by `public-names-test`, which reads that document like the other two, and its
numbers by `spec-values-test`, which reads it and compares what it says with the values the libraries were
*compiled* from.

The second one is deliberately not a mirror. A mirror repeats a number and compares the constant with the
copy, so it catches a change to the constant and nothing else; this compares the document with the constant,
so a change on either side fails. It links `quantum-shell-config`, `-system`, `-audio` and `-ipc` rather
than parsing headers, for the reason the config keys are mirrored by a test instead of by a comment: a
constant that was renamed or moved cannot be read out of a header by a regular expression that still
matches, it stops the build. What it cannot reach is stated at each case rather than left to look equal —
the `/proc` file cap and the audio retry pair are declared in a `.cpp` file, where nothing can link them,
so those are read from the source text with the exact declaration required, which catches a changed value
and not a constant moved elsewhere. And what no test can read at all is a sentence: a behaviour, a
limitation, a budget that is prose, is read by people, and the ones that matter have a test of their own.

It also holds the §2.6 singleton tables, in both directions and from the strongest source each row admits.
Seven of them are read from the meta-object of the class that declares them — the meta-object is what the
engine resolves a binding against, so it is the spelling that matters when a `Q_PROPERTY` is renamed — and
for each of those every property and every `Q_INVOKABLE` the class declares itself must appear in its row
while every name in its row must exist. The bar window, `LayerShellWindow`, is read from the class's own
`Q_PROPERTY` and `Q_INVOKABLE` declarations, and the reason is a measurement rather than a preference: the
class derives from `QQuickWindow`, so linking it into a test whose subject is a document makes the
sanitizer job report the font stack's process-lifetime fontconfig caches as leaks of that binary — 722474
bytes in 16702 allocations, every frame in libfontconfig or libpangocairo and none in this repository —
where the same binary without that link exits clean. It also costs a measured 72–74 ms a pass against
13–15 ms without, which is what that library being loaded is worth. What that header reading gives up is
written where it reads: a `Q_PROPERTY` renamed, added or removed in the declaration fails, and a
disagreement between a declaration and what moc built from it cannot be seen — a thing moc does not permit,
but the weaker half does not pretend to prove it. The row's "each with NOTIFY" is checked signal by signal,
and the counts the `Config` row states for the two nested tables — `bar.system.*` (4), `bar.audio.*` (2) —
are compared with the properties those objects declare. That row is held in both directions too, over the
names it writes as `bar.<name>` and `bar.<name>.*`: every property the object `bar` declares has to appear
in it and every name in it has to be one, so a new `[bar.*]` table landing in the configuration with no row
to describe it fails here instead of being a set of keys nobody is told about. What that cannot see is a *slot*: this project's
QML-facing calls are `Q_INVOKABLE` on purpose, and the check treats a slot as not part of the surface rather
than pretending otherwise.

The document is read from the source tree the binary was configured against, named at configure time the
way `bar-interaction-test` takes the shipped QML, and the configure fails if any file it reads is missing.
That is the point of it: the four falsehoods found when the document was first audited — a count off by
one, three stale sentences about landed work — were all things a person had to notice, and two of the
four are now things a test notices instead.

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
  gate by deleting or renaming the code it was meant to check. `tools`, `CMakeLists.txt`, `src`, `qml`
  and `tests` are all on that list, and the CI job's own read-only run names the same five so the
  legible copy and the failing one cannot disagree about what must exist.
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

The repository is published (`origin`), so the workflow is real rather than aspirational, and it has
been run — but GitHub-hosted minutes are blocked on this account, so both jobs' steps are run on this
machine instead, against the tree in `build/ci`: `CC=clang CXX=clang++` with `cmake --preset ci`,
`cmake --build --preset ci` and `ctest --preset ci`, and `cmake --preset asan` with its build and
ctest. That is not a workaround for an untested workflow: running the steps locally is what found the
matrix's own setup error, since `CC` has to be the C compiler matching the matrix's C++ one and a
`CC=clang++` job fails at configure before it builds anything. It also found something a green GCC
tree had hidden: the audio module's pod construction used SPA's vararg macros — compound literals and
GNU statement expressions — which clang refuses under `-Werror`, so the clang job was failing to build
`src/audio/` while `ctest --preset dev` stayed green. Both now build with SPA's own builder functions
instead, and `ctest --preset ci` passes 33/33 on this machine (the count moved as modules landed; the
network module added its own test and its own read-only live test, and the read-only one needs no opt-in).

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
without restarting, and responds to `qsctl`; and it survives a compositor restart. The survival was the
count left open longest — first proven for the connection layer alone (`niri-live-restart-test`
reattaching its own connections), then through the running shell
(`niri-live-shell-restart-test`, described with the live tests above): a bar in the compositor before,
an observed death when it stops, and a bar and a correct `qsctl state` in the compositor after. The bar's
route is in that list no longer either: `niri-live-layershell-test` covers it — the surface in
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

**Started.** The workspace strip is interactive: a click focuses the workspace a capsule names, and the
wheel over the strip moves to the workspace below or above. Both go through the shell's actions,
registered for QML as the `NiriActions` singleton beside the state service — see § Exposed QML API,
where the gestures and the reason a click on the already-focused capsule asks for nothing are written
down. The C++ side is `bar-interaction-test` (the shipped `qml/Bar.qml` in a real engine, offscreen, with
real click and wheel events read back as the requests niri would receive) plus the new case in
`niri-live-action-test`, which confirms `FocusWorkspaceUp`/`FocusWorkspaceDown` against a running niri
26.04.

**Capsule groups are in**, as the bar's arrangement rather than as a container added for its own sake:
`qml/CapsuleGroup.qml` is a named group, the bar declares three of them — left, centre and right — and a
widget is placed by being declared in the group it belongs to, carrying no coordinate or anchor of its own.
The group gives every widget of it the group's height, enforced in the component rather than described, so
widgets of different natural sizes line up without any of them knowing where it is — see § Exposed QML
API, where the convention and the naming are written down, and where the rule that a group arrives with
the widget that belongs in it is what the centre one did.

**The system status is in**, as the centre group's first occupant: CPU and memory read from `/proc` through
`src/system/SysMonService.cpp` and drawn by `qml/SystemMonitor.qml`. It is the one module whose reading has
a clock of its own, and the difference from every other reading in the shell is that the kernel holds no
CPU percentage at all — only cumulative counters — so the reading is a difference between two moments and
`cpuAvailable` is false until one exists. Its settings are the configuration's rather than constants —
`[bar.system]`'s cadence, drawn readouts and memory form, the shell's first nested table, with the cadence
applied by the composition root at startup and followed on every edit, and the rest bound in the widget
itself — and the service writes one record per accepted interval, which is what the live case in
`niri-live-layershell-test` reads back. See § System statistics for the three
decisions inside it, for the empty state the widget draws instead of a zero, and for why this is the
single sanctioned departure
from § Event-driven.

**The volume readout is in**, as the trailing group's first occupant and the phase's first reading with a
real event source: `src/audio/PipeWireService.cpp` follows the sink the `default` metadata names and
subscribes to its Props, so nothing in the module polls and nothing about it needs the waiver the system
status does. See § Audio for the cube-root convention `wpctl get-volume` prints, the settings that are the
configuration's (`show_volume`, `volume_scale` — because a volume has two honest numbers and the file decides
which one is drawn and which one a notch is measured in — and the two steps, `step_percent` and
`step_decibels`), and the private-daemon test that proves it. Like the system status's cadence, the audio
service writes one record each time it accepts a step or a unit — `a wheel notch moves the volume by N dB,
which is the step the decibel readout uses`, and `the volume readout and the wheel's step are both in
percent` — because a running shell's wheel is otherwise invisible from outside the process; the live case in
`niri-live-layershell-test` reads those back out of a shell it started, at startup and after a live edit,
which is the composition root's wiring and the one thing no unit test can call.

**The network readout is in**, as the counter-example that costs nothing to make: unlike the volume it is
the same shape one daemon over, and it still needs no waiver. `src/dbus/NetworkService.cpp` finds
NetworkManager's name on the system bus and follows `PropertiesChanged` — first on the manager, then, for
whichever objects the manager's own answers name, on the active connection, the device behind it, its
access point and the daemon's own connectivity. What the readout needs is a decision per property rather
than a number: the chain is named by the daemon instead of guessed, so the module never assumes Wi-Fi,
never assumes an access point exists, and reports no signal strength rather than a zero when the device
has none to give. Its settings are the configuration's — `[bar.network]`'s three booleans, whether the
readout is drawn and whether the name and the signal are drawn beside it — and its two halves are
`network-test`, whose service half runs against a NetworkManager test double that owns the name on a
`dbus-daemon` the test starts itself, and `network-live-test`, which compares the module's reading against
what `busctl` prints for the same objects on the desktop's own daemon.

**Battery and media are implemented.** The battery readout follows UPower; the media readout
follows MPRIS players on the session bus and draws the selected player's title and artist.
`media-test` covers second-player selection and a newer signal surviving a delayed initial reply
on a private bus. The shipped bar was also observed rendering real VLC metadata on niri.

**Idle measurement, 2026-09-17:** the Release binary (`build/release/quantum-shell`), with the
bar visible on DP-3 and the default configuration, consumed 15 CPU ticks at 100 Hz over
60.0348 seconds: **0.250% of one core**. `/proc/<pid>/stat` supplied user + system ticks;
Python's monotonic clock supplied elapsed time. `/proc/<pid>/status` reported VmRSS rising
from **172492 to 173548 KiB** (168.4 to 169.5 MiB). This is process RSS, not private memory
or GPU memory. Machine: Ryzen 7 5800XT, RTX 4060 Ti, Linux 7.2.6-1-cachyos, Qt 6.11.2;
live niri session, with other desktop applications and the remainder of the Release build running.
`niri msg --json layers`
confirmed `quantum-shell-bar` on the Top layer before sampling. The probe was stopped afterward.
The earlier Debug sample was 0.367% over 60 seconds, with endpoint RSS 174504 KiB.

**Phase 1 remains open:** CPU meets the <1% budget, but Release RSS exceeds the <150 MB
budget. Memory attribution and reduction are the next engineering work; a Release build did
not remove the excess. This single interval does not establish long-term memory stability.
The system-sampling waiver below also remains unresolved; no rule or budget was changed.

**Exit criteria:** a fully functional daily-driver bar; no polling; workspace changes are instant
and event-driven; idle CPU budget met. The performance row's "no polling loops" is met in the sense the
shell reads it — nothing polls the compositor, and the one cadence in the shell exists where the kernel
offers no alternative and is gated on the bar being on screen — but it is not met literally: see § System
statistics for the exception and the waiver it needs.

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
