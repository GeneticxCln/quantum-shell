// The configuration schema: the keys the shell reads, what each one defaults to, and the rules a value
// in the file has to satisfy to be used.
//
// Everything here is a pure function of the file's text. Nothing holds state, nothing watches a file
// and nothing knows about Qt objects or QML — which is what lets every rule below be tested by handing
// it a string, and what keeps the schema in one place instead of scattered through the loader.
//
// Three rules from QUANTUM_SHELL.md § Configuration are the shape of this file:
//
//   * Every file carries `schema_version`. A file without one is assumed to be the current version and
//     warned about; a file written for a version this build does not know is refused rather than
//     guessed at, because a key that means something else in a newer schema is worse than no key.
//   * Unknown keys warn and missing keys default. The defaults are the values this file's own
//     `BarConfig` initializers hold, so there is exactly one place a default is written down and no
//     embedded TOML copy to drift from it.
//   * A value that cannot be used is reported with the value that was kept instead, never silently
//     replaced: a warning naming `bar.height` is what tells a user their edit did nothing.
//
// Warnings and errors are separated by what the caller should do about them. A warning means the rest
// of the file was usable and has been applied; an error means the file was not usable at all and
// whatever was configured before still stands.
#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>

#include <array>
#include <optional>

namespace quantum::config {

// The forms the memory readout can be drawn in, spelled once, here, and declared above the struct that
// defaults to one of them so the spelling exists in exactly one place. The *rendering* is the widget's — a
// string for a person to read is presentation, and `src/` does not format one — so what this list is for is
// refusing a spelling the shell does not know and handing the accepted one to QML. `bar-interaction-test`
// drives every token below through the shipped `qml/SystemMonitor.qml`, which is what makes a token renamed
// on one side and not the other fail a test rather than fall back to a default on screen.
//
//   used_of_total   the used share of what the machine has, "7.4/31G"
//   used            the used share alone, "7.4G"
//   available       what could still be handed out, "24G"
//   percent         the used share as a percentage, "24%"
// The cadence the shell ships with, in milliseconds: the same number `SysMonService::DefaultSampleIntervalMs`
// states, and named here rather than written only into the initializer below for one reason — a compile-time
// guard has to be able to read it, and the struct itself stopped being usable in a constant expression when it
// gained the memory form's string. `config-test` compares the two names at compile time, which is what keeps
// the file's default and the service's from drifting apart.
inline constexpr int DefaultSampleIntervalMs = 2000;

// The units the volume readout can be drawn in, spelled once here for the reason the memory forms are: the
// schema refuses a spelling it does not know by name, so the widget can never be handed a unit it has no
// branch for.
//
// Two units and not three, because these are the two numbers the desktop's own tools print for one sink's
// factor, both read off this session: `wpctl get-volume` prints the percentage — WirePlumber's cube root of
// the linear factor, which every volume control on the desktop agrees about — and `pactl list sinks` prints
// the decibels beside it, `27525 /  42% / -22,61 dB` for the factor 0.074087. A third token for the raw linear
// factor is deliberately absent: no tool on the desktop shows a person that number, and a readout is a thing a
// person reads.
//
//   percent    the desktop's convention, "35%"
//   decibel    the physical gain, "-22.6 dB", and "-∞ dB" for a sink at silence
inline constexpr auto VolumeScalePercent = "percent";
inline constexpr auto VolumeScaleDecibel = "decibel";
inline constexpr std::array<const char*, 2> VolumeScales{VolumeScalePercent, VolumeScaleDecibel};

inline constexpr auto MemoryFormatUsedOfTotal = "used_of_total";
inline constexpr auto MemoryFormatUsed = "used";
inline constexpr auto MemoryFormatAvailable = "available";
inline constexpr auto MemoryFormatPercent = "percent";
inline constexpr std::array<const char*, 4> MemoryFormats{
    MemoryFormatUsedOfTotal, MemoryFormatUsed, MemoryFormatAvailable, MemoryFormatPercent};

// The `[bar.system]` table: the settings of the system status readout.
struct SystemConfig {
    // How far apart two /proc readings are, in milliseconds. It is the cadence `SysMonService` samples at and
    // therefore the interval its CPU percentage is an average over, which is why the name carries its unit:
    // the question this value invites is "milliseconds or seconds?", and a name that answers it is cheaper
    // than a comment nobody reads next to the number.
    //
    // The two numbers below are the same two `SysMonService.h` states, which is where the reason for each
    // lives — 2 s of policy, 10 ms of kernel. They are spelled here rather than included from there because a
    // schema is a pure function of the file's text and has no business pulling a QObject service into every
    // translation unit that reads a config value; `config-test` compares both pairs at compile time, so this
    // is a mirror with a guard rather than a copy that can drift.
    int sampleIntervalMs = DefaultSampleIntervalMs;

    // Whether the CPU readout is drawn, and whether the memory one is. Two flags rather than one
    // `readings = [...]` list for the reason the list would exist: a list decides an order, and the order
    // two readouts appear in is arrangement — which QUANTUM_SHELL.md gives to `qml/` and which `Bar.qml`
    // already decides when it says the system status sits in the centre group. A list here would be a
    // second place the bar's own layout is written down, and the two could disagree.
    //
    // Hiding a readout is not the same as switching off its reading: the service samples `/proc/stat` and
    // `/proc/meminfo` together, in one pass, whatever is drawn, so a hidden readout costs nothing and a
    // shown one is never a stale number waiting for a flag to change.
    bool showCpu = true;
    bool showMemory = true;

    // The form the memory readout is drawn in: one of `MemoryFormats` below, defaulting to
    // `MemoryFormatUsedOfTotal`. It is a spelling rather than a template — the widget renders it, and a
    // format string would be a small language this shell has no use for. A token the schema does not know is
    // refused by name, so the widget can never be handed one it has no branch for.
    //
    // There is no `cpu_format` beside it, and that is a decision rather than an omission: the only honest
    // form for a CPU percentage today is a percentage, and a key whose every legal value renders the same
    // thing is a key that does nothing. A second form arrives with the widget that can draw it.
    QString memoryFormat = QLatin1String(MemoryFormatUsedOfTotal);

    bool operator==(const SystemConfig&) const = default;
};

// The shortest sampling interval a file may ask for, in milliseconds: the same floor the service refuses
// below, stated once here for the file and once there for the code, with the guard above keeping them equal.
// One `USER_HZ` tick is 10 ms, and an interval shorter than a tick can span no tick at all.
inline constexpr int MinSampleIntervalMs = 10;

// The wheel's step in percentage points, and how small a step a file may ask for. Both are the numbers
// `PipeWireService` states — `DefaultStepPercent` and `MinimumStepPercent`, where the reason for each
// lives — and both are spelled here for the reason the sampling cadence is: a compile-time guard has to be
// able to read them, and `config-test` compares the two pairs, so the file's default and the service's
// cannot drift.
//
// There is no ceiling on the step, and that is a decision rather than an omission: `steppedPercent`
// clamps the result at full scale, so a step larger than the range is a person asking for one notch to
// reach the end. A floor of one point is not the same kind of thing — a step of zero is a wheel that does
// nothing at all, and it is refused by name.
inline constexpr int DefaultVolumeStepPercent = 5;
inline constexpr int MinVolumeStepPercent = 1;

// The same distance in the other unit, applied when `volumeScale` is `decibel`. One decibel is a mixer's own
// coarse step, and the floor is the resolution of the readout it is measured against rather than a judgement
// about hearing: the bar draws decibels to one decimal place, so a step below a tenth of one is a wheel notch
// that changes nothing on screen. `step_percent`'s floor is the same rule applied to the other readout, which
// is drawn as whole points. `PipeWireService` declares both numbers and `config-test` compares the two
// spellings at compile time, the rule the sampling cadence is held to.
inline constexpr double DefaultVolumeStepDecibels = 1.0;
inline constexpr double MinVolumeStepDecibels = 0.1;

// The `[bar.audio]` table: the settings of the volume readout in the bar's trailing group.
struct AudioConfig {
    // Whether the readout is drawn. The same shape and the same meaning as `[bar.system]`'s `show_cpu` and
    // `show_memory`: hiding a readout is not switching off its reading, and the service follows the
    // default sink whether or not anything is drawn from it.
    bool showVolume = true;

    // The unit the readout is drawn in: one of `VolumeScales` above, defaulting to `VolumeScalePercent`.
    //
    // It is one question with two answers rather than a spelling: the volume the daemon owns is the same
    // number either way, and what the unit decides is what is *drawn* and which of the two step keys a wheel
    // notch applies — `stepPercent` in this unit, `stepDecibels` in the other. That is deliberate, and it is
    // the arithmetic that argues for it: a percentage step is a growing distance in gain, the default five
    // points being 1.28 dB at the top of the range and 46.7 dB at the bottom of it, so a person reading
    // decibels and moving by points would be moved by a number that depends on where they already are. A
    // step measured in one unit while the readout is in another is the disagreement this avoids; the two
    // steps are separate keys rather than one rescaled value because the same key meaning two things
    // depending on another key is a schema that lies about itself.
    QString volumeScale = QLatin1String(VolumeScalePercent);

    // How far one wheel notch moves the volume in percentage points, applied when `volumeScale` is `percent`.
    // Read by the service rather than by the widget: the step is the distance a notch moves the *volume*,
    // which is a value the daemon owns, and a widget that computed the new level itself would be a second
    // place the volume is decided.
    int stepPercent = DefaultVolumeStepPercent;

    // The same distance in decibels, applied when `volumeScale` is `decibel`, and read by the service for the
    // same reason. Two keys rather than one because a notch is a distance in the unit a person is reading, and
    // which unit that is is already the key above.
    double stepDecibels = DefaultVolumeStepDecibels;

    bool operator==(const AudioConfig&) const = default;
};

// The `[bar.network]` table: the settings of the network readout in the bar's trailing group.
//
// Three flags and no key for what is *read*, which is the shape `[bar.system]` established and here it is
// forced by the data rather than chosen: NetworkManager decides what the connection is, whether there is a
// signal quality to show, and whether it thinks the network works, and the shell's only decisions are which
// of those facts a person wants on a bar. There is deliberately no `state_format` or `signal_format` — see the
// flag below for what a signal quality has instead of one, and a state has exactly one honest spelling.
struct NetworkConfig {
    // Whether the readout is drawn at all. The same shape and meaning as `[bar.system]`'s and `[bar.audio]`'s:
    // hiding a readout is not switching off its reading, and the service follows the connection whether or not
    // anything is drawn from it.
    bool showStatus = true;

    // Whether the connection's own name is drawn: the SSID when the connection is wireless, the profile's name
    // when it is not. It is a flag rather than a format because a name is not a number and has no forms — it has
    // a length, and a name can be as long as an SSID, so whether it belongs on a bar is a person's decision
    // about their own screen. With it off, a connected readout is the state and, for a wireless connection, the
    // signal quality — never a stand-in for the name.
    bool showName = true;

    // Whether a wireless connection's signal quality is drawn. This is the readout's one quantity and it has a
    // flag rather than a form for two reasons, both about what the number *is*: NetworkManager publishes it as a
    // byte of quality and not as a strength in dBm, so percent is the only unit it is in, and the only other way
    // a person draws it — bars, a coarse shape rather than a number — is a glyph this bar has no font for. So
    // the key says whether the number appears and there is no `signal_format` beside it: a key whose every legal
    // value draws the same thing is a key that does nothing.
    bool showStrength = true;

    bool operator==(const NetworkConfig&) const = default;
};

// The `[bar.battery]` table: the settings of the battery readout in the bar's trailing group.
//
// Three flags and no key for what is *read*, which is the shape `[bar.system]` established and here it is
// forced by the data rather than chosen: UPower decides whether there is a battery at all, what its charge is,
// whether the charge is going in or out, how long it says that will take and how worried it is, and the shell's
// only decisions are which of those a person wants on a bar. There is deliberately no `format` key for the
// level and no unit for the time — see the flag below for what a percentage has instead of a format, and the
// daemon's clock is already in hours and minutes with one honest spelling.
struct BatteryConfig {
    // Whether the readout is drawn at all. The same shape and meaning as the three tables above it: hiding a
    // readout is not switching off its reading, and the service follows the daemon whether or not anything is
    // drawn from it. It is also not the same question as whether this machine *has* a battery, which is the
    // daemon's answer and is drawn as nothing at all — a machine with no battery has no readout to configure.
    bool showStatus = true;

    // Whether the charge level is drawn. A flag rather than a format for the reason the network readout's signal
    // quality is one: UPower publishes the level as a percentage and nothing else — it is a proportion of the
    // battery's own full charge, not an amount of energy — so percent is the only unit it is in, and the only
    // other way a person draws it, a bar of cells, is a glyph this bar has no font for. A key whose every legal
    // value draws the same thing is a key that does nothing.
    bool showPercentage = true;

    // Whether the time the daemon reports is drawn beside the level — the charge it says is left before the
    // battery is empty, or before it is full. On by default for the reason the readings of the other three
    // tables are: it is a fact the daemon computed, and a shell that hid it until someone edited a file would be
    // shipping a configuration nobody asked for. With it off the readout is the level and the state, which is
    // the whole of what a person who does not trust a time estimate wants.
    bool showTime = true;

    bool operator==(const BatteryConfig&) const = default;
};

// The `[bar.media]` table: the settings of the media player readout in the bar's trailing group.
//
// One flag and no keys for what is *read*, which is the same shape the network and battery tables took: MPRIS
// players decide what track is playing, who made it, and whether it is playing or paused, and the shell's only
// decision is whether that readout appears. There is deliberately no format key — a track has a title and an
// artist, both names, and neither has a form the way a number does.
struct MediaConfig {
    // Whether the readout is drawn at all. The same shape and meaning as the four tables above it: hiding the
    // readout is not switching off its reading, and the service follows MPRIS players whether or not anything is
    // drawn from them. It is also not the same question as whether an MPRIS player *exists*, which is the
    // service's answer — when no player is running, or when the active player has no track loaded, the readout
    // draws nothing at all, which is one reading and not a failure.
    bool showMedia = true;

    bool operator==(const MediaConfig&) const = default;
};

// `[bar.notifications]`. One flag, for the reason the media table's single flag gives: everything else
// the readout could offer is a fact a sender owns — the summary, the body, the application name — so
// the only question left to the person is whether the readout is drawn. And unlike the media readout's
// "no player", the shell's notification reading can be absent for a reason the file does not control
// (another daemon owns the name), which is why `show_notifications` gates the drawing and not the
// reading: the shell takes the name either way.
struct NotificationsConfig {
    // Whether the readout is drawn at all. The service still owns the bus name when this is false, the
    // same way `SysMonService` still reads /proc when its readout is hidden.
    bool showNotifications = true;

    bool operator==(const NotificationsConfig&) const = default;
};

// The bar's configuration, validated. Every value here has been checked against the rules below, so a
// consumer may use it directly.
struct BarConfig {
    // The bar's height in logical pixels. It is also the exclusive zone reserved from the tiling area,
    // which is why it has to be a real height: a zero-height bar would reserve nothing and draw nothing.
    int height = 32;

    // The zwlr_layer_surface_v1 namespace. AGENTS.md freezes the shell's namespaces as
    // `quantum-shell-*`, and this is the name `niri msg layers` reports, so a value outside that prefix
    // is refused here as well as in the layer-shell integration — this is the earlier of the two, and
    // the one that can name the file a user has to edit.
    //
    // The value is read when the surface role is assigned, which happens once. A change to it on a
    // running shell is reported by the layer surface rather than silently ignored.
    QString layerNamespace = QStringLiteral("quantum-shell-bar");

    // The `[bar.system]` table. A table rather than a compound key name like `system_sample_interval_ms`, so
    // that the widget that owns these settings owns a table in the file — which is the shape the readouts
    // still to come will take.
    SystemConfig system;

    // The `[bar.audio]` table, the second readout to take that shape. Its own table rather than keys on
    // `[bar]` because the settings belong to the widget that reads them, which is the rule the system
    // table's comment states and the reason it is a rule rather than an accident.
    AudioConfig audio;

    // The `[bar.network]` table, the third. Same rule, same shape: a readout's settings are written together
    // and they belong to the widget that reads them.
    NetworkConfig network;

    // The `[bar.battery]` table, the fourth. Same rule again, and it is now the shape every readout's settings
    // take rather than a pattern three tables happened to follow.
    BatteryConfig battery;

    // The `[bar.media]` table, the fifth. Same rule, same shape: the readout's one setting is written in its own
    // table and belongs to the widget that reads it.
    MediaConfig media;

    // The `[bar.notifications]` table, the sixth. Same rule, same shape: the readout's one setting is
    // written in its own table and belongs to the widget that reads it.
    NotificationsConfig notifications;

    bool operator==(const BarConfig&) const = default;
};

// The whole validated configuration. Nested objects mirror the file's tables, so `bar` is the `[bar]`
// table and nothing else is interpolated between the file and this struct.
struct ConfigValues {
    BarConfig bar;

    bool operator==(const ConfigValues&) const = default;
};

// The schema version this build writes and understands. Migrations are pure functions that will live in
// this file beside `parseConfig` when there is a second version to migrate from; today `1` is the only
// version that has ever existed, so a migration path would be a branch nothing can take.
inline constexpr int SchemaVersion = 1;

// The shortest bar a file may ask for, in logical pixels. Zero is not a bar — it reserves no exclusive
// zone and draws nothing — so the floor is one pixel, and it is named here rather than written into the
// comparison so that `spec-values-test` can hold the bound the engineering spec states against the bound
// this schema applies. An upper bound is not named beside it: `INT_MAX` is the largest an `int` holds, and
// a named constant equal to it would be a second spelling of the same thing.
inline constexpr int MinBarHeight = 1;

// The prefix every layer-shell namespace must begin with. Spelled here and in
// `src/wayland/LayerShellIntegration.cpp`, where it is the last check before the name reaches the
// protocol; both are deliberate, because a name that reaches niri cannot be taken back.
inline constexpr auto LayerNamespacePrefix = "quantum-shell-";

// The outcome of reading one file.
struct ParseResult {
    // The defaults, with every value the file supplied and this build accepted applied over them. On an
    // error this is still the defaults — the caller decides whether to use them, and the watcher's
    // answer is not to: a file with a typo in it must not reset a working configuration.
    ConfigValues values;

    // Problems that did not stop the rest of the file from being used, one line each, in the order they
    // were found: an unknown key, a value of the wrong type, a value that was refused, a missing
    // `schema_version`. Each names the key and says what was kept instead.
    QStringList warnings;

    // Problems that made the file unusable: it is not TOML, or it was written for a schema version this
    // build does not know. Non-empty means `values` must not be applied.
    QStringList errors;
};

// Parses and validates one configuration file. `text` is the file's whole contents; a file that does not
// exist is the caller's concern and is not an error (QUANTUM_SHELL.md: defaults ship embedded, so a
// missing config file is not an error).
ParseResult parseConfig(const QByteArray& text);

// The keys by the path a user names them, which is the spelling `qsctl config get` takes. Declared here
// rather than wherever they are asked for, because this file is where a key exists: the schema is what
// reads a key, what defaults it, and what refuses it. A path in this list that resolves to nothing, or a
// key the schema reads that is missing from it, is a name that lies about what the shell reads — so
// `config-test` walks the list in both directions and fails on either.
inline constexpr auto KeyBarHeight = "bar.height";
inline constexpr auto KeyBarLayerNamespace = "bar.layerNamespace";
inline constexpr auto KeyBarSystemSampleIntervalMs = "bar.system.sample_interval_ms";
inline constexpr auto KeyBarSystemShowCpu = "bar.system.show_cpu";
inline constexpr auto KeyBarSystemShowMemory = "bar.system.show_memory";
inline constexpr auto KeyBarSystemMemoryFormat = "bar.system.memory_format";
inline constexpr auto KeyBarAudioShowVolume = "bar.audio.show_volume";
inline constexpr auto KeyBarAudioVolumeScale = "bar.audio.volume_scale";
inline constexpr auto KeyBarAudioStepPercent = "bar.audio.step_percent";
inline constexpr auto KeyBarAudioStepDecibels = "bar.audio.step_decibels";
inline constexpr auto KeyBarNetworkShowStatus = "bar.network.show_status";
inline constexpr auto KeyBarNetworkShowName = "bar.network.show_name";
inline constexpr auto KeyBarNetworkShowStrength = "bar.network.show_strength";
inline constexpr auto KeyBarBatteryShowStatus = "bar.battery.show_status";
inline constexpr auto KeyBarBatteryShowPercentage = "bar.battery.show_percentage";
inline constexpr auto KeyBarBatteryShowTime = "bar.battery.show_time";
inline constexpr auto KeyBarMediaShowMedia = "bar.media.show_media";
inline constexpr auto KeyBarNotificationsShowNotifications = "bar.notifications.show_notifications";
inline constexpr std::array<const char*, 18> KeyPaths{
    KeyBarHeight,
    KeyBarLayerNamespace,
    KeyBarSystemSampleIntervalMs,
    KeyBarSystemShowCpu,
    KeyBarSystemShowMemory,
    KeyBarSystemMemoryFormat,
    KeyBarAudioShowVolume,
    KeyBarAudioVolumeScale,
    KeyBarAudioStepPercent,
    KeyBarAudioStepDecibels,
    KeyBarNetworkShowStatus,
    KeyBarNetworkShowName,
    KeyBarNetworkShowStrength,
    KeyBarBatteryShowStatus,
    KeyBarBatteryShowPercentage,
    KeyBarBatteryShowTime,
    KeyBarMediaShowMedia,
    KeyBarNotificationsShowNotifications};

// The validated value at `path`, or nothing when this build reads no such key. The value is the one the
// bar was built with rather than the text in the file: a height the schema refused never appears here.
std::optional<QVariant> configValueForPath(const ConfigValues& values, QStringView path);

}  // namespace quantum::config
