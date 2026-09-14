// A connected output, as niri reports it in niri-ipc v26.04's `Output`.
//
// Outputs are **not** part of the event stream: niri-ipc v26.04's `Event` enum has no output variant,
// and the event-stream state has no outputs part — a subscription to a running niri 26.04 is answered
// with workspaces, windows, keyboard layouts, overview, config and casts, and nothing about outputs.
// An output therefore arrives only as the answer to an `Outputs` request, which is why `NiriOutputs`
// exists to keep this in step with the events that can change it.
//
// Parsing is tolerant field by field, as with workspaces and windows; `name` is the exception,
// because an output without one cannot be matched against the output names that workspaces carry.
// Three fields of the wire struct are deliberately not parsed — `serial`, `physical_size` and
// `is_custom_mode` — because nothing in the shell reads them, and parsing what nothing uses is how a
// field ends up believed rather than exercised. They are ignored, not rejected.
//
// Two readings here are load-bearing for correctness rather than display: `logical` is absent exactly
// when the output is disabled or not mapped, and its `scale` is fractional (1.25 on the machine this
// was verified against), so nothing may assume an integer or a scale of 1.
#pragma once

#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <optional>

namespace quantum::niri {

// One mode an output offers, from niri-ipc v26.04's `Mode`. `refreshRate` is in millihertz exactly as
// niri reports it — 59997 is 59.997 Hz — and nothing here rounds it to a whole number.
struct NiriOutputMode {
    quint16 width = 0;  // physical pixels
    quint16 height = 0;
    quint32 refreshRate = 0;  // millihertz
    bool isPreferred = false;

    bool operator==(const NiriOutputMode& other) const;
    bool operator!=(const NiriOutputMode& other) const;
};

// Where an output sits in the compositor's coordinate space, from niri-ipc v26.04's `LogicalOutput`.
struct NiriLogicalOutput {
    qint32 x = 0;
    qint32 y = 0;
    quint32 width = 0;  // logical pixels, already accounting for the transform
    quint32 height = 0;
    double scale = 0.0;
    // Exactly the text niri sent. niri-ipc v26.04's `Transform` is one of Normal, _90, _180, _270,
    // Flipped, Flipped90, Flipped180, Flipped270; only `Normal` could be observed on the machine this
    // was verified against, so the value is kept as text rather than mapped to an enum whose mapping
    // would be an unverified guess about a rotated display.
    QString transform;

    // Whether `transform` is one of the eight names niri-ipc v26.04 defines. False means the
    // compositor sent a transform this build has never seen, which is worth seeing rather than
    // rounding to "normal".
    bool transformIsKnown() const;

    bool operator==(const NiriLogicalOutput& other) const;
    bool operator!=(const NiriLogicalOutput& other) const;
};

class NiriOutput {
public:
    // Every transform name niri-ipc v26.04 defines, for the test that pins this list to the protocol.
    static QStringList knownTransforms();

    // An invalid output (isValid() == false) when the object carries no usable `name`.
    static NiriOutput fromJson(const QJsonObject& object);

    bool isValid() const;

    QString name() const;   // the connector name, e.g. "DP-3"; what workspaces reference
    QString make() const;   // empty when the compositor reports none
    QString model() const;

    QList<NiriOutputMode> modes() const;
    // The index niri reports in `current_mode`, present only when the output has a current mode.
    // niri sends null for a disabled output, which is a different fact from an index this build
    // cannot resolve — currentMode() is what resolves it.
    std::optional<int> currentModeIndex() const;
    // The mode at that index, or nothing when the index is absent or outside the mode list.
    std::optional<NiriOutputMode> currentMode() const;

    bool isVrrSupported() const;
    bool isVrrEnabled() const;

    // False when niri reports no logical output, which is how it says the output is disabled or
    // unmapped. An output that is present but disabled is a real state, not a missing output.
    bool isEnabled() const;
    std::optional<NiriLogicalOutput> logical() const;

    // For log lines and test failure messages. Never shown in the UI.
    QString describe() const;

    bool operator==(const NiriOutput& other) const;
    bool operator!=(const NiriOutput& other) const;

private:
    bool valid_ = false;
    QString name_;
    QString make_;
    QString model_;
    QList<NiriOutputMode> modes_;
    std::optional<int> currentModeIndex_;
    bool vrrSupported_ = false;
    bool vrrEnabled_ = false;
    std::optional<NiriLogicalOutput> logical_;
};

}  // namespace quantum::niri

// Outputs travel through signals, so they can be queued across threads later.
Q_DECLARE_METATYPE(quantum::niri::NiriOutput)
Q_DECLARE_METATYPE(quantum::niri::NiriOutputMode)
Q_DECLARE_METATYPE(quantum::niri::NiriLogicalOutput)
Q_DECLARE_METATYPE(QList<quantum::niri::NiriOutput>)
