# Quantum Shell

A native Wayland bar for [niri](https://github.com/YaLTeR/niri), written in C++23 and Qt 6 QML.

Every widget on it reads the running compositor rather than a snapshot of one. Workspaces, the
focused window, the keyboard layout and the output list arrive on niri's event stream; volume comes
from PipeWire; the network, battery and media readouts come from NetworkManager, UPower and MPRIS.
The shell is also the session's notification daemon — it owns `org.freedesktop.Notifications` on the
session bus and answers the specification's four methods itself.

niri is the only compositor this supports, deliberately and permanently: the deep integration above
is what the single-compositor choice buys. See `QUANTUM_SHELL.md` § Non-Goals.

## Status

Stated plainly, because a readout that lies about what it draws is the one failure this project
treats as the worst.

**Working today:**

- The bar itself: a layer-shell surface pinned to the top of an output, with a reserved exclusive
  zone, arranged as a leading group, a centre group and a trailing group.
- Nine readouts driven by live data: the workspace strip, window list, clock, system status, volume,
  network, battery, media and notifications.
- Clicking a workspace capsule focuses that workspace in niri; the wheel over the strip moves to the
  workspace below or above. The window list beside it shows the windows on the bar's own output's
  workspaces, marks the focused one, and a click focuses the one named.
- A launcher (`qsctl launcher toggle`) over the applications the machine offers, ordered by what you type and
  then by what you started lately; a control centre (`qsctl control-center toggle`) with volume, Do Not
  Disturb and media transport; a volume on-screen display; and a calendar opened by clicking the clock. Only
  one of the launcher, control centre, notification history and calendar is open at a time, and a click
  outside it closes it.
- A notification daemon that takes the bus name, answers `Notify`, `CloseNotification`,
  `GetCapabilities` and `GetServerInformation`, publishes the sender's own text, and tells a sender when
  its notification is closed, expired or replaced (`NotificationClosed`).
- A notification history of the last fifty notifications and a Do Not Disturb mode, both on the notification
  readout: a left click switches the mode (no toast is shown, but notifications are still recorded) and a right
  click opens and closes the history panel. Neither is configuration; both start off every launch.
- A toast per output: a layer surface on the top layer, drawn over what is on screen rather than shrinking
  it, withdrawn when its expiry runs out — the expiry a sender sent, or the shell's default for a sender
  that sent `-1`.
- Configuration in TOML, read at startup and followed while the shell runs without a restart.
- A local IPC socket with `qsctl`, its command-line client.

**Absent at this point:** brightness control, the lock screen, session actions, idle handling, desktop widgets, animation presets, the plugin system, the dock, clipboard history, Bluetooth and power profiles. The palette
and the typeface **are** configurable (`[bar.colors]` and `[bar.font]`) and can be kept in a theme file
(`theme = "name"`, below); what remains of theming is animation, spacing and assets, which are the things a
theme does not carry yet because nothing in the shell draws from them. Each of
these is a phase in `QUANTUM_SHELL.md` § Development Roadmap, and each phase records whether it has
started.

## Requirements

| Component | Version |
|---|---|
| Qt | 6.11 or newer, with `WaylandClient` and `WaylandScannerTools` |
| CMake | 3.31 or newer |
| A C++23 compiler | GCC or Clang |
| toml++ | 3.4 or newer, or fetched at build time |
| niri | any version; capabilities are detected at runtime |

Optional, each behind its own readout: PipeWire for volume, NetworkManager for the network, UPower
for the battery, and any MPRIS player for media. A machine without one of those draws the readout's
empty state and nothing else — none of them is needed to start the shell.

Qt's layer-shell client interface is private, so the build asks for private modules explicitly
(`QT_FIND_PRIVATE_MODULES`). The distro package that carries those headers is `qt6-wayland` on
Arch, `qt6-wayland-dev` on Debian-family systems.

## Build

```sh
cmake --preset release && cmake --build --preset release
```

The presets are `dev` (Debug), `release`, `ci` and `asan` (Debug with AddressSanitizer and
UndefinedBehaviorSanitizer). A warning is treated as a defect here: every target builds under
`-Werror` with `-Wall -Wextra -Wpedantic -Wshadow -Wcast-qual -Wold-style-cast -Wnon-virtual-dtor
-Wmissing-declarations`.

If a distribution build must use the packages it was told to build against rather than fetching a
second copy of toml++, configure with `-DQUANTUM_SHELL_SYSTEM_DEPS=ON`; the build then fails rather
than fetching when no system toml++ is found.

## Release tarball

```sh
cmake --build build/release --target dist
sha256sum -c build/release/dist/quantum-shell-<version>.tar.gz.sha256
```

`dist` archives the *committed* tree (`git archive HEAD`) into `quantum-shell-<version>.tar.gz` with a checksum file
`sha256sum -c` reads. It refuses a tree with uncommitted or untracked changes, and the same commit gives the same
bytes. It writes two files and publishes nothing. How a version's files, protocol and configuration change between
releases, and what to do about it, is in [UPGRADING.md](UPGRADING.md).

## Install

```sh
cmake --install build/release --prefix /usr
```

Four things are installed, and each has a reason to be where it is:

- `bin/quantum-shell` and `bin/qsctl` — the shell and its client.
- `lib/libquantum-shell-wayland.so` — the layer-shell client, loaded by both the shell and the
  plugin below it.
- `lib/qt6/plugins/wayland-shell-integration/libquantum-shell-layer-shell.so` — the shell-integration
  plugin, in the directory Qt looks a shell integration up by, because a plugin installed anywhere
  else is one Qt never loads.
- `share/dbus-1/services/org.freedesktop.Notifications.service` — D-Bus activation for the
  notification daemon, so a notification arriving before the shell is up starts it.

## Run

The shell integration is selected by environment variable, so niri has to start the shell with it
set:

```kdl
spawn-at-startup "env" "QT_WAYLAND_SHELL_INTEGRATION=quantum-shell" "quantum-shell"
```

From a build tree before installing, point Qt at the plugin directory as well:

```sh
cd build/dev && QT_PLUGIN_PATH="$PWD/plugins" QT_WAYLAND_SHELL_INTEGRATION=quantum-shell ./quantum-shell
```

The bar is drawn with Qt's software renderer. That is a measured decision and not a fallback: the
hardware path idles at roughly 168 MB resident against a budget of under 150 MB, most of it the
driver's shader compiler, while the same executable with every readout live idles at roughly 87 MB
under the software renderer, and the bar is a 2D scene the software renderer draws without the
driver. Naming a backend explicitly gives you the hardware path instead, which is also what the
planned 3D work needs:

```sh
QSG_RHI_BACKEND=opengl quantum-shell
```

A shell started by niri has no terminal, so its records go to the systemd journal:

```sh
journalctl --user _COMM=quantum-shell -n 20
```

## Configuration

`$XDG_CONFIG_HOME/quantum-shell/config.toml`, or `~/.config/quantum-shell/config.toml`. The file is
optional — every key has a default, and a shell with no file draws the defaults. An edit while the
shell is running is picked up without a restart.

```toml
schema_version = 1

[bar]
height = 32
namespace = "quantum-shell-bar"

[bar.system]
sample_interval_ms = 2000
show_cpu = true
show_memory = true
memory_format = "used_of_total"

[bar.audio]
show_volume = true
volume_scale = "percent"
step_percent = 5
step_decibels = 1.0

[bar.network]
show_status = true
show_name = true
show_strength = true

[bar.battery]
show_status = true
show_percentage = true
show_time = true

[bar.media]
show_media = true

[bar.notifications]
show_notifications = true
timeout_ms = 5000

[launcher]
max_results = 8

[bar.osd]
show_osd = true
timeout_ms = 1500

[bar.colors]
foreground = "#c8cad8"
muted = "#5a5d70"
accent = "#7aa2f7"
urgent = "#f7768e"

[bar.font]
family = "Inter"
size = 12
weight = 400
```

Every value above is the default, so that block is the whole surface written out rather than a
change to it. The rules each key is held to:

| Key | Accepted values |
|---|---|
| `theme` | a theme's name (letters, digits, `-` and `_`), read from `themes/<name>.toml` beside `config.toml`, or an absolute path to a theme file. Empty, the default, is no theme. Anything else is refused by name |
| `bar.height` | an integer from 1 up, and also the exclusive zone reserved from the tiling area |
| `bar.namespace` | a string beginning `quantum-shell-`, the name `niri msg layers` reports |
| `bar.system.sample_interval_ms` | an integer of at least 10, one kernel tick |
| `bar.system.memory_format` | `used_of_total`, `used`, `available` or `percent` |
| `bar.audio.volume_scale` | `percent` for the desktop's cube-root percentage, `decibel` for the physical gain |
| `bar.audio.step_percent` | an integer of at least 1, how far one wheel notch moves the volume |
| `bar.audio.step_decibels` | at least 0.1, the same distance in the decibel unit |
| `bar.colors.*` | a colour literal: `#` and six hex digits, or eight with the alpha first. Named colours (`red`), three-digit shorthand and `rgb()` are refused by name |
| `bar.font.family` | a non-empty string, the face every readout draws in; an empty one is refused, since `sans-serif` is how a file says "whichever" |
| `bar.font.size` | an integer from 1 up: the body size, and the readouts that draw larger derive from it (the clock by one, media and notifications by two), so one number reflows the whole bar |
| `bar.font.weight` | an integer from 100 to 900, Qt's own weight scale |
| `bar.notifications.timeout_ms` | an integer of at least 500 ms, or the spec's `0` (never expire). It is the default a sender's `-1` resolves to; a sender that names its own length is honoured rather than clamped |

### Themes

A theme is one TOML file holding the two tables that say how the bar looks, spelled exactly as they are in
`config.toml`:

```toml
# ~/.config/quantum-shell/themes/nord.toml
schema_version = 1

[bar.colors]
foreground = "#d8dee9"
accent = "#88c0d0"

[bar.font]
family = "Noto Sans"
```

Name it with `theme = "nord"` at the top level of `config.toml`. The theme is the base: a value that
`config.toml` writes in its own `[bar.colors]` or `[bar.font]` wins over the theme's, and a value neither
writes is the default. Editing the key, or the theme file it names, is picked up while the shell runs — the
colours and the font change on screen with no restart. A theme that does not exist, is not TOML, or was
written for another `schema_version` is reported by name and path and applies nothing; a theme cannot set
anything but those two tables (a `bar.height` in one is reported and ignored). `qsctl config get theme`
prints the name in use.

A key that is unknown, or a value of the wrong type, or a value outside the rules, is reported as a
warning naming the file position and the value kept instead. A value is never quietly coerced:
`show_cpu = "no"` is a mistake and is reported as one, rather than being read as true.

`show_*` keys hide a readout; they do not switch off its reading. The service behind a hidden readout
still follows its daemon, so showing it again costs nothing and never reveals a stale number.

## The client

`qsctl` addresses the shell by its abstract socket name (`\0quantum-shell`) rather than by a
process, so it reaches whichever shell holds the name:

```sh
qsctl version                       # the shell's version and the IPC protocol it speaks
qsctl state                         # what the bar is drawn from, one line of JSON
qsctl config get bar.height         # a value, as the running shell resolved it
qsctl bar toggle                    # hides or shows the bar and reports which
qsctl launcher toggle               # opens or closes the launcher and reports which
qsctl control-center toggle         # opens or closes the control centre and reports which
```

`config get` reads the same values the shell is using, not the file, so it is how a change to a
running shell is confirmed.

## Tests

```sh
ctest --preset dev
```

The suite registers 36 tests. Most need no session and no display; the ones that act on the desktop are
opt-in, because they open and close real overviews or restart a compositor, and niri has no headless
mode to run them against:

```sh
QS_NIRI_SESSION_TESTS=1 cmake --preset dev && ctest --preset dev   # the two that act on the session
QS_NIRI_RESTART_TESTS=1 cmake --preset dev && ctest --preset dev   # the one that restarts a nested niri
QS_AUDIO_TESTS=1 cmake --preset dev && ctest --preset dev -R audio-live-test
```

Two gates run as tests and fail the suite rather than reporting: `repo-scan` refuses the build if the
tree contains unfinished work, scaffolding or an empty catch, and `public-names-test` refuses a
documented command that names a test nobody registers — a stale `ctest -R` matches nothing and still
exits 0, which is a green run that ran nothing.

```sh
cmake --build --preset dev --target scan    # the gate alone
cmake --build --preset dev --target check   # the gate plus every test
```

## Design documents

| Document | What it is |
|---|---|
| `QUANTUM_SHELL.md` | the design source: architecture, the roadmap and its phase status, the risk register |
| `ENGINEERING_SPEC.md` | the code-derived surface: every default, bound, refusal and contract |
| `AGENTS.md` | the repository state, the rules condensed, and the command reference |
| `SYSTEM_PROMPT.md` | the behaviour contract the prohibitions and the scan patterns come from |

Each is authoritative for its own subject; `AGENTS.md` § Authority records which one wins when two
of them speak to the same thing.

## Licence

MIT. See `LICENSE`.
