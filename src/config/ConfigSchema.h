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
inline constexpr std::array<const char*, 2> KeyPaths{KeyBarHeight, KeyBarLayerNamespace};

// The validated value at `path`, or nothing when this build reads no such key. The value is the one the
// bar was built with rather than the text in the file: a height the schema refused never appears here.
std::optional<QVariant> configValueForPath(const ConfigValues& values, QStringView path);

}  // namespace quantum::config
