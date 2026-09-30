# Quantum Shell — Engineering Spec

**Status:** derived from source at `main f89873f` plus the uncommitted notification
landing observed in `git status` (new `src/dbus/NotificationService.h/.cpp`,
`qml/Notifications.qml`, `tests/unit/notification_test.cpp`,
`tests/support/NotificationBus.h`, `tests/fixtures/notification-bus/`; `Config`, the
logging categories, `main.cpp` and `Bar.qml` changed with it). Contracts below are
stated from the code as read; live/compositor behaviors were not re-executed while
deriving this file and are marked where they rest on the listed live tests. What was
measured rather than read is the bus's own vocabulary: which of Qt's three
`registerService` replies means what, one case at a time against a `dbus-daemon` of a
test's own — a free name and a name this connection already owns both answer
`ServiceRegistered`, a name another connection holds answers `ServiceQueued` — which is
what makes a queued shell publish itself the moment the bus hands it the name.

**Authority order:** `SYSTEM_PROMPT.md` (behaviour) › `QUANTUM_SHELL.md` (design) ›
`AGENTS.md` (repo state) › this file (frozen surface + contracts, derived). This file
adds no rules and waives none. Where it disagrees with code, code wins and this file
is stale — fix it in the same change. Where it disagrees with the three documents,
§8 names the drift.

---

## 1. Version floors and toolchain

| Requirement | Floor | Pinned / tested |
| --- | --- | --- |
| Qt | 6.11.0 | 6.11.2 in CI; `find_package(Qt6 6.11 ...)` fails configure below it |
| niri | 26.04 | `NiriVersion::minimumYear/Month = 26/04`; verified against niri-ipc v26.04 |
| C++ | C++23, no extensions | `CMAKE_CXX_STANDARD 23`, `CXX_EXTENSIONS OFF` |
| C | enabled for wayland-scanner output only | generated `*-protocol.c` is the only C in the build |
| CMake | 3.31 | 4.4.3 in CI |
| toml++ | 3.4 | installed ≥3.4 or fetched tag `v3.4.0`; `QUANTUM_SHELL_SYSTEM_DEPS=ON` forbids fetch |
| PipeWire | libpipewire-0.3 ≥ 1.6 | `pkg_check_modules` fails configure below it; run-time re-check warns |
| WirePlumber | 0.5 | owns `default.audio.sink`; design dependency, not linked |
| wayland-protocols | staging | XML vendored under `src/wayland/protocols/` |

Warnings are policy, not advice: `quantum-shell-warnings` carries `-Wall -Wextra
-Wpedantic -Werror -Wshadow -Wcast-qual -Wold-style-cast -Wnon-virtual-dtor
-Wmissing-declarations` on GCC/Clang. Generated protocol code is exempt by living in
its own target. Pending user-only decision: whether the Qt floor moves to 6.12 LTS
when it ships. Agents never pick this.

---

## 2. Frozen public surface

Renaming anything here breaks scripts, QML files, or clients. Approval required.

### 2.1 Layer-shell namespaces

Prefix `quantum-shell-` (`LayerNamespacePrefix`, mirrored in
`LayerShellIntegration.cpp`). Six surfaces today: `quantum-shell-bar` (the bar,
`qml/Main.qml`), `quantum-shell-toast` (the toast, `qml/Toast.qml` — a literal
in the QML rather than a configured key, because a notification is a surface of its
own kind and not a readout's setting) and `quantum-shell-notification-history` (the
history panel, `qml/NotificationHistory.qml`, likewise a literal; one surface on the
primary output while `notificationHistoryOpen` is true, 400 wide and as tall as its
content up to 520, keyboard interactivity none, no exclusive zone) and
`quantum-shell-osd` (the volume display, `qml/VolumeOsd.qml`, likewise a literal; one
surface per output while an adjustment is on screen, overlay layer, anchored to the
bottom edge alone so the compositor centres it, 300×132 (a 72 px panel above a 60 px
transparent strip that lifts it off the edge), keyboard interactivity none, no exclusive zone)
and `quantum-shell-launcher` (the launcher, `qml/Launcher.qml`, likewise a literal; one
surface on the primary output while `LauncherService.open` is true, overlay layer,
anchored to nothing so the compositor centres it, 600 wide and as tall as its results
up to `max_results` rows, **exclusive keyboard** while it is up, no exclusive zone) and
`quantum-shell-control-center` (the control centre, `qml/ControlCenter.qml`, likewise a literal; one
surface on the primary output while `ControlCenterService.open` is true, overlay layer, anchored top and
right so it sits below the bar, 360 wide and as tall as its three sections, keyboard interactivity
**on demand** — it takes keys once clicked, and Escape closes it — no exclusive zone) and
`quantum-shell-calendar` (`qml/Calendar.qml`, likewise a literal; one surface on the primary output while
`CalendarService.open` is true, opened by a click on the bar's clock, overlay layer, anchored top and right so it sits below
the bar, keyboard interactivity **on demand**, no exclusive zone; a month grid of the system's own date with today marked,
no events) and
`quantum-shell-backdrop` (`qml/Backdrop.qml`, likewise a literal; the transparent surface behind the launcher,
the control centre and the notification history, one on the primary output while any of them is open, **top
layer** so the panels' overlay layer is above it by the protocol's own order, anchored to all four edges with an
exclusive zone of −1 so it covers the whole output, keyboard interactivity none; a press on it closes the panel
through that panel's service, which is how a click outside the panel dismisses it).
Enforced twice:
schema refuses a configured value outside the prefix; integration refuses the surface
(no role, record on `quantum.shell.wayland`).

### 2.2 Local IPC transport

- Address `\0quantum-shell` (abstract). Qt spelling `SocketName = "quantum-shell"` with
  `AbstractNamespaceOption`; a literal NUL in the string binds `@@quantum-shell`, a
  different socket nothing connects to.
- Frames: newline-delimited JSON, one request line → one response line.
- Every frame carries `version`. A version-only frame is the handshake, decoded as a
  `version` request. `ProtocolVersion = 1`; bumped only when an existing frame changes
  meaning, never for a new verb.
- `MaxLineBytes = 64 KiB`; oversize line or unterminated buffer → refuse and close.
- Peer identity: an abstract socket has no permission bits, so every accepted connection
  is checked against the kernel's record of the connecting process (`SO_PEERCRED`) and a
  uid other than the server's own effective uid (`IPCServer::permittedUid()`) is closed
  before a byte is read — no answer, no refusal frame, one warning on
  `quantum.shell.ipc`. A connection whose uid the kernel will not report is refused the
  same way. A closed connection's socket is deleted with its helper (`openConnections()` is
  the count of client sockets the server holds). `setPermittedUid` exists so the refusal is testable without a second user.
- Non-JSON / non-object / version mismatch → refuse naming the problem and close.
  Unknown verb, missing argument, unknown config key → refuse by name, connection stays
  open.

Request shape: `{"version":1,"verb":"state"}` plus `path` for `config get`.
Response shape: success `{"version":1,"ok":true,"data":{...}}` (`data` present even when
empty); refusal `{"version":1,"ok":false,"error":"<reason>"}` (never blank).

### 2.3 IPC verbs (the whole surface: `verb::All`, 6 entries)

| Verb | Answers with | Source |
| --- | --- | --- |
| `version` | `name`, `shell` (both from the build, not retyped), `protocol` | app identity |
| `state` | `workspaces`, `focusedWindow`, `outputs`, `keyboardLayout`, `overviewOpen`, `connected` — `NiriService` property names verbatim | `ShellCapabilities::state` |
| `config get <path>` | `path`, `value` | `configValueForPath` on validated values |
| `control-center toggle` | `open` (post-toggle state) | `ControlCenterService::toggle`, the state the panel itself follows; a shell whose panel did not load answers `false` |
| `launcher toggle` | `open` (post-toggle state) | `LauncherService::toggle`, the object the launcher's own surface follows: opens it if closed and closes it if open; a shell whose launcher did not load answers `false`. The verb a niri key binding runs (`spawn "qsctl" "launcher" "toggle"`) |
| `bar toggle` | `visible` (post-toggle state) | every bar's own visibility, moved together: hides them all if any is showing and shows them all if none is; no bars at all answers `false` |

### 2.4 `qsctl` CLI

`version`, `state`, `config get <key>`, `bar toggle`, `launcher toggle`, `control-center toggle`. `config get` prints the bare
value (for `x=$(qsctl config get bar.height)`); all others print one JSON line.
Refusals go to stderr, answers to stdout. Exit codes are interface:
`0` answered · `1` refused · `2` bad command line (nothing sent) · `3` no shell
listening / no answer (2 s connect, 5 s answer timeouts) · `4` protocol mismatch (both
versions named). Unknown words (`qsctl volume up`, bad `bar`/`config` subcommands) are
client-side usage errors naming the words — never sent to the shell.

### 2.5 Configuration keys

File: `$XDG_CONFIG_HOME/quantum-shell/config.toml` (`~/.config/...` fallback).
`schema_version` must equal `SchemaVersion = 1`.

| File spelling | Key path (`qsctl config get`) | Default | Bounds / rule | Live? |
| --- | --- | --- | --- | --- |
| `schema_version` | — | 1 | missing → warn, assume current; mismatch / non-int → whole file rejected, previous config stands | — |
| `theme` | `theme` | `""` (no theme) | a string: a bare name (letters, digits, `-`, `_`) read from `themes/<name>.toml` beside `config.toml`, or an absolute path; anything else is refused by name and no theme applies. The theme's `[bar.colors]` and `[bar.font]` are the base the file's own tables are laid over — a value `config.toml` writes itself wins. A theme that is missing, not TOML or from another `schema_version` is a warning and applies nothing | yes — editing the key, or the theme file it names, re-applies the colours and font with no restart |
| `[bar] height` | `bar.height` | `32` | int `1..INT_MAX`; also the exclusive zone | yes, resizes on screen |
| `[bar] namespace` | `bar.layerNamespace` | `"quantum-shell-bar"` | must start `quantum-shell-` | no — role assigned once; change warns, takes effect next start |
| `[bar.system] sample_interval_ms` | `bar.system.sample_interval_ms` | `2000` | int `10..INT_MAX` (floor = one `USER_HZ` tick; service refuses the same floor) | yes, re-arms now |
| `[bar.system] show_cpu` | `bar.system.show_cpu` | `true` | strict boolean, no coercion (`1`/`"no"` refused) | yes |
| `[bar.system] show_memory` | `bar.system.show_memory` | `true` | strict boolean | yes |
| `[bar.system] memory_format` | `bar.system.memory_format` | `"used_of_total"` | one of `used_of_total`, `used`, `available`, `percent`; unknown token refused by name | yes |
| `[bar.audio] show_volume` | `bar.audio.show_volume` | `true` | strict boolean | yes |
| `[bar.audio] volume_scale` | `bar.audio.volume_scale` | `"percent"` | one of `percent`, `decibel`; unknown token refused by name; selects the readout unit *and* which step a notch applies | yes |
| `[bar.audio] step_percent` | `bar.audio.step_percent` | `5` | int `1..INT_MAX`; zero/negative refused by name in schema and service; no ceiling (result clamps); applies in the `percent` unit | yes |
| `[bar.audio] step_decibels` | `bar.audio.step_decibels` | `1.0` | number `0.1..∞` (both `2` and `2.0` read); below the floor refused by name in schema and service; no ceiling (result clamps); applies in the `decibel` unit | yes |
| `[bar.network] show_status` | `bar.network.show_status` | `true` | strict boolean | yes |
| `[bar.network] show_name` | `bar.network.show_name` | `true` | strict boolean | yes |
| `[bar.network] show_strength` | `bar.network.show_strength` | `true` | strict boolean | yes |
| `[bar.battery] show_status` | `bar.battery.show_status` | `true` | strict boolean | yes |
| `[bar.battery] show_percentage` | `bar.battery.show_percentage` | `true` | strict boolean | yes |
| `[bar.battery] show_time` | `bar.battery.show_time` | `true` | strict boolean | yes |
| `[bar.media] show_media` | `bar.media.show_media` | `true` | strict boolean | yes |
| `[bar.notifications] show_notifications` | `bar.notifications.show_notifications` | `true` | strict boolean | yes |
| `[bar.notifications] timeout_ms` | `bar.notifications.timeout_ms` | `5000` | an integer of at least 500 ms, or the spec's `0` (never expire); anything else below the floor refused | yes, and it is the value a sender's `-1` resolves to |
| `[launcher] max_results` | `launcher.max_results` | `8` | an integer from 1 to 50 (`MinLauncherMaxResults`, `MaxLauncherMaxResults`); outside it, or not an integer (a boolean is not one), refused by name and the default kept | yes; the next query uses it |
| `[bar.osd] show_osd` | `bar.osd.show_osd` | `true` | strict boolean | yes; switching it off takes down a display that is up |
| `[bar.osd] timeout_ms` | `bar.osd.timeout_ms` | `1500` | an integer of at least 500 ms (`MinOsdTimeoutMs`); below it, or not an integer, refused by name and the default kept — there is no "never" for a volume bar | yes, the next adjustment uses it |
| `[bar.colors] foreground` | `bar.colors.foreground` | `"#c8cad8"` | colour literal: `#` and 6 hex digits, or 8 with the alpha first; anything else refused by name | yes, repaints what reads it |
| `[bar.colors] muted` | `bar.colors.muted` | `"#5a5d70"` | colour literal, same rule | yes |
| `[bar.colors] accent` | `bar.colors.accent` | `"#7aa2f7"` | colour literal, same rule | yes |
| `[bar.colors] urgent` | `bar.colors.urgent` | `"#f7768e"` | colour literal, same rule | yes |
| `[bar.font] family` | `bar.font.family` | `"Inter"` | a non-empty string; an empty one refused, since `sans-serif` is how a file says "whichever" | yes |
| `[bar.font] size` | `bar.font.size` | `12` | an integer from 1 up, the floor `bar.height` has and for the same reason | yes, reflows every readout |
| `[bar.font] weight` | `bar.font.weight` | `400` | an integer from 100 to 900, Qt's own weight scale | yes |

Rules: unknown keys warn with full path; missing keys keep defaults; wrong type or
refused value warns naming key, value found, and value kept (never silent, never
coerced). Unusable file (non-TOML, bad `schema_version`) applies nothing.

### 2.6 QML singletons (module `QuantumShell 1.0`, one place: `QmlModule.h`)

| Name | Properties / calls | Notes |
| `Config` | `theme`, `launcher.maxResults` (1), `bar.height`, `bar.layerNamespace`, `bar.system.*` (4), `bar.audio.*` (4), `bar.network.*` (3), `bar.battery.*` (3), `bar.media.*` (1), `bar.notifications.*` (2), `bar.osd.*` (2), `bar.colors.*` (4), `bar.font.*` (3); per-leaf NOTIFY; `bar`/`launcher`/`system`/`audio`/`network`/`battery`/`media`/`notifications`/`osd`/`colors`/`font` objects CONSTANT | no engine reload, ever |
| `NiriService` | `workspaces`, `focusedWindow`, `outputs`, `keyboardLayout`, `overviewOpen`, `connected` — each with NOTIFY, emitted only on real change | absent = empty map/list, never plausible zero; ids as text |
| `NiriActions` | `focusWorkspaceById(idText)`, `focusWorkspaceUp()`, `focusWorkspaceDown()`; signal `actionFailed` | every non-`handled` outcome also logged |
| `SysMonService` | `cpuPercent`, `cpuAvailable`, `memoryUsedKb`, `memoryTotalKb`, `memoryAvailableKb`, `memoryAvailable`, `active`, `sampleIntervalMs` | `cpuPercent` 0 while `cpuAvailable` false; read the flags |
| `PipeWireService` | `available`, `muted`, `volumePercent`, `volumeDecibels`, `stepPercent`, `stepDecibels`; `toggleMute()`, `stepVolume(dir)`, `setVolumePercent(p)` | both numbers 0 while `available` false; read the flag. `volumePercent` = cube root of the linear factor ×100, `volumeDecibels` = 20·log₁₀ of the same factor (both computed from one reading; neither derives the other). `stepVolume` applies whichever of the two steps the unit selects; the unit itself is a C++ setter, deliberately not a property (no binding has a use for it) |
| `LauncherService` | `open`, `query`, `results`, `selectedIndex`, `maxResults`, `applicationCount`, `scanning`; `toggle()`, `moveSelection(delta)`, `launchSelected()`, `launch(index)` | `open` and `query` are writable (the surface's Escape and its text field write them). `results` is a list of `{id, name, comment}` maps, best match first, at most `maxResults`; `selectedIndex` is 0 whenever the results change and -1 when there are none. `applicationCount` is what the last finished scan offered before the query; `scanning` is true while one runs. The scan reads the XDG `applications/` directories on a worker thread each time the launcher opens (never on a timer), a user's copy shadows the system's and a user's `Hidden=true` deletes an entry; terminal applications and entries whose `TryExec` is not installed are not offered. A launch is the entry's `Exec` split into an argument vector (no shell; field codes removed, `%%` and `%c` resolved) and started with `QProcess::startDetached` from the home directory; one that cannot start is a record on `quantum.shell.launcher` and leaves the launcher open. A successful launch is also recorded in `$XDG_STATE_HOME/quantum-shell/launcher-history.json` (`~/.local/state` when unset; the shell's own state, not configuration: no key, no `schema_version`) as `{desktop file ID: {count, last}}`, written atomically on a writer thread, read on the scan's worker; among equally good matches the higher recency-weighted count (100/70/50/30/10 by age within 4 days/2 weeks/1 month/3 months/older, times the count) comes first, then the name, and it never outranks a better match. A file that is not that format is refused whole with a record and replaced by the next launch |
| `CalendarService` | `open`; `toggle()` | `open` is writable (the panel's Escape writes it) and is the only fact this service owns: what the panel shows is the system's date, read by the QML. It is toggled by a click on the clock and followed by `CalendarHost` |
| `ControlCenterService` | `open`; `toggle()` | `open` is writable (the panel's Escape writes it) and is the only fact this service owns: what the panel shows and does belongs to `PipeWireService` (volume, mute), `NotificationService` (Do Not Disturb) and `MediaService` (transport). It is toggled from the IPC (`control-center toggle`) and followed by `ControlCenterHost`, which creates and destroys the surface |
| `NetworkService` | `available`, `state`, `connectionName`, `interfaceName`, `deviceKind`, `hasStrength`, `strength`, `connectivity` | `state`/`deviceKind`/`connectivity` are tokens (`disconnected\|connecting\|connected`, `wifi\|ethernet\|other`, `none\|portal\|limited\|full`); `connectivity` is empty when the daemon has not said, which is not `full`. `strength` 0 while `hasStrength` false — read the flag. No `Q_INVOKABLE`: the shell has no control centre to open, so there is no gesture to offer |
| `BatteryService` | `available`, `present`, `onBattery`, `hasPercentage`, `percentage`, `state`, `warning`, `hasTimeRemaining`, `timeRemaining` | all 0/empty/false while `available` false; `percentage`/`timeRemaining` 0/empty while their `has*` flag false; `present` false on desktop (no battery). `state` tokens: `unknown\|charging\|discharging\|fully-charged\|empty\|pending-charge\|pending-discharge`. `warning` tokens: `unknown\|none\|discharging\|low\|critical\|action`. No `Q_INVOKABLE`: the shell has no power panel to open |
| `MediaService` | `available`, `title`, `artist`, `playerName`, `playbackStatus`; `playPause()`, `next()`, `previous()` | all empty/false while `available` false — the widget draws nothing, not a dash, because "no media" is a complete reading; `playbackStatus` tokens: `playing\|paused\|stopped` (empty = unknown). No player or player with no track = `available` false.. The three methods are requests to the player being followed (the one the reading is published from), sent to its own bus name as the MPRIS2 `org.mpris.MediaPlayer2.Player` methods `PlayPause`, `Next` and `Previous`; each returns whether a request was sent (`false` with no player) and a player that refuses is a record on `quantum.shell.media`. What is drawn afterwards is the property change that comes back, never an assumed state |
| `NotificationService` | `notificationAvailable`, `notificationSummary`, `notificationBody`, `notificationApplication`, `notificationCount`, `notificationExpireTimeout`, `notificationHistory`, `notificationHistoryCount`, `notificationDoNotDisturb`, `notificationHistoryOpen`; `clearNotificationHistory()`, `removeFromNotificationHistory(id)` | all empty/false while `notificationAvailable` false; `notificationSummary` is the sender's own text, `notificationBody` is the sender's own body, `notificationApplication` is the sender's own application name. `notificationCount` is a running total for the life of the service: it counts what this process has been sent, so it is deliberately **not** withdrawn with the reading when the name is released, and no widget draws it today. `notificationExpireTimeout` is the sender's own `expire_timeout`, published as the spec hands it and **not** resolved here: `-1` is the spec's use-the-default and which default that is, `0` is never-expire, and a positive count is milliseconds — the toast's business, not a daemon's. Every one of them moves together and only on a real change: a repeat registration emits nothing, and the queued handover publishes availability as soon as the bus makes the name the shell's History and mode: `notificationHistory` is a list of `{id, application, summary, body, received}` maps, newest first, capped at `NotificationHistoryLimit` = 50 (a fixed length, not a key); an update that reuses an id replaces its entry and moves it to the front; entries survive a close, an expiry and Do Not Disturb, and are not withdrawn with the reading. `notificationDoNotDisturb` and `notificationHistoryOpen` are **runtime state, never configuration**: both start false every launch and neither is persisted. Do Not Disturb stops a toast being shown — the notification is still received, answered, recorded and drawn by the readout — and tells the sender it is over (`NotificationClosed` reason 4); switching it on takes down a toast that is up, switching it off replays nothing |
| `LayerShellWindow` | `layer`, `anchors`, `exclusiveZone`, `keyboardInteractivity`, `layerNamespace`, `margins`, `inputRect`; `present()` | `inputRect` is the part of the surface that takes the pointer (an empty rectangle, the default, is the whole surface); `present()` is called by `BarHost`, not from QML, and only after the window's screen is assigned: showing the window is what assigns the layer role against an output and the role is assigned once, and the output cannot be chosen from QML because `screen` is not a QML property of a `QQuickWindow`. Set all props first |

Every row above is checked against code by `spec-values-test`, in both directions: every
name a row states must exist and every property and `Q_INVOKABLE` the class declares must
be named. Seven rows are read from the meta-object of the service that declares them, which
is the spelling the engine resolves a binding against. The `LayerShellWindow` row is the
exception and is read from that class's own `Q_PROPERTY` and `Q_INVOKABLE` declarations
instead, because the class derives from `QQuickWindow`: linking it into a test whose
subject is a document makes the sanitizer job report the font stack's process-lifetime
caches as that binary's leaks (722474 bytes in 16702 allocations, every frame in
libfontconfig or libpangocairo). That is the weaker source and is not presented as the
stronger one — it catches a `Q_PROPERTY` renamed, added or removed in the declaration,
and it cannot see a disagreement between a declaration and the meta-object moc built from
it. So a property added to a service, or renamed on it, fails `spec-values-test` until
these tables follow. The `Config` row is held the same way in both directions: every
`bar.<name>` it states is a property of the object `bar` and every property that object
declares is stated, so a new `[bar.*]` table with no row of its own fails here rather than
being a set of keys the document never mentions.

Key names per shape live in `NiriServiceKeys.h` (always-present vs optional lists;
absence is a claim: `activeWindowId` only with a reported active window,
`workspaceId` only on a placed window, geometry keys only with a logical output, mode
keys only with a current mode). Compile-time mirrors in tests fail the build on
rename; a rename here = a rename everywhere + approval.

### 2.7 Logging categories (declared once, `Logging.h`)

`quantum.shell` (lifecycle) · `.niri` (attach/loss/backoff) · `.config` (every refusal
+ unknown key) · `.ipc` (socket + every refusal; answers never logged) · `.wayland`
(layer-surface refusals) · `.system` (every unreadable file / refused line + cadence
record) · `.audio` (daemon, sink, refused params/writes, backoff) · `.network`
(bus reachability, the unique bus name this process holds, the daemon leaving and
arriving, every object that refused to be read) · `.battery` (the display device it
followed, the bus name this process holds, every refusal) · `.media` (the bus it
monitors, the player it follows, every discovery, subscription and metadata refusal) ·
`.notification` (the notifications name taken, queued behind another daemon or refused,
every `Notify` with the id answered, every close, and the name released). Pattern
`%{time yyyy-MM-dd HH:mm:ss.zzz} [%{type}] %{category}: %{message}` unless
`QT_MESSAGE_PATTERN` is set. Destination is Qt's: terminal when stderr is one,
journal otherwise (`journalctl --user _COMM=quantum-shell`). Secrets are never logged.

### 2.8 Test and environment names (declared once, `tests/public_names.cmake`)

49 tests: `public-names-test`, `qs-scan-self-test`, `repo-scan`,
`slot-order-independence`, `slot-order-randomised-shard-{1..4}`, `niri-live-test`,
`niri-live-stream-test`, `niri-live-action-test`, `niri-live-layershell-test`,
`niri-live-restart-test`, `niri-live-shell-restart-test`, `niri-live-scale-test`, `audio-live-test`,
`network-live-test`, `notification-test`, `osd-test`, `apps-test`, `launcher-test`, `control-center-test`, `crash-handler-test`, `dist-test`, `niri-version-test`, `niri-ipc-test`, `niri-event-stream-test`,
`niri-state-test`, `niri-actions-test`, `niri-output-test`,
`niri-keyboard-layouts-test`, `niri-outputs-test`, `niri-service-test`,
`niri-reconnect-test`, `config-test`, `config-watcher-test`, `sysmon-test`, `clock-test`,
`audio-test`, `network-test`, `battery-test`, `media-test`, `ipc-protocol-test`, `ipc-server-test`,
`ipc-capabilities-test`, `app-logging-test`, `snapshot-reconcile-test`,
`spec-values-test`, `bar-interaction-test`.
12 env vars: `NIRI_SOCKET`, `DESTDIR`, `QS_NIRI_SESSION_TESTS`, `QS_NIRI_RESTART_TESTS`,
`QS_NIRI_SCALE_TESTS`, `QS_AUDIO_TESTS`, `QS_TEST_ORDER_SEED`, `QS_TEST_ORDER_COVERAGE`,
`QS_TEST_ORDER_PASSES`, `QS_MIN_PAIR_COVERAGE`, `QS_WORST_TRIPLES_SHOWN`,
`QS_WORST_QUADS_SHOWN`. A documented `ctest -R` selecting nothing exits 0 — the
two-sided public-names check is what catches that, and this file is one of the three
documents that check is pointed at, so a name or a `-R` here that no longer exists fails
`public-names-test` rather than sitting in a document nobody reads. The cost of that
protection is this one: a variable name quoted here is read as a *documented* environment
variable, so anything of that shape mentioned below has to be declared in the list above.
Rules discussed by name rather than by value are spelled out in prose for that reason.

Both counts above, and the lists under them, are read from the declarations rather than repeated:
`spec-values-test` compares the numbers stated here with `tests/public_names.cmake` and requires
every declared test to be named in this section, so a test added to the build and not to this
document fails rather than going unnoticed.

The §2.6 rows are read the same way, and from the strongest source each one admits: the meta-object of
the singleton, which is what a QML binding resolves against, for the four whose classes that test
links; and the class's own declarations, for `LayerShellWindow`, whose link would cost the sanitizer
job the font stack's caches (§2.6 states this where the table is). Every property and `Q_INVOKABLE` a
class declares is required to appear in its row, every named token is required to exist, `each with
NOTIFY` is checked signal by signal, and the two wildcards' counts — `bar.system.*` and `bar.audio.*`, 4 leaves each — are compared with the
properties those nested objects actually declare. (The count in this sentence is prose rather than a
figure the test reads, and it had been wrong since the volume readout landed: it said `bar.audio.*` was
2 when the table was reading 3 keys. Corrected here with the fourth, which is the one thing this
document is for.)

### 2.9 Installation layout

What `cmake --install` places, and the one thing about it that is a contract rather
than a packaging detail:

| Path | What it is |
| --- | --- |
| `${CMAKE_INSTALL_BINDIR}/quantum-shell` | the shell |
| `${CMAKE_INSTALL_BINDIR}/qsctl` | its client (`§2.4`) |
| `${CMAKE_INSTALL_LIBDIR}/libquantum-shell-wayland.so` | the layer-shell client, loaded by both the shell and the plugin below it |
| `${QT6_INSTALL_PLUGINS}/wayland-shell-integration/libquantum-shell-layer-shell.so` | the shell-integration plugin, in the directory Qt resolves `QT_WAYLAND_SHELL_INTEGRATION=quantum-shell` against |
| `${CMAKE_INSTALL_DATADIR}/dbus-1/services/org.freedesktop.Notifications.service` | D-Bus activation for the daemon the shell is |
| `${CMAKE_INSTALL_DATADIR}/licenses/QuantumShell/LICENSE` | the licence |
| `${CMAKE_INSTALL_DOCDIR}/README.md` | the README |

The plugin's `INSTALL_RPATH` is `$ORIGIN/` plus the relative path from the plugin
directory to `CMAKE_INSTALL_LIBDIR`, computed from those two variables rather than
written as `../..`. CMake's generic `$ORIGIN:$ORIGIN/../lib` resolves against the
plugin's own directory, which is three below `${CMAKE_INSTALL_LIBDIR}`, so the default
names a directory that does not exist and the plugin loads without the library it is
built on. `ldd` on the installed plugin is what found that, not the build tree.

The activation file's `Exec` path is resolved at **install** time, not at configure
time, so `cmake --install --prefix <p>` names the prefix the files actually landed in. A
path written at configure time would name the build's own prefix and fail silently the
first time a notification arrives before the shell is up. CMake applies `DESTDIR` to its
own install commands and not to a file this project writes itself, so the install rule
applies it — which is why `DESTDIR` is declared among the public names above.

No Nix flake is installed or provided: it cannot be built, installed or run on the
machine this was developed on, and an unverifiable packaging file is not one this project
ships. `packaging/PKGBUILD` is verified by building the package.

---

## 3. Module contracts

### 3.1 niri connection (`src/niri/`)

- **Wire** (`NiriProtocol`): request = one JSON value + `\n`; reply =
  `{"Ok":…}` / `{"Err":"…"}`; event = single-key object per line. Line cap 4 MB —
  larger means not-the-protocol, buffer dropped. Unknown request answers
  `{"Err":"error parsing request"}` — the only error a capability probe reads as
  "build lacks it". Ids: absent/null/negative = no value; id 0 is never "none".
- **Request connection** (`NiriIPC`): async `QLocalSocket`, ordered replies, one read
  buffer. `send` for unit variants, `sendObject` (compact JSON, newline-free by
  construction) for actions. `detect()` emits exactly one of `versionDetected` /
  `detectionFailed`, then `capabilitiesDetected`. Late replies after the sender's scope
  ended still dispatch — handlers must not point at dead storage
  (pinned by `deliversAReplyAfterTheSendingScopeHasEnded`).
- **Event connection** (`NiriEventStream`): separate socket (niri stops reading
  requests after `EventStream`). First line is the subscribe reply, rest are events.
  Three distinct reports, never collapsed: `unmodelledEvent` (known 26.04 name, not
  handled), `unknownEvent` (not in the 26.04 enum — protocol moved), `malformedEvent`
  (handled, bad payload).
- **Model** (`NiriState`): one truth per fact (focus = flag on window; active = flag on
  workspace). `WorkspacesChanged`/`WindowsChanged` replace the set; opening/changing a
  focused window unfocuses the rest; activating a workspace deactivates same-output
  others; focused implies sole-focused across outputs. Signals fire only on real
  change. Field-tolerant parse; missing `id` (workspace/window) or `name` (output) =
  invalid, refused. `layout`/`focus_timestamp` (window) and `serial`/`physical_size`/
  `is_custom_mode` (output) deliberately unparsed. `clear()` on loss.
- **Outputs** (`NiriOutputs`): niri 26.04 streams no output event — refresh is
  request-driven on two derived triggers only: `WorkspacesChanged` whose referenced
  output-name set changed, and `ConfigLoaded`. Coalesced (≤1 in flight + 1 queued).
  Unreadable reply = failed refresh, last complete list stands (a missing output ≡ an
  unplugged monitor). `refresh()` public for callers with their own reason.
- **Version/capabilities** (`NiriVersion`): non-`year.month` = invalid, never coerced.
  Features: 11 (`compositorBackgroundEffect` version-gated ≥26.04; `version`,
  `outputs`, `workspaces`, `windows`, `layers`, `keyboardLayouts`, `focusedOutput`,
  `focusedWindow`, `overviewState`, `casts` probed read-only). Unprobed = `Unknown`,
  never enabled. Interactive/blocking/state-changing requests are never probes.
- **Actions** (`NiriActions`): only actions with a caller exist (8 methods; 141 enum
  variants do not each get surface). Optional fields sent as explicit null. Ids cross
  QML as text, digit-checked back to `u64`; `maximumId` = `INT64_MAX` (JSON double
  exact only to 2⁵³ — above it is refused, never rounded into a wrong workspace).
  Click on the already-focused capsule sends nothing (with
  `workspace-auto-back-and-forth` it would land on the previous workspace). Wheel =
  niri's own up/down; "below" is the compositor's answer, never computed from the
  strip. `Result`: `handled` / `refused` / `notDelivered` / `unexpected`.
- **Reconnect** (`NiriReconnect`): backoff first 250 ms ×2, cap 8000 ms, jitter ≤100 ms
  (0 = exact, for tests), per-attempt deadline 5000 ms. Socket re-resolved every
  attempt (`$NIRI_SOCKET` while the path exists, else rediscovery by niri's
  `niri.<wayland>.<pid>.sock` naming rule). `attached` = request connected + stream
  acknowledged; `lost` once per outage. Backoff/debounce timers only — nothing polled
  while attached.
- **Service/keys/module**: §2.6. `connected` = stream subscribed. Transform kept as the
  text niri sent (`transformIsKnown` covers the 8 enum names; only `Normal` observed —
  not mapped to an enum on a guess). Refresh rate in millihertz, unrounded.

### 3.2 Configuration (`src/config/`)

Pure schema (`parseConfig` of text) → `Config` QObject tree → `ConfigWatcher`
(file + parent dir + nearest existing ancestor; survives atomic replace; re-derives
watch set per event). First read synchronous before the engine (bar born configured);
later parses on a worker thread, diff+signals on the GUI thread; coalesced
(`readAgain_`) so multi-step editor writes apply the last state. Per-leaf compare:
only changed properties emit. `ConfigSystem`/`ConfigAudio` hold references into the
parent's validated struct — one value, read by QML and `qsctl` alike. Table-per-widget
rule: a widget's settings own a table (`[bar.system]`, `[bar.audio]`); two flags, not
a `readings` list (a list would re-decide layout order, which belongs to `qml/`); no
`cpu_format` (a key with one honest value does nothing).

### 3.3 System readings (`src/system/`)

Aggregate `cpu ` line only (8 counters; `guest`/`guest_nice` excluded — already inside
`user`/`nice`); `MemTotal` + `MemAvailable` in `kB` only (no `MemFree`-arithmetic
substitute); files capped at 1 MB. `busyPercent` refuses non-advance, backwards, and
idle-delta-exceeds-total (unsigned wrap would print as a number). Service: first
sample immediate (memory on screen at once), `cpuAvailable` false until two readings;
deactivation stops the timer and forgets everything (stale numbers never shown; idle
cost of a hidden bar = zero wake-ups). Single-shot timer re-armed after each reading
at the accepted cadence; one record per accepted cadence, silence on unchanged value.
Constructor takes the proc root so tests drive every input shape from fixtures plus
two slots against the real `/proc`. Needs a user-written waiver line for the
event-driven rule before it is sanctioned — see §8; until then it is a documented
exception, not a waived one.

### 3.4 Audio (`src/audio/`)

Follows the sink named by metadata `default`, key `default.audio.sink`, JSON
`{"name":"…"}` typed `Spa:String:JSON`; sink = node with `media.class="Audio/Sink"`
matched by `node.name`. Reading = `SPA_PARAM_Props`: `channelVolumes` (float array;
first channel is the sink's, count kept for writes) + `mute` (not `softMute`).
Event-driven twice over: `pw_node_subscribe_params(Props)` plus `info.change_mask &
PW_NODE_CHANGE_MASK_PARAMS` re-enumeration; initial `enum_params` seeds the current
value. Either route alone keeps the suite green; both removed blinds it — the pair is
load-bearing, not either half. Percent = cube root of linear
(`percentFromLinear(0.042872) == 35`, matching `wpctl get-volume`); above-unity linear
kept (PipeWire `channelmix.max-volume` 10.0 is real); percent wheel math clamps `0..100`, and a
notch in the decibel unit clamps to the same ceiling (linear 1.0, which is 0 dB):
Decibels = 20·log₁₀ of the same factor (`decibelsFromLinear(0.074087) == -22.605`,
which `pactl list sinks` prints as `-22,61 dB` beside that sink's `42%`), silence =
`-INFINITY` rather than a floor (pulse's own printer passes `-INFINITY` through), and
both numbers are computed from one factor on the PipeWire thread so the readout cannot
show two readings at once. `[bar.audio].volume_scale` picks which of them is drawn *and*
which of the two steps a notch is measured in — a percentage step spans 1.28 dB at 99%
and 46.7 dB at 1%, so a fixed gain is what a person reading dB is owed.
Each accepted step and unit writes one record on `quantum.shell.audio` naming what it
holds, which is the only way a running shell's wheel is observable from outside the
process — the same reason `SysMonService` records its cadence — and it is what the live
case in `niri-live-layershell-test` reads back;
one decibel is `10^(1/20)` on the factor, and a notch from silence lands on the quietest
level the shell writes (1%) because a ratio from zero does not exist;
step-while-muted keeps mute (as `wpctl set-volume` does). Unnamed default / no Props
yet / daemon away = `available == false` + dash; malformed Props keeps the last
reading + record. Daemon loss: teardown, withdraw, exponential backoff retry
(250 ms → 8 s). Callbacks on the PipeWire thread loop; published state only via
queued GUI-thread apply; writes under the loop lock. `PropsWrite`: ≤64 channels, 1 KiB
owned buffer (pod dangles never), null on overflow — caller honors null. Gestures are
requests; the daemon's answer is what gets drawn, so the bar never drifts from the
mixer. Click with no reading is refused + logged.

### 3.5 Wayland / bar (`src/wayland/`, `qml/`, `src/app/BarHost.*`, `src/app/main.cpp`)

One bar per output, and it is one per *enabled* output: `BarHost` (`src/app/BarHost.*`)
instantiates `qml/Main.qml` once for every screen Qt reports and keeps the set in step with
`QGuiApplication::screenAdded`/`screenRemoved`, so a monitor plugged in or turned off is a
bar appearing or going away without a poll. The screen is assigned from C++ with
`QWindow::setScreen` and the surface is presented after it, because a layer surface is
created against an output when the window is mapped and the role is assigned once; the
output's *name* arrives as the `outputName` initial property, which is the one thing the QML
cannot work out for itself. `BarHost::toggleAll()` moves every bar and answers the state they
are all in afterwards, `anyVisible()` is the sampling gate, and both are read from the
windows rather than from a copy — a bar hidden and shown again goes through
`LayerShellWindow::setSurfaceVisible`, which applies the same empty-namespace refusal
`present()` does. Where a bar's workspaces come from: `Workspaces.qml` filters
`NiriService.workspaces` on the model's own `output` field, so a bar draws its own output's
sets and not the other monitor's, and a workspace niri reports with no output appears on no
bar (`qml/Workspaces.qml` states the reasoning). With no `outputName` the strip draws the
whole model, which is the component loaded on its own.

Route 1: own `zwlr_layer_shell_v1` bindings on QtWaylandClient's private
shell-integration interface (`QT_WAYLAND_SHELL_INTEGRATION=quantum-shell`);
`quantum-shell-wayland` shared (app + plugin share one `QMetaObject`);
`LayerShellWindow` props flow into `get_layer_surface`; initial commit carries no
buffer; first paint waits for the compositor configure (`isExposed` gate); live edits
re-send configuration + commit. Bar asks: top layer, top+left+right anchors (width
from compositor, size 0 on the stretched axis — the non-stretched zero-width refusal
is fixed by construction), `NoKeyboard`, `height == exclusiveZone == Config.bar.height`
(live), `layerNamespace` from config (once). Non-`LayerShellWindow` and off-prefix
namespaces get no surface, with records.

Graphics API: the shell calls `QQuickWindow::setGraphicsApi(QSGRendererInterface::Software)`
before the first window, and only when `QSG_RHI_BACKEND` is unset — so the default is the
software renderer (measured idle RSS 85.4 MB, both budgets met) and an explicit choice wins
(`QSG_RHI_BACKEND=opengl` is the hardware path, kept for Phase 2 3D). Records confirm both
directions: `Loading backend software` by default, `Creating QRhi with backend OpenGL` with
the variable set. §7 carries the measurement and the attribution behind it.

Arrangement law: widgets declare into named groups (`left`, `centre`, `right`),
carry no coordinates/anchors/offsets; `CapsuleGroup.fit()` (on completed, children
change, height change, guarded to settle) gives every child the group height; content
centers itself in the given height; invisible widgets hold no space. Centre group is
bar-centered — a too-narrow bar overlaps rather than squeezes (decision deferred to
the group that must give way). Click = `focusWorkspaceById` with the model's id text
(skipped when already focused); wheel = niri up/down, one step per 120 units of accumulated
`angleDelta` (a mouse notch exactly; a touchpad's small deltas add up, the remainder
is dropped when `WheelHandler` ends the gesture after its 100 ms of silence). Widgets: honest empty states only — workspaces show the `niri` dot iff
`!connected`; CPU/memory/volume show a dash while their `available` flag is false;
hidden-by-config readouts take zero width. Boundary: pixels/geometry/animation in
`qml/` only; sockets/TOML/PAM/processes in `src/` only; state crosses via QObject
singletons, never by QML reaching into C++ internals.

Composition order (`main.cpp`): logging → register `LayerShellWindow` → config
sync-read + watcher → SysMon register + cadence wiring → audio register + step
wiring → IPC/stream/state/outputs + `observe` (once each; outputs first-wired on
first `attached`) → service/actions singletons → reconnect armed → `BarHost`
instantiates `qrc:/qml/Main.qml` per screen (a component that does not load = exit 1;
no output at all warns and draws nothing) → sampling follows whether *any* bar is
visible (covers `qsctl bar toggle`) → IPC listen (bind failure warns, shell still draws —
a second shell is useful) → `audio.start()` → `reconnect.start()` → `exec()`.

### 3.6 IPC server/client (`src/ipc/`, `src/app/ShellCapabilities.*`)

`IPCServer` knows no domain facts: decode → `Capabilities` → encode. Three methods,
same objects QML reads (service values verbatim, schema resolver, weak bar pointer).
`qsctl` binary = connection + four calls; all string decisions in `QsctlCli`
(test-driven without a socket).

---

## 4. Verification matrix

Default `ctest --preset dev` (jobs 4): gate + order checks + 25 unit binaries, no
session. Live layers need a session and opt-ins; acting tests take the
`niri-desktop` resource lock and never run two at once. `ctest --preset session`
registers those opt-ins and runs the four tests that need a compositor, refusing
when none of them is registered. `ctest --preset dev -R niri-live-scale-test`
registers the scale test, which starts a compositor of its own and takes no lock.

| Claim | Proved by | Run |
| --- | --- | --- |
| No banned content in shipped paths | `qs-scan-self-test` (fixtures), `repo-scan` (tree) | `cmake --build --preset dev --target scan` |
| Test/env names used anywhere exist | `public-names-test` + deferred configure guard | `ctest --preset dev -R public-names-test` |
| No slot passes on a sibling's leftovers | `slot-order-independence` (each slot solo + reverse) | `ctest --preset dev -R slot-order-independence` |
| Order robustness, 96 seeded passes × 4 shards | `slot-order-randomised-shard-{1..4}` (pair floor 95%, triple + quad coverage reported) | `ctest --preset dev -R slot-order-randomised` |
| niri wire, events, model, actions, outputs, layouts, reconnect | `niri-*-test` (10 binaries, test-double end of the socket) | `ctest --preset dev -R 'niri-(version\|ipc\|event-stream\|state\|actions\|output\|keyboard\|outputs\|service\|reconnect)-test'` |
| Bar gestures + every text the bar draws read as plain text + arrangement + each of the four memory forms and the empty state drawn from a `/proc` of the test's own with fixed numbers + volume widget states + both volume units drawn from readings the test hands the widget's own rule (silence, mute and no-reading included) with the token driven through a real file + the volume round trip measured against the group's live contents with a minute moved on mid-trip, each group width waited for because a positioner lays out a frame later + one slot that reads the machine's `/proc` and asserts the first reading is a baseline rather than a percentage + the network readout's three flags driven through a real file and its whole rendering rule exercised with the daemon's tokens (every state, wifi/ethernet, a signal of zero against no signal, portal/limited/absent, the empty state) + the notification readout end to end, against the shell as the desktop's notification daemon on a `dbus-daemon` of the test's own: a sender's `Notify` over the wire, the application name and the summary the widget then draws, the empty state as a dash before anything is sent, the flag hiding the readout and giving its room back **with a reading in hand**, and the readout dark again once another connection holds the notifications name | `bar-interaction-test` (shipped QML, real engine offscreen, real window events) | `ctest --preset dev -R bar-interaction-test` |
| `/proc` parsers, refusals, cadence + its record | `sysmon-test` (fixtures + 2 real-`/proc` slots) | `ctest --preset dev -R sysmon-test` |
| Props parse, cube-root, decibel conversion (unity 0, silence `-INFINITY`, above-unity positive), wheel math in both units (a dB notch as a fixed gain, its clamp, the percentage notch's growing dB span, silence), write pod, refusals, the unit's token round trip, `[bar.audio]` rules incl. the two scale tokens | `audio-test` (no daemon) | `ctest --preset dev -R audio-test` |
| The notifications name taken on a bus of the test's own, `Notify` answered with an id, `replaces_id` echoed as the caller's own id, the sender's text published verbatim, the spec's other two methods, `NotificationClosed` for a close, an expiry and a displaced notification (read off the wire from a second connection) and silence for an id that is not showing, ids that never collide with an echoed `replaces_id`, the toast's size (declared width, height following the text), an earlier notification's clock not withdrawing a never-expiring toast, a queued shell publishing when the holder leaves, the shell dark while another connection holds the name, and the real `notify-send` reaching it (probed first, that one slot skipped by name on a machine that cannot run it) | `notification-test` (starts a `dbus-daemon` of its own; the shell takes the name there) | `ctest --preset dev -R notification-test` |
| The volume display: none up before an adjustment, one surface per output for one announced by the audio service and withdrawn by its clock, a further adjustment restarting the clock without adding a surface, `show_osd = false` never showing one and taking down one that is up, the overlay layer / bottom anchor / no keyboard / no exclusive zone / declared size the surface is created with, and the text and bar fill the surface draws for a percentage, a decibel value, silence, a mute and no reading — with a warning from the QML failing the slot. What the *service* announces (an adjustment of the followed sink, and not the first reading, a switch or a repeat) is `audio-live-test`'s | `osd-test` (offscreen, a service that is never started); `audio-live-test` for the signal | `ctest --preset dev -R osd-test` |
| What the shell says when a fatal signal kills it: a copy of the test binary installs the handler and dies of each signal (SIGSEGV by a null write, SIGABRT, SIGFPE, and a real stack overflow, which only the alternate stack can report), and what it wrote to standard error is read back — the version and the signal by number and name, a `backtrace:` line with frames after it — together with the manner of death: the process must still die *of that signal* (a core dump and a supervisor's restart depend on it), not exit; a clean exit with the handler installed says nothing | `crash-handler-test` | `ctest --preset dev -R crash-handler-test` |
| The release tarball and its checksum: built against a git repository the check makes for itself — the committed files under a `quantum-shell-<version>/` prefix and nothing else (no ignored build output, no `.git`), a checksum file `sha256sum -c` accepts (and a wrong one it rejects, so the check has teeth), the same commit giving the same bytes twice, and a tree with uncommitted changes refused with the dirty path named and no tarball left behind | `dist-test` | `ctest --preset dev -R dist-test` |
| The launcher's reading of the desktop, no display and no bus: the Desktop Entry parse (only the `[Desktop Entry]` group, first key wins, localised `Name`/`Comment` in the specification's order, the string escapes and list values), the `Exec` split (quotes, the four in-quote escapes, field codes removed or resolved, no shell), what is offered on a given desktop (hidden, terminal, `OnlyShowIn`, `NotShowIn`), the ranking of a query, frecency (its buckets, the history file's round trip and refusals, tie-only ordering, a launch remembered across services), a scan of directories the test writes (user copy shadows the system's, a user's `Hidden` deletes an entry, `TryExec`, subdirectory ids), the service scanning on open on a worker thread and filtering by query, selection wrapping, `max_results` bounded, and a launch that starts a real `touch` on a path containing spaces and closes the panel — with a program that cannot start leaving it open | `apps-test` | `ctest --preset dev -R apps-test` |
| Every D-Bus name against the daemon's own spelling, its state/connectivity/device-type numbers as the widget's tokens (unknown refused, not guessed), the property map read through its variants (bare and `QDBusVariant` both, text for a number and a number for text refused, `ao` arriving as a raw `QDBusArgument`), a change that overtakes an object's first read not undone by the reply, the reading the chain adds up to (wifi/ethernet/unassociated/offline/clamped/unknown state), and the service against a NetworkManager test double on a private bus: the reading arriving, following `PropertiesChanged` with **zero** calls to the daemon counted after the first read, a chain that moves with the objects it left no longer followed, a daemon leaving and arriving, a reply from a daemon that is gone dropped, an object that refuses leaving the rest standing, an unreachable bus refused with a record | `network-test` (starts a `dbus-daemon` of its own; the test double owns the name there) | `ctest --preset dev -R network-test` |
| The clock being moved from under it: the real-time clock set, twice in a row, by a real `clock_settime` to the value just read (skips with its reason without `CAP_SYS_TIME`), and a timezone switched the way `timedatectl` does it — the symlink replaced, then the zone now in force edited in place — while a file appearing beside the zone, and a quiet period, say nothing; plus `bar-interaction-test`'s slot that a stale time is read again when the service says the clock moved | `clock-test` (files of its own and a real timerfd), `bar-interaction-test` | `ctest --preset dev -R clock-test` |
| Schema, diffing, `Config` bindings, compile-time mirrors (defaults, floors, token list, key paths) | `config-test`, `config-watcher-test` (scratch dirs) | `ctest --preset dev -R config` |
| IPC frames, refusals, a connection from another uid closed unanswered, server over a real socket, capabilities vs real service | `ipc-protocol-test`, `ipc-server-test`, `ipc-capabilities-test` | `ctest --preset dev -R ipc` |
| Record format + category names | `app-logging-test` | `ctest --preset dev -R app-logging-test` |
| Every number this document states, and every §2.6 singleton row, against the code that holds them | `spec-values-test` (reads this file, links the libraries, walks seven service meta-objects and the Config tree, reads the bar window's header) | `ctest --preset dev -R spec-values-test` |
| Reconciliation contract (budget, exhaustion fails, settled-vs-missed diagnosis) | `snapshot-reconcile-test` (scripted readings) | `ctest --preset dev -R snapshot-reconcile-test` |
| Model agrees with the compositor (read-only) | `niri-live-test`, `niri-live-stream-test` | `NIRI_SOCKET=… ctest --preset dev -R 'niri-live-(test\|stream-test)'` |
| Overview open/close + focus up/down on screen, and a refused action leaving the overview where it was (a change the compositor itself reports is reported, a model out of step fails) | `niri-live-action-test` | `NIRI_SOCKET=… QS_NIRI_SESSION_TESTS=1 ctest -R niri-live-action-test` |
| Real layer surface, wire anchors/zone, configured height+namespace, file→service cadence + live edit, the wheel's two steps and the unit reaching the *service* of a running shell + a live unit edit, `qsctl` vs compositor (socket in `/proc/net/unix`, state field-for-field, toggle out/in of layer list) | `niri-live-layershell-test` | `… QS_NIRI_SESSION_TESTS=1 ctest -R niri-live-layershell-test` |
| Restart survival (nested niri kill + restart; shell death observed: prompt, exit 1, no signal, socket released; second shell answers about the new session) | `niri-live-restart-test`, `niri-live-shell-restart-test` | `… QS_NIRI_RESTART_TESTS=1 ctest -R restart` |
| Volume vs a private daemon (external change/mute/sink-switch both ways, own writes as cubed percent on all channels, step + clamp, a 1 dB notch measured on the daemon's own factor as a `10^(1/20)` ratio, the unit switching which step applies, the dB floor and an unknown unit token refused, muted-step stays muted, daemon away/back, both published units against the daemon's own factor read by `pw-dump`) | `audio-live-test` (own daemon, own socket; `pw-cli`/`pw-metadata`/`pw-dump` as independent reader/writer) | `QS_AUDIO_TESTS=1 ctest --preset dev -R audio-live-test` |
| The reading against the desktop's own NetworkManager — state, connectivity, connection name, device interface, device kind and signal quality, each compared with what `busctl` prints for the same object, and the walk repeated to show the objects reached are the daemon's own | `network-live-test` (read-only; `busctl` as the independent reader; skips where no system bus has NetworkManager) | `ctest --preset dev -R network-live-test` |

Rule for new work: every behavior arrives with a test that fails with the behavior
removed; no test skipped/loosened/deleted to go green; falsifiers named in the
report; evidence re-run after the last edit. Throwaway repro scripts prove work —
they are not kept. Kept tests defend observable contracts only.

---

## 5. Failure and logging catalog (abridged — full texts live at the call sites)

niri refused/unknown request · unmodelled/unknown/malformed events · missed-event vs
unsettled-desktop diagnosis (≥2 readings always) · invalid id-less objects dropped ·
outputs refresh failure keeps last list · invalid version string reported, never
coerced · action outcomes on `actionFailed` + `quantum.shell.niri`. Config: §2.5
warning/error split, all on `quantum.shell.config`. System: §3.3 refusals on
`quantum.shell.system`. Audio: §3.4 refusals on `quantum.shell.audio`. Wayland:
role/namespace refusals on `quantum.shell.wayland`. IPC: every refusal on
`quantum.shell.ipc` with reason; answers silent. Notification daemon — the one
service the shell *is* rather than a reader of — the name taken, queued behind
another daemon or refused (including a bus that was never connected), every `Notify`
with the id answered, every `CloseNotification` (and whether it closed anything), and the reading withdrawn on the
bus's own answer, all on `quantum.shell.notification`. Shell lifecycle (version, protocol,
config path, socket bind result) on `quantum.shell`.

---

## 6. Honest limitations (not defects-in-hiding)

- Outputs: changes firing neither a workspace output-set change nor a config reload
  (transient `niri msg output` edits; hotplug without a workspace) wait for the next
  trigger; `refresh()` is the escape hatch.
- Wheels (strip + volume) step once per 120 units of accumulated distance, not per event,
  so a touchpad swipe is as many steps as the distance it covers and a mouse notch is one.
  A fast fling still covers a lot of distance and steps that many times; there is no
  time-based rate limit of niri's `cooldown-ms=150` kind. Consecutive volume notches in
  one turn of the event loop each start from the volume the previous one wrote
  (`PipeWireService` records a successful write as the base for the next), so a burst
  no longer loses steps to the daemon's not having echoed yet.
- Narrow bar: centre group overlaps side groups rather than yielding.
- Volume OSD: shown for an adjustment of the followed sink only (`PipeWireService::volumeAdjusted`); the
  surface takes pointer input over its whole 300×132 area (the transparent strip under the panel
  included) because the layer-shell integration sets no input region; primary and every other output
  alike; brightness, keyboard-layout and media displays do not exist.
- Notifications: the daemon, the toast, the history panel and Do Not Disturb are real; actions are not.
  `NotificationService` owns `org.freedesktop.Notifications` (queued, never stolen) and answers
  the spec's four methods. One notification is showing at a time — the newest wins — and the
  daemon tracks its id (`currentNotificationId()`): `CloseNotification` for that id closes it and
  the sender hears `NotificationClosed(id, 3)`; the toast's expiry ends it with reason 1; a newer
  notification that takes the slot closes the older one with reason 4 (the spec's
  "undefined/reserved"); a close for any other id is accepted and says nothing. New ids skip the
  id that is showing and never wrap to zero, because a sender may hand back any `replaces_id`
  and the daemon echoes it. `GetCapabilities` advertises `body` and nothing else: the toast
  draws the body, but every text is drawn as plain text, so `body-markup` would be a capability
  claimed and not honoured (libnotify strips markup for a daemon that does not claim it).
  `actions`, `icons` and `hints` are ignored, so a sender cannot offer a button (no
  `ActionInvoked`); the only thing a person can do to a notification is click its toast, which
  withdraws it and tells the sender reason 2 (`Dismissed`); `expire_timeout` is published as the spec hands it
  (`notificationExpireTimeout`, `§2.6`) and resolved by `ToastHost`: `-1` is
  `[bar.notifications] timeout_ms`, `0` never expires — and cancels the clock an earlier timed
  notification armed — and a positive count is milliseconds. Becoming the daemon announces a change but no notification (`currentNotificationId()` stays zero), so it creates no toast. The bar's readout draws one
  notification (the sender's application name and summary) and keeps it after a close, because a
  close does not make the text that was in it into something else. The history is the last
  `NotificationHistoryLimit` (50) notifications, in memory only — it is not kept across a
  restart — and the panel that shows it takes no keyboard and grabs no pointer, so a click
  elsewhere leaves it open: only its own Do Not Disturb/Clear controls, the readout's right
  click, the compositor closing it or its output going away end it. It is on the primary output
  only, not one per output. Do Not Disturb has no schedule and no per-application rules. A shell that is queued behind another notifier
  publishes nothing and draws nothing, which is a life the shell can spend entirely on a
  desktop that runs its own daemon — measured on the author's, where `swaync` holds the name.
- Layer surfaces: on an axis that is not stretched the client's size wins over the compositor's
  echo of an earlier request (a configure that lags a resize would otherwise put the window back
  to the old size), and `set_size` is sent again when the *proposed* size changes — a live edit
  of `bar.height`, a toast whose text made it taller. The compositor's number is used only where
  the window declared nothing, which proposes the whole output on that axis; the toast
  therefore declares both (`width: 380`, height from its text). Verified against a headless
  sway (wlroots layer-shell), not against niri: the niri live tests that would pin it are not
  written, and this environment has no niri.
- Clock: `qml/Clock.qml` re-arms a single-shot timer for the next minute boundary, and Qt timers run on the
  monotonic clock, which does not advance during suspend, so on its own it is late by however far the wall
  clock jumped. `ClockService` (`src/system/`) is the event source that corrects it: a `timerfd` on
  `CLOCK_REALTIME` armed with `TFD_TIMER_CANCEL_ON_SET`, which the kernel completes with `ECANCELED` when the
  clock is set (a manual step, an NTP step, and the step taken on resume from suspend), and a watch on
  `/etc/localtime` and its directory for a timezone switch, reporting only when the zone it resolves to differs.
  `clockChanged()` makes the clock read the time and re-arm. Verified by `clock-test` (a real `clock_settime` to
  the value just read, twice in a row; skipped where the process lacks `CAP_SYS_TIME`) and on a headless sway,
  where stepping the wall clock five minutes forward changed the bar's time within 1.5 s. **Not verified:** a real
  suspend and resume (this environment cannot suspend), which is assumed to be reported the same way because the
  kernel treats the step it takes on resume as the clock being set; and a real `timedatectl set-timezone`, which
  is exercised here as the symlink replacement it performs, against a file of the test's own.
- Bar hide/show: **not a leak.** A short run does show growth (+96 KiB over 400 pairs, about 0.5 KiB a pair),
  which is why it was recorded as unaccounted for, but over 2000 pairs the release shell's RSS climbs
  49,800 → 50,284 KiB in the first 250 pairs and then oscillates between 50,356 and 50,608 KiB with drops
  (1500 → 1750 pairs: −236 KiB) — the shape of the QML engine's JS heap being collected, not of a leak. What
  grows in the short run is JS string garbage from the readouts' bindings (valgrind, 100 pairs against 0 on
  the debug build: `QLocale::toString`, `QV4::Heap::String::simplifyString` and `numberToString` under
  `SysMonService::readingChanged`), which is freed when the collector runs. Measured on a headless sway,
  default configuration, 4-core Xeon 2.8 GHz, `qsctl bar toggle` twice per pair; not measured on niri. The much
  larger leak that used to sit here was the IPC server keeping every closed client's socket (fixed;
  `IPCServer::openConnections()`).
- toml++ 3.4.0 defects the schema works around rather than fixes, because the dependency is pinned:
  `TOML_ASSERT` is disabled for `ConfigSchema.cpp` (a `[` followed by a newline asserts in the key parser
  and aborts assertion-enabled builds), and `parseConfig` refuses a non-ASCII byte outside a string or
  comment by line before parsing (U+00A1..U+0499 reaches `__builtin_unreachable()` in the library's
  whitespace test). Both are pinned in `config-test`; a toml++ upgrade should re-run them.
- Reconcile cannot catch a dropped event later overwritten by a newer one on the same
  field — agreement after the fact is agreement.
- Volume writes refuse past 64 channels; above-unity volumes are read, never written
  by the wheel.
- The volume readout's unit is not cosmetic: it selects what is drawn *and* which of the
  two step keys a notch applies, and the two are separate keys rather than one rescaled
  value because the same key meaning two things depending on another key is a schema that
  lies about itself (this clause read the opposite way until the change that made a notch a
  distance in the unit being read; the second step key it said did not exist now does).
  What is still true: the unit a notch is measured in is not the *widget's* choice, and
  `PipeWireService::setWheelStepUnit` is a C++ setter rather than a property for that
  reason — what QML reads is the unit it draws, which is `Config`'s. The widget's rule is
  exercised with the numbers a daemon sends and the token through a real file, while the
  *service's* numbers against a real daemon are `audio-live-test`'s: nothing loads the
  widget against a live daemon, which is why the rule takes its readings as inputs, and the
  dB-notch arithmetic itself is pinned in `audio-test` as a function of the factor.
- `bar-interaction-test`'s volume round trip compares the group against its contents at
  the moment of the check rather than against a width remembered earlier, because that
  group also holds the wall clock and `Inter` resolves here to a proportional-figure font
  (`HH:mm` is 28.28–37.06 px, so every minute changes it). A positioner's width is its
  contents — *once it has laid out*: a positioner's own size is not a binding, so the group
  and its `implicitWidth` follow a child's width change only when the next frame goes
  through (measured: unchanged through `qWait(1)`, correct at 78.25 from 78.09375 after a
  frame, for one minute's 10/64 px change in the clock). Every group comparison here is
  therefore waited for rather than read once. What the comparison proves is that the
  widget is counted among the group's contents; the remembered form asserted that the time
  stood still and failed the shuffled check. The
  readouts `SysMonService` feeds move for the same reason — the configured cadence — and
  the slots that assert them are deterministic instead: the service is pointed at a `/proc`
  of the test's own, so each expectation is a literal worked out from numbers the test
  wrote and no assertion in those slots can be moved by the machine they run on. The
  counters in that fixture advance on every write, because a real `/proc` only ever goes up
  and a slot that sampled twice against a line that had not moved would be provoking the
  service's own refusal — a record `sysmon-test` asserts and those slots have no business
  inventing. One slot reads the machine's files, since a fixture proves the parsing and
  something has to prove the reading is real, and it asserts that the *first* reading is a
  baseline rather than a percentage: that is what says the baseline it measures the second
  one against is the machine's, because a pair spanning the fixture and the machine is not
  refused at all — the machine's counters are the larger, so what it publishes is the busy
  share of the whole boot. No slot in that file provokes a refusal it does not assert: the
  fixture's counters advance on every write, and the live slot's two machine readings are a
  tick apart, because the service records a pair that is not an interval and a record in
  somebody else's transcript is not a fixture's business. Measured by marking that slot's
  call sites and running it alone — the record appeared in three runs of six while a
  redundant second sample sat microseconds after the first, and in none of twelve runs of
  the slot alone once it was removed.
- A live value remembered across a wait is not a baseline any test can rely on, and where
  the claim survives the movement the compositor discriminates instead: a model that
  agrees with a changed compositor was told about the change, a model that disagrees has
  moved on its own. The acting cases whose claim needs the desktop to hold still are left
  failing rather than made unfailable, because reporting a moved desktop would let a shell
  that did nothing pass.
- Gate detector limits (in `patterns.txt`, by design): line-based (multi-line failure
  shapes invisible); filler names, canned values, reachability, timer intent, and
  plausible-looking defaults are human review, not machine checks.
- No headless niri exists — restart tests nest in the session, hence opt-in; CI runs
  GCC + Clang + ASan/UBSan but never a compositor.

---

## 7. Budgets

Measured on 2026-09-17 with the shipped **software-rendered 2D default** (the user's
decision after the attribution below; `QSG_RHI_BACKEND=opengl` remains the opt-in for
the hardware path): Release bar visible on niri DP-3, default configuration, no override
environment, Ryzen 7 5800XT / RTX 4060 Ti, Linux 7.2.6-1-cachyos, Qt 6.11.2. Over
60.0537 seconds (Python monotonic clock), `/proc/<pid>/stat` user + system time
increased by 11 ticks at 100 Hz: **0.183% of one core**. VmRSS from `/proc/<pid>/status`
was 87372 → 87496 KiB (85.4 MB). **Both budgets pass: CPU <1%, RSS <150 MB.**
Other desktop applications were running; this is process RSS, not private or GPU memory.

The hardware path remains reachable and is recorded for the decision it required: the
same Release executable idled at 0.250% CPU with VmRSS 172492 → 173548 KiB (168.4–169.5 MiB)
under the default OpenGL/NVIDIA stack, whose driver mappings totalled 56360 KiB RSS
(`libnvidia-gpucomp` 35280, `libLLVM` 16316, `libnvidia-eglcore` 11384 — resident
mappings, not an allocation breakdown). A Vulkan probe measured 254520 KiB and a
GTK-theme-cleared probe 150536 KiB; both were diagnostics, not adopted changes.
The software default closes the memory budget by stepping off the GPU for the 2D bar;
per-feature idle-cost measurement (§ 3D) applies when the hardware path returns for 3D.

A second idle measurement, on 2026-09-30, and a weaker one: Release shell, default configuration, bar visible on a
**headless sway** (pixman renderer) on a 4-core Xeon 2.8 GHz, no niri and no PipeWire daemon, so both the niri and
the audio services sat in their reconnect backoff. Over 60.0 s, user + system time was **0.067% of one core**, and
VmRSS was 50952 → 51048 KiB (about 50 MB). It is a lower bound for a shell whose niri and audio connections are up,
not a replacement for the niri figure above, which stays the one the budgets are judged on.

Heap growth: 2000 hide/show pairs (`qsctl bar toggle` twice each) on the same setup climbed RSS 49800 → 50284 KiB in
the first 250 pairs and then held between 50356 and 50608 KiB with periodic drops — the pattern of a collected heap,
not a leak (§6, "Bar hide/show").

Idle wake-ups: hidden bar = zero wake-ups from sampling; visible bar wakes once per accepted
cadence plus the clock's once-per-minute single-shot. Shard wall clock ≈ slowest
shard: ~330 s, measured as 324.8 s then 329.9 s in the two runs of the current split, whose four
shards spanned 284.4–324.8 s and 289.7–329.9 s. Two things account for it, and only one of them is
this landing. The split's own numbers grew: the per-binary pass costs in `tests/CMakeLists.txt` now
sum to ≈269 s of a shard's 96 passes, against the ≈230 s of the five runs this paragraph used to
record (228.3–230.8 s), because four of those binaries have been re-measured upward as landings added
slots to them — `bar-interaction-test` among them, whose notification slots drive the shell as the
desktop's notification daemon and cost it the 163 ms a pass it grew by here (1029 → 1192 ms, which
is 16 s of the 96). The measured span then sits 6–23% above that ≈269 s, which is the spread this
figure has always had on a machine doing other work: a single pass of this binary was timed at 1.2 s
to 9.0 s under that load, so the shards are the figure to read and not a per-run constant.
~1230 s when the four are run in sequence (1221.5 s and 1239.7 s).

`bar-interaction-test` ≈ 1192 ms/pass;
`network-test` ≈ 2364 ms, the most expensive unit pass in the suite because it starts a `dbus-daemon`
of its own and holds a reply in flight;
`sysmon-test` ≈ 788 ms; full default suite per AGENTS.md command reference. Numbers
are measurements on the author's machine, not guarantees — re-measure with the
commands in §4.

---

## 8. Doc drift (code is right; prose lags — fix prose, not code)

Six items were found when this file was first derived. Four were fixed in the same
change that added this section — that is the rule this file exists to be held to, and
their removal is the evidence it was applied rather than acknowledged. Of the two that
remain, one is not an agent's to write at all and the other is a trap deliberately kept:
neither is an oversight, and saying so is the point of the section.

**Closed, with what the fix was:**

1. `QUANTUM_SHELL.md` § IPC verb table listed 3 config paths while the resolver serves
   8 — the table now names all eight and points at `KeyPaths` as the list, so a ninth
   key is an edit to the table rather than a discovery.
2. `QUANTUM_SHELL.md` § Exposed QML API registration prose described C++ registration
   "moving once `qml/` exists" while `qml/` exists — rewritten: `src/QmlModule.h`
   declares the URI and both versions, `NiriQmlModule.h` re-exports them, and there is
   no pending move. The stale sentence in `NiriQmlModule.h`'s own comment went with it.
3. `QUANTUM_SHELL.md` § IPC and `QsctlCli.h` justified `qsctl volume up`'s absence
   with "no audio service" — the refusal is correct and unchanged (client-side usage
   error, nothing sent) but the *reason* was wrong once the volume module landed, and it
   is a comment inside `src/`, so it was fixed there and in the document together.
4. `QUANTUM_SHELL.md` Phase 1 said audio was not started — it has a section of its own
   in that document now, and the roadmap paragraph names what is left (network, battery,
  media, the renderer decision with its measured budget, and the unresolved
  system-sampling waiver) instead of listing audio among the absent ones.

**Open, and not for an agent to close:**

5. `SYSTEM_PROMPT.md`'s Waivers block still reads `(none)` while the tree carries the
   sampling timer the event-driven rule forbids. The gate's `polling-timer` warnings are
   the visible marker. Only the user writes a waiver, as a dated line; until then §3.3 is
   an exception-by-comment, documented but unsanctioned, and this file says so rather than
   making it look settled.
6. File-vs-key spelling trap, kept because it is a real footgun rather than a defect: the
   file says `namespace`, the key path and QML say `layerNamespace`, and both sides are
   correct — §2.5 pins which side takes which spelling, so a change that "fixes" one of
   them to match the other is the change that breaks the interface.

---

## 9. Change rules for this file

Update in the same change that: adds/renames a config key, IPC verb, QML singleton
property, layer namespace, log category, test/env name, or version floor; changes a
default, bound, backoff, timeout, cap, or refusal behavior; or alters the composition
order. The names are checked by `public-names-test` and the numbers by
`spec-values-test` — which checks a §2.6 property the same way, from the meta-object where
the class is linked and from the class's own declarations where it is not — so a stale value
here fails a test rather than waiting to be noticed
— but only for the facts those two read: a sentence about behaviour, a limitation, a
budget that is prose, is read by people, and the ones that matter have a test of their
own under §4. New modules land here with their contract the way they land in `QUANTUM_SHELL.md`
with their design. Keep tables exact — a value here that code contradicts is the
"file that lies" failure the public-names check exists to prevent.
