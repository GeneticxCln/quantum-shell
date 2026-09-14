#include "niri/NiriVersion.h"

#include <QRegularExpression>
#include <QStringList>

#include <array>

namespace quantum::niri {
namespace {

// "26.04 (8ed0da4)" as reported by the running compositor, and the bare "26.04" form without a build
// commit. niri versions are two digits of year and two of month, so a longer year ("2026.04") and a
// month outside 01..12 are rejected rather than read as some other, larger version.
const QRegularExpression& versionExpression() {
    static const QRegularExpression expression(
        QStringLiteral(R"(^\s*(\d{2})\.(\d{2})(?:\s+\(([^)]*)\))?\s*$)"));
    return expression;
}

struct FeatureEntry {
    NiriFeature feature;
    const char* name;     // stable identifier for logs and tests
    const char* request;  // niri-ipc v26.04 `Request` variant, empty when the version decides it
    const char* variant;  // the `Response` variant that answers `request`, empty when there is none
};

// Both spellings are written out, rather than derived from each other: niri names most responses
// after their request, but not all of them (PickWindow answers with PickedWindow, Output with
// OutputConfigChanged), so a shared spelling would be an assumption that breaks on the next feature
// added here. Verified against a running niri 26.04, one request at a time.
constexpr std::array<FeatureEntry, 11> features{{
    {NiriFeature::compositorBackgroundEffect, "compositor-background-effect", "", ""},
    {NiriFeature::version, "version", "Version", "Version"},
    {NiriFeature::outputs, "outputs", "Outputs", "Outputs"},
    {NiriFeature::workspaces, "workspaces", "Workspaces", "Workspaces"},
    {NiriFeature::windows, "windows", "Windows", "Windows"},
    {NiriFeature::layers, "layers", "Layers", "Layers"},
    {NiriFeature::keyboardLayouts, "keyboard-layouts", "KeyboardLayouts", "KeyboardLayouts"},
    {NiriFeature::focusedOutput, "focused-output", "FocusedOutput", "FocusedOutput"},
    {NiriFeature::focusedWindow, "focused-window", "FocusedWindow", "FocusedWindow"},
    {NiriFeature::overviewState, "overview-state", "OverviewState", "OverviewState"},
    {NiriFeature::casts, "casts", "Casts", "Casts"},
}};

const FeatureEntry* entryFor(NiriFeature feature) {
    for (const auto& entry : features) {
        if (entry.feature == feature) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

NiriVersion NiriVersion::parse(QStringView raw) {
    NiriVersion version;
    const QRegularExpressionMatch match = versionExpression().match(raw.toString());
    if (!match.hasMatch()) {
        return version;
    }

    const int year = match.captured(1).toInt();
    const int month = match.captured(2).toInt();
    if (year <= 0 || month < 1 || month > 12) {
        return version;
    }

    version.year_ = year;
    version.month_ = month;
    version.commit_ = match.captured(3);
    return version;
}

bool NiriVersion::isValid() const {
    return year_ > 0;
}

bool NiriVersion::atLeast(int year, int month) const {
    if (!isValid()) {
        return false;
    }
    if (year_ != year) {
        return year_ > year;
    }
    return month_ >= month;
}

bool NiriVersion::isSupported() const {
    return atLeast(minimumYear, minimumMonth);
}

int NiriVersion::year() const {
    return year_;
}

int NiriVersion::month() const {
    return month_;
}

QString NiriVersion::commit() const {
    return commit_;
}

QString NiriVersion::toString() const {
    if (!isValid()) {
        return QString{};
    }
    QString text = QStringLiteral("%1.%2").arg(year_).arg(month_, 2, 10, QLatin1Char('0'));
    if (!commit_.isEmpty()) {
        text += QStringLiteral(" (%1)").arg(commit_);
    }
    return text;
}

QString featureName(NiriFeature feature) {
    const FeatureEntry* entry = entryFor(feature);
    return entry == nullptr ? QString{} : QString::fromLatin1(entry->name);
}

QString requestName(NiriFeature feature) {
    const FeatureEntry* entry = entryFor(feature);
    return entry == nullptr ? QString{} : QString::fromLatin1(entry->request);
}

QString responseVariant(NiriFeature feature) {
    const FeatureEntry* entry = entryFor(feature);
    return entry == nullptr ? QString{} : QString::fromLatin1(entry->variant);
}

QString availabilityName(Availability availability) {
    switch (availability) {
    case Availability::unknown:
        return QStringLiteral("unknown");
    case Availability::unsupported:
        return QStringLiteral("unsupported");
    case Availability::supported:
        return QStringLiteral("supported");
    }
    return QStringLiteral("unknown");
}

QList<NiriFeature> probedFeatures() {
    // Request-backed, read-only, and answered standalone. `version` is not here because detection
    // asks for it first and records it from that reply, and `compositorBackgroundEffect` has no
    // request to send.
    return {
        NiriFeature::outputs,        NiriFeature::workspaces,     NiriFeature::windows,
        NiriFeature::layers,         NiriFeature::keyboardLayouts, NiriFeature::focusedOutput,
        NiriFeature::focusedWindow,  NiriFeature::overviewState,  NiriFeature::casts,
    };
}

NiriCapabilities::NiriCapabilities(const NiriVersion& version) : version_(version) {
    applyVersionGated();
}

void NiriCapabilities::setVersion(const NiriVersion& version) {
    version_ = version;
    applyVersionGated();
}

NiriVersion NiriCapabilities::version() const {
    return version_;
}

void NiriCapabilities::recordProbe(NiriFeature feature, Availability state) {
    states_.insert(static_cast<int>(feature), state);
}

Availability NiriCapabilities::availability(NiriFeature feature) const {
    return states_.value(static_cast<int>(feature), Availability::unknown);
}

bool NiriCapabilities::isSupported(NiriFeature feature) const {
    return availability(feature) == Availability::supported;
}

void NiriCapabilities::applyVersionGated() {
    // QUANTUM_SHELL.md § Compositor does the compositing: `ext-background-effect` is what niri 26.04
    // and later expose, which is also the project's floor. Below it, or with no version at all, the
    // answer is "unsupported" or "unknown" rather than a guess.
    Availability state = Availability::unknown;
    if (version_.isValid()) {
        state = version_.atLeast(NiriVersion::minimumYear, NiriVersion::minimumMonth)
                    ? Availability::supported
                    : Availability::unsupported;
    }
    states_.insert(static_cast<int>(NiriFeature::compositorBackgroundEffect), state);
}

QString NiriCapabilities::describe() const {
    QStringList parts;
    parts << (version_.isValid() ? QStringLiteral("version %1").arg(version_.toString())
                                 : QStringLiteral("version unknown"));
    for (const auto& entry : features) {
        parts << QStringLiteral("%1=%2")
                     .arg(QString::fromLatin1(entry.name),
                          availabilityName(availability(entry.feature)));
    }
    return parts.join(QStringLiteral(", "));
}

}  // namespace quantum::niri
