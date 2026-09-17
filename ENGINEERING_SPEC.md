# Quantum Shell — Engineering Spec

**Status:** derived from source at `main a449a39` plus the uncommitted volume landing
observed in `git status` (new `src/audio/`, `src/system/`, `qml/Volume.qml`,
`qml/SystemMonitor.qml`, `qml/CapsuleGroup.qml`). Contracts below are stated from the
code as read; live/compositor behaviors were not re-executed in this audit and are
marked where they rest on the listed live tests.

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
`LayerShellIntegration.cpp`). Only surface today: `quantum-shell-bar`. Enforced twice:
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
- Non-JSON / non-object / version mismatch → refuse naming the problem and close.
  Unknown verb, missing argument, unknown config key → refuse by name, connection stays
  open.

Request shape: `{"version":1,"verb":"state"}` plus `path` for `config get`.
Response shape: success `{"version":1,"ok":true,"data":{...}}` (`data` present even when
empty); refusal `{"version":1,"ok":false,"error":"<reason>"}` (never blank).

### 2.3 IPC verbs (the whole surface: `verb::All`, 4 entries)

| Verb | Answers with | Source |
| --- | --- | --- |
| `version` | `name`, `shell` (both from the build, not retyped), `protocol` | app identity |
| `state` | `workspaces`, `focusedWindow`, `outputs`, `keyboardLayout`, `overviewOpen`, `connected` — `NiriService` property names verbatim | `ShellCapabilities::state` |
| `config get <path>` | `path`, `value` | `configValueForPath` on validated values |
| `bar toggle` | `visible` (post-toggle state) | bar window's own visibility; null bar answers `false` |

### 2.4 `qsctl` CLI

`version`, `state`, `config get <key>`, `bar toggle`. `config get` prints the bare
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

Rules: unknown keys warn with full path; missing keys keep defaults; wrong type or
refused value warns naming key, value found, and value kept (never silent, never
coerced). Unusable file (non-TOML, bad `schema_version`) applies nothing.

### 2.6 QML singletons (module `QuantumShell 1.0`, one place: `QmlModule.h`)

| Name | Properties / calls | Notes |
| `Config` | `bar.height`, `bar.layerNamespace`, `bar.system.*` (4), `bar.audio.*` (4), `bar.network.*` (3), `bar.battery.*` (3), `bar.media.*` (1); per-leaf NOTIFY; `bar`/`system`/`audio`/`network`/`battery`/`media` objects CONSTANT | no engine reload, ever |
| `NiriService` | `workspaces`, `focusedWindow`, `outputs`, `keyboardLayout`, `overviewOpen`, `connected` — each with NOTIFY, emitted only on real change | absent = empty map/list, never plausible zero; ids as text |
| `NiriActions` | `focusWorkspaceById(idText)`, `focusWorkspaceUp()`, `focusWorkspaceDown()`; signal `actionFailed` | every non-`handled` outcome also logged |
| `SysMonService` | `cpuPercent`, `cpuAvailable`, `memoryUsedKb`, `memoryTotalKb`, `memoryAvailableKb`, `memoryAvailable`, `active`, `sampleIntervalMs` | `cpuPercent` 0 while `cpuAvailable` false; read the flags |
| `PipeWireService` | `available`, `muted`, `volumePercent`, `volumeDecibels`, `stepPercent`, `stepDecibels`; `toggleMute()`, `stepVolume(dir)`, `setVolumePercent(p)` | both numbers 0 while `available` false; read the flag. `volumePercent` = cube root of the linear factor ×100, `volumeDecibels` = 20·log₁₀ of the same factor (both computed from one reading; neither derives the other). `stepVolume` applies whichever of the two steps the unit selects; the unit itself is a C++ setter, deliberately not a property (no binding has a use for it) |
| `NetworkService` | `available`, `state`, `connectionName`, `interfaceName`, `deviceKind`, `hasStrength`, `strength`, `connectivity` | `state`/`deviceKind`/`connectivity` are tokens (`disconnected\|connecting\|connected`, `wifi\|ethernet\|other`, `none\|portal\|limited\|full`); `connectivity` is empty when the daemon has not said, which is not `full`. `strength` 0 while `hasStrength` false — read the flag. No `Q_INVOKABLE`: the shell has no control centre to open, so there is no gesture to offer |
| `BatteryService` | `available`, `present`, `onBattery`, `hasPercentage`, `percentage`, `state`, `warning`, `hasTimeRemaining`, `timeRemaining` | all 0/empty/false while `available` false; `percentage`/`timeRemaining` 0/empty while their `has*` flag false; `present` false on desktop (no battery). `state` tokens: `unknown\|charging\|discharging\|fully-charged\|empty\|pending-charge\|pending-discharge`. `warning` tokens: `unknown\|none\|discharging\|low\|critical\|action`. No `Q_INVOKABLE`: the shell has no power panel to open |
| `MediaService` | `available`, `title`, `artist`, `playerName`, `playbackStatus` | all empty/false while `available` false — the widget draws nothing, not a dash, because "no media" is a complete reading; `playbackStatus` tokens: `playing\|paused\|stopped` (empty = unknown). No player or player with no track = `available` false. No `Q_INVOKABLE`: transport controls are Phase 2 |
| `LayerShellWindow` | `layer`, `anchors`, `exclusiveZone`, `keyboardInteractivity`, `layerNamespace`, `margins`; `present()` | QML calls `present()`, not `show()`; set all props first |

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
arriving, every object that refused to be read). Pattern
`%{time yyyy-MM-dd HH:mm:ss.zzz} [%{type}] %{category}: %{message}` unless
`QT_MESSAGE_PATTERN` is set. Destination is Qt's: terminal when stderr is one,
journal otherwise (`journalctl --user _COMM=quantum-shell`). Secrets are never logged.

### 2.8 Test and environment names (declared once, `tests/public_names.cmake`)

40 tests: `public-names-test`, `qs-scan-self-test`, `repo-scan`,
`slot-order-independence`, `slot-order-randomised-shard-{1..4}`, `niri-live-test`,
`niri-live-stream-test`, `niri-live-action-test`, `niri-live-layershell-test`,
`niri-live-restart-test`, `niri-live-shell-restart-test`, `audio-live-test`,
`network-live-test`, `niri-version-test`, `niri-ipc-test`, `niri-event-stream-test`,
`niri-state-test`, `niri-actions-test`, `niri-output-test`,
`niri-keyboard-layouts-test`, `niri-outputs-test`, `niri-service-test`,
`niri-reconnect-test`, `config-test`, `config-watcher-test`, `sysmon-test`,
`audio-test`, `network-test`, `battery-test`, `media-test`, `ipc-protocol-test`, `ipc-server-test`,
`ipc-capabilities-test`, `app-logging-test`, `snapshot-reconcile-test`,
`spec-values-test`, `bar-interaction-test`.
10 env vars: `NIRI_SOCKET`, `QS_NIRI_SESSION_TESTS`, `QS_NIRI_RESTART_TESTS`,
`QS_AUDIO_TESTS`, `QS_TEST_ORDER_SEED`, `QS_TEST_ORDER_COVERAGE`,
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

### 3.5 Wayland / bar (`src/wayland/`, `qml/`, `src/app/main.cpp`)

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

Arrangement law: widgets declare into named groups (`left`, `centre`, `right`),
carry no coordinates/anchors/offsets; `CapsuleGroup.fit()` (on completed, children
change, height change, guarded to settle) gives every child the group height; content
centers itself in the given height; invisible widgets hold no space. Centre group is
bar-centered — a too-narrow bar overlaps rather than squeezes (decision deferred to
the group that must give way). Click = `focusWorkspaceById` with the model's id text
(skipped when already focused); wheel = niri up/down (mouse 1:1; no 150 ms cooldown —
rapid touchpad scrolls step repeatedly, cooldown belongs with the gestures when one
exists). Widgets: honest empty states only — workspaces show the `niri` dot iff
`!connected`; CPU/memory/volume show a dash while their `available` flag is false;
hidden-by-config readouts take zero width. Boundary: pixels/geometry/animation in
`qml/` only; sockets/TOML/PAM/processes in `src/` only; state crosses via QObject
singletons, never by QML reaching into C++ internals.

Composition order (`main.cpp`): logging → register `LayerShellWindow` → config
sync-read + watcher → SysMon register + cadence wiring → audio register + step
wiring → IPC/stream/state/outputs + `observe` (once each; outputs first-wired on
first `attached`) → service/actions singletons → reconnect armed → engine loads
`qrc:/Main.qml` (load failure = exit 1) → sampling follows bar visibility (covers
`qsctl bar toggle`) → IPC listen (bind failure warns, shell still draws — a second
shell is useful) → `audio.start()` → `reconnect.start()` → `exec()`.

### 3.6 IPC server/client (`src/ipc/`, `src/app/ShellCapabilities.*`)

`IPCServer` knows no domain facts: decode → `Capabilities` → encode. Three methods,
same objects QML reads (service values verbatim, schema resolver, weak bar pointer).
`qsctl` binary = connection + four calls; all string decisions in `QsctlCli`
(test-driven without a socket).

---

## 4. Verification matrix

Default `ctest --preset dev` (jobs 4): gate + order checks + 21 unit binaries, no
session. Live layers need a session and opt-ins; acting tests take the
`niri-desktop` resource lock and never run two at once.

| Claim | Proved by | Run |
| --- | --- | --- |
| No banned content in shipped paths | `qs-scan-self-test` (fixtures), `repo-scan` (tree) | `cmake --build --preset dev --target scan` |
| Test/env names used anywhere exist | `public-names-test` + deferred configure guard | `ctest --preset dev -R public-names-test` |
| No slot passes on a sibling's leftovers | `slot-order-independence` (each slot solo + reverse) | `ctest --preset dev -R slot-order-independence` |
| Order robustness, 96 seeded passes × 4 shards | `slot-order-randomised-shard-{1..4}` (pair floor 95%, triple + quad coverage reported) | `ctest --preset dev -R slot-order-randomised` |
| niri wire, events, model, actions, outputs, layouts, reconnect | `niri-*-test` (10 binaries, test-double end of the socket) | `ctest --preset dev -R 'niri-(version\|ipc\|event-stream\|state\|actions\|output\|keyboard\|outputs\|service\|reconnect)-test'` |
| Bar gestures + arrangement + each of the four memory forms and the empty state drawn from a `/proc` of the test's own with fixed numbers + volume widget states + both volume units drawn from readings the test hands the widget's own rule (silence, mute and no-reading included) with the token driven through a real file + the volume round trip measured against the group's live contents with a minute moved on mid-trip, each group width waited for because a positioner lays out a frame later + one slot that reads the machine's `/proc` and asserts the first reading is a baseline rather than a percentage + the network readout's three flags driven through a real file and its whole rendering rule exercised with the daemon's tokens (every state, wifi/ethernet, a signal of zero against no signal, portal/limited/absent, the empty state) | `bar-interaction-test` (shipped QML, real engine offscreen, real window events) | `ctest --preset dev -R bar-interaction-test` |
| `/proc` parsers, refusals, cadence + its record | `sysmon-test` (fixtures + 2 real-`/proc` slots) | `ctest --preset dev -R sysmon-test` |
| Props parse, cube-root, decibel conversion (unity 0, silence `-INFINITY`, above-unity positive), wheel math in both units (a dB notch as a fixed gain, its clamp, the percentage notch's growing dB span, silence), write pod, refusals, the unit's token round trip, `[bar.audio]` rules incl. the two scale tokens | `audio-test` (no daemon) | `ctest --preset dev -R audio-test` |
| Every D-Bus name against the daemon's own spelling, its state/connectivity/device-type numbers as the widget's tokens (unknown refused, not guessed), the property map read through its variants (bare and `QDBusVariant` both, text for a number and a number for text refused, `ao` arriving as a raw `QDBusArgument`), a change that overtakes an object's first read not undone by the reply, the reading the chain adds up to (wifi/ethernet/unassociated/offline/clamped/unknown state), and the service against a NetworkManager test double on a private bus: the reading arriving, following `PropertiesChanged` with **zero** calls to the daemon counted after the first read, a chain that moves with the objects it left no longer followed, a daemon leaving and arriving, a reply from a daemon that is gone dropped, an object that refuses leaving the rest standing, an unreachable bus refused with a record | `network-test` (starts a `dbus-daemon` of its own; the test double owns the name there) | `ctest --preset dev -R network-test` |
| Schema, diffing, `Config` bindings, compile-time mirrors (defaults, floors, token list, key paths) | `config-test`, `config-watcher-test` (scratch dirs) | `ctest --preset dev -R config` |
| IPC frames, refusals, server over a real socket, capabilities vs real service | `ipc-protocol-test`, `ipc-server-test`, `ipc-capabilities-test` | `ctest --preset dev -R ipc` |
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
`quantum.shell.ipc` with reason; answers silent. Shell lifecycle (version, protocol,
config path, socket bind result) on `quantum.shell`.

---

## 6. Honest limitations (not defects-in-hiding)

- Outputs: changes firing neither a workspace output-set change nor a config reload
  (transient `niri msg output` edits; hotplug without a workspace) wait for the next
  trigger; `refresh()` is the escape hatch.
- Wheels (strip + volume) act per event with no cooldown; niri's own bind rate-limits
  at 150 ms. Mouse 1:1; touchpad flings multi-step.
- Narrow bar: centre group overlaps side groups rather than yielding.
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

Measured on 2026-09-17: Release bar visible on niri DP-3, default configuration,
Ryzen 7 5800XT / RTX 4060 Ti, Linux 7.2.6-1-cachyos, Qt 6.11.2. Over 60.0348 seconds
(Python monotonic clock), `/proc/<pid>/stat` user + system time increased by 15 ticks
at 100 Hz: **0.250% of one core**. VmRSS from `/proc/<pid>/status` increased from
172492 to 173548 KiB (168.4 to 169.5 MiB). CPU passes <1%; RSS fails <150 MB.
Other desktop applications and the remainder of the Release build were running; this is process RSS, not private or GPU memory.
Phase 1 is not closed. See `QUANTUM_SHELL.md` Phase 1 for the workload and remaining work.

Idle: hidden bar = zero wake-ups from sampling; visible bar wakes once per accepted
cadence plus the clock's once-per-minute single-shot. Shard wall clock ≈ slowest
shard: ~230 s in the five runs of the current split (230.8, 229.6, 229.4 replaying a pinned
seed, 228.3, 226.6), with the shards of one invocation spanning 200.6–230.8 s, and
204.8–252.7 s across the runs on a machine shared with other work, which is the variance
to expect from the figure; ~853 s when the four are run in sequence. `bar-interaction-test` ≈ 1046 ms/pass;
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
   media, the idle-budget measurement) instead of listing audio among the absent ones.

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
