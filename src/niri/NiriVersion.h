// niri version and capability detection.
//
// Every niri-version-dependent decision in the shell goes through this file: the version the running
// compositor reports over IPC, the project's minimum supported niri, and the features that are
// decided by asking the compositor rather than by assuming what it can do.
//
// Two rules shape it. Nothing is assumed available — a feature nothing has answered for is Unknown,
// not enabled. And a version string that is not a year.month is reported as invalid instead of being
// read as some other version, so an unrecognised compositor is visible rather than silently misread.
#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringView>

namespace quantum::niri {

// The version string niri reports over IPC, e.g. "26.04 (8ed0da4)".
//
// niri is versioned year.month; the IPC crate for the same release is "=26.4.0" (niri-ipc v26.04,
// the release this was verified against). The parenthesised suffix is the build commit: it is kept
// for reporting and never compared.
class NiriVersion {
public:
    // The floor: SYSTEM_PROMPT.md § Version floors, QUANTUM_SHELL.md § Version floors.
    static constexpr int minimumYear = 26;
    static constexpr int minimumMonth = 4;

    // Invalid (isValid() == false) for anything that is not "<year>.<month>" with a real month, so
    // an unrecognised build is reported as unknown rather than as a version we happen to accept.
    static NiriVersion parse(QStringView raw);

    bool isValid() const;
    bool atLeast(int year, int month) const;
    // True only when a version was recognised and it is at least the project's floor.
    bool isSupported() const;

    int year() const;
    int month() const;
    QString commit() const;
    QString toString() const;

private:
    int year_ = 0;
    int month_ = 0;
    QString commit_;
};

// A capability the shell gates on the compositor build.
//
// Most entries are IPC requests: niri answers a request it does not know with an error, so asking is
// the only honest way to know what a given build accepts. `compositorBackgroundEffect` is not a
// request — niri >= 26.04 is what exposes `ext-background-effect`, so the version decides it.
enum class NiriFeature {
    compositorBackgroundEffect,
    version,
    outputs,
    workspaces,
    windows,
    layers,
    keyboardLayouts,
    focusedOutput,
    focusedWindow,
    overviewState,
    casts,
};

// What is known about a feature. Unknown is where everything starts and where it stays until the
// version or a probe answers.
enum class Availability {
    unknown,
    unsupported,
    supported,
};

// Stable identifier for logs and test failure messages, e.g. "focused-window".
QString featureName(NiriFeature feature);
// The niri request name from niri-ipc v26.04's `Request` enum, empty for a version-gated feature.
QString requestName(NiriFeature feature);
// The `Response` variant an Ok reply must carry, empty for a version-gated feature. Most responses
// are named after their request, but not all of them, so the mapping is stored rather than derived.
QString responseVariant(NiriFeature feature);
QString availabilityName(Availability availability);
// The read-only requests probed at detection time, in probe order. Interactive, blocking and
// state-changing requests (PickWindow, PickColor, Action, Output, EventStream) are deliberately
// absent: a capability probe must never cause an effect.
QList<NiriFeature> probedFeatures();

// What the running compositor was observed to support.
//
// Version-gated entries are decided as soon as a version is known; request-backed entries are filled
// in by NiriIPC's probe, one request each. Anything neither source has answered stays Unknown, so a
// caller can tell "not supported" apart from "not known".
class NiriCapabilities {
public:
    NiriCapabilities() = default;
    explicit NiriCapabilities(const NiriVersion& version);

    void setVersion(const NiriVersion& version);
    NiriVersion version() const;

    void recordProbe(NiriFeature feature, Availability state);
    Availability availability(NiriFeature feature) const;
    bool isSupported(NiriFeature feature) const;

    // "version 26.04 (8ed0da4), compositor-background-effect=supported, version=supported, ..."
    QString describe() const;

private:
    // Recomputes what the version alone decides.
    void applyVersionGated();

    NiriVersion version_;
    QHash<int, Availability> states_;
};

}  // namespace quantum::niri
