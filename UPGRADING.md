# Upgrading Quantum Shell

What changes between versions of the shell, what a person has to do about it, and what the shell does when the
two sides of an interface disagree. Everything here is what the code does today; where a mechanism does not exist
yet, this file says so rather than describing one.

## What is versioned

| Thing | Where it is written | Today | What a mismatch does |
| --- | --- | --- | --- |
| The shell | `project(... VERSION ...)` in `CMakeLists.txt`; `qsctl version` prints it | `0.1.0` | nothing: it is a label for bug reports and packages |
| The IPC protocol | `ProtocolVersion` in `src/ipc/IPCProtocol.h`; every frame carries it | `1` | the shell refuses the request and names both versions; `qsctl` exits `4` and says the two sides cannot trust each other's answer |
| The configuration schema | `schema_version` in `config.toml`; `SchemaVersion` in `src/config/ConfigSchema.h` | `1` | a version this build does not know is an error: the file is **not applied** and the last good values stay (at startup, the defaults) |

The first release has not been cut, so there is no earlier released version to upgrade *from*. The rest of this
file is the contract the next upgrade will be held to.

## Upgrading the package

1. Install the new package or the new `cmake --install` over the old one. The four artifacts move together —
   `quantum-shell`, `qsctl`, the layer-shell client library and the shell-integration plugin Qt loads — and they
   must be the same version: a `qsctl` from one release and a shell from another is exactly the protocol mismatch
   above.
2. **Restart the shell.** A running shell keeps the code it started with, and replacing the plugin or the client
   library under a live process is not supported. Stop it (`pkill -x quantum-shell`) and start it the way the
   session starts it (in niri, the `spawn-at-startup` line from the README), or log out and back in.
3. Confirm what is running: `qsctl version` prints the shell's version and the protocol it speaks.
4. Read what the shell said about your configuration:
   `journalctl --user _COMM=quantum-shell -b | grep quantum.shell.config`. A key the new version does not read, and
   a value it refuses, are each one line naming the key by its path; the default is used for that key and nothing
   else is affected.

## The configuration file

- Every file carries `schema_version`. A file that omits it gets a warning and is read as the current version —
  that is the case for a file written before the field existed.
- **A file from a newer schema than the shell knows is not applied.** The shell keeps the values it already has,
  says so as an error in the journal, and does not guess: reading a newer file as if it were older could apply a
  value with a different meaning.
- **Keys the shell does not read are a warning, not an error**, reported by full path
  (`bar.colours` is reported as `bar.colours`). So a file written for a newer shell still applies on an older
  one — the keys it does not understand are named and ignored — and a key renamed by an upgrade shows up as a
  warning with the old spelling, not as silent loss.
- A value of the wrong type or outside its bound is refused by name and the default for that key is kept; the
  other keys in the file still apply.
- **Migrations.** When a second schema version exists, the step from version *N* to *N+1* will be a pure function
  in `src/config/ConfigSchema.cpp`, run before validation, with a case in `config-test` per step. None exists
  today because only version 1 has ever existed; this file will gain a row in the table below with each one.

| Schema | Introduced with | What an upgrade to it changes in a file |
| --- | --- | --- |
| 1 | the first release | nothing: it is the only version |

## Names that do not change without a version

These are public interface (`AGENTS.md` § Public interface is frozen without approval), so a script, a compositor
key binding or an external widget can rely on them across upgrades:

- the layer-shell namespaces `quantum-shell-*` (`bar`, `toast`, `notification-history`, `osd`, `launcher`,
  `control-center`);
- the abstract socket `\0quantum-shell` and the six IPC verbs (`version`, `state`, `config get`, `bar toggle`,
  `launcher toggle`, `control-center toggle`);
- every configuration key in `ENGINEERING_SPEC.md` § 2.5, with its default and bound.

A change to any of them is a breaking change: it needs approval, a bump of the version that governs it (the
protocol version for a verb, the schema version for a key) and a row here that says what a person does about it.
Adding a verb, a key or a namespace is not breaking, and an older `qsctl` or file simply does not use it.

## Downgrading

Supported in the same terms: install the older package, restart the shell. A configuration written for the newer
version applies, with a warning for every key the older one does not read; a `qsctl` newer than the shell it is
talking to gets the protocol-mismatch refusal if the protocol version differs, and otherwise a refusal that names
any verb the older shell does not implement.
