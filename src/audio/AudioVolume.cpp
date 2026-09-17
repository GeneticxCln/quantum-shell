#include "audio/AudioVolume.h"

#include "app/Logging.h"

#include <spa/param/props.h>
#include <spa/param/param.h>
#include <spa/param/type-info.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <spa/pod/vararg.h>
#include <spa/utils/type.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>

namespace quantum::audio {

std::optional<SinkState> parseProps(const spa_pod* props)
{
    // Two refusals that are really one: a pod that is not a Props object is a daemon answering a Props
    // request with something else, and one that is a Props object without `channelVolumes` is a Props
    // object that says nothing about volume. Neither is a volume, so neither is a reading.
    if (!spa_pod_is_object_type(props, SPA_TYPE_OBJECT_Props))
        return std::nullopt;

    const spa_pod_prop* volumes = spa_pod_find_prop(props, nullptr, SPA_PROP_channelVolumes);
    if (volumes == nullptr)
        return std::nullopt;

    uint32_t count = 0;
    uint32_t elementSize = 0;
    uint32_t elementType = 0;
    const void* values = spa_pod_get_array_full(&volumes->value, &count, &elementSize, &elementType);

    // The element type is checked rather than assumed. `spa_pod_get_array` returns the body of any array
    // without looking at what it holds, so a `channelVolumes` that arrived as integers would be read as
    // floats and produce a volume that is not the daemon's — a number on the bar that nothing sent.
    if (values == nullptr || elementType != SPA_TYPE_Float || elementSize != sizeof(float) || count == 0)
        return std::nullopt;

    SinkState state;
    state.channels = static_cast<int>(count);
    state.linearVolume = static_cast<double>(static_cast<const float*>(values)[0]);

    // Mute is optional in a Props object: a sink that never mentions it is not muted, rather than
    // unknown, because absence in Props is the default and the default is unmuted.
    if (const spa_pod_prop* mute = spa_pod_find_prop(props, nullptr, SPA_PROP_mute)) {
        bool muted = false;
        if (spa_pod_get_bool(&mute->value, &muted) == 0)
            state.muted = muted;
    }

    return state;
}

int percentFromLinear(double linear)
{
    if (!(linear > 0.0))
        return 0;  // also the answer for a negative or NaN factor, which `!(x > 0)` covers
    // Rounded rather than truncated: 0.35³ is 0.042875, and truncation would drop a step between the
    // shell's own read and write of the same volume.
    return static_cast<int>(std::lround(std::cbrt(linear) * 100.0));
}

double decibelsFromLinear(double linear)
{
    // One comparison for the three non-positive cases, the shape `percentFromLinear` uses: `!(x > 0)` is how
    // a NaN is caught as well as a zero and a negative, and all three are silence rather than a number.
    if (!(linear > 0.0))
        return -std::numeric_limits<double>::infinity();
    return 20.0 * std::log10(linear);
}

double linearFromPercent(int percent)
{
    const double share = static_cast<double>(percent < 0 ? 0 : percent) / 100.0;
    return share * share * share;
}

int steppedPercent(int current, int direction, int stepPercent, int maxPercent)
{
    if (direction == 0 || stepPercent <= 0)
        return current;
    const qint64 stepped = static_cast<qint64>(current) + static_cast<qint64>(direction) * stepPercent;
    if (stepped < 0)
        return 0;
    if (stepped > maxPercent)
        return maxPercent;
    return static_cast<int>(stepped);
}

std::optional<WheelStepUnit> wheelStepUnitFromToken(QStringView token)
{
    // Compared through `QString` rather than against a Latin-1 literal directly: a token is a handful of ASCII
    // characters from a configuration file, this runs when a value is handed over rather than per wheel notch,
    // and one comparison spelling is cheaper to be right about than a conversion rule.
    const QString text = token.toString();
    if (text == QLatin1String("percent"))
        return WheelStepUnit::Percent;
    if (text == QLatin1String("decibel"))
        return WheelStepUnit::Decibel;
    return std::nullopt;
}

double linearAfterWheelStep(double linear, int direction, WheelStepUnit unit, int stepPercent,
                            double stepDecibels, int maxPercent)
{
    const double ceiling = linearFromPercent(maxPercent);
    // The factor clamped into what the wheel is willing to write, with everything that is not a positive
    // number — a zero for a sink turned all the way down, a negative or a NaN for a daemon not telling us a
    // volume — read as silence. One comparison for all three, the shape `percentFromLinear` uses.
    const double current = linear > 0.0 ? std::min(linear, ceiling) : 0.0;

    // A direction the widget never sends and a step of no size: both leave the factor where it is rather than
    // moving it to one end, which is what an unchecked multiplication would do. `steppedPercent`'s rule.
    if (direction == 0)
        return current;
    if (unit == WheelStepUnit::Percent)
        return linearFromPercent(steppedPercent(percentFromLinear(linear), direction, stepPercent, maxPercent));
    if (!(stepDecibels > 0.0))
        return current;

    // Zero has no ratio, so a notch from silence cannot be a gain: the quietest level this shell writes is
    // where it lands, and a quieter notch from silence stays at silence. `AudioVolume.h` carries the argument.
    if (!(linear > 0.0))
        return direction > 0 ? linearFromPercent(1) : 0.0;

    const double stepped = linear * std::pow(10.0, static_cast<double>(direction) * stepDecibels / 20.0);
    return std::clamp(stepped, 0.0, ceiling);
}

std::optional<QString> sinkNameFromMetadata(QStringView value)
{
    const QJsonDocument document = QJsonDocument::fromJson(value.toString().toUtf8());
    if (!document.isObject())
        return std::nullopt;
    const QJsonValue name = document.object().value(QStringLiteral("name"));
    if (!name.isString() || name.toString().isEmpty())
        return std::nullopt;
    return name.toString();
}

PropsWrite::PropsWrite(int channels, double linearVolume, bool muted)
{
    // Every channel gets the same factor, which is what a volume control does and what `wpctl` writes:
    // a per-channel balance is a different setting this shell does not offer, and writing one channel
    // alone would silently unbalance a stereo sink.
    const int count = channels > 0 ? channels : 2;
    if (count > MaxChannels) {
        qCWarning(quantum::app::audioLog)
            << "refusing to write a volume for" << count << "channels; a sink with more than"
            << MaxChannels << "is not one this shell has been asked to control";
        // No pod: `pod()` stays null and the caller writes nothing. Returning is honest — the
        // alternative would be writing the first 64 channels of a sink and leaving the rest alone.
        return;
    }
    const auto factor = static_cast<float>(linearVolume);

    // Built with SPA's own builder functions rather than its vararg macros (`SPA_POD_BUILDER_INIT`,
    // `spa_pod_builder_add_object`, `SPA_POD_Array`). Both spellings produce the same pod, and the macros
    // were the reason this file did not compile under clang: they are compound literals and GNU statement
    // expressions, which `-Werror` in the CI matrix's clang job refuses as extensions. The functions below
    // are the macros' own expansion, named one step at a time, so the pod is the library's either way and
    // the build no longer depends on a GNU extension. Every step is checked because a builder that runs out
    // of room returns a negative result rather than truncating: a write that cannot be built whole is not
    // built at all, and `pod()` stays null for the caller to honour.
    spa_pod_builder builder{};
    builder.data = buffer_.data();
    builder.size = static_cast<uint32_t>(buffer_.size());

    spa_pod_frame props{};
    spa_pod_frame array{};
    bool built = spa_pod_builder_push_object(&builder, &props, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props) >= 0
                 && spa_pod_builder_prop(&builder, SPA_PROP_channelVolumes, 0) >= 0
                 && spa_pod_builder_push_array(&builder, &array) >= 0;
    for (int channel = 0; built && channel < count; ++channel)
        built = spa_pod_builder_float(&builder, factor) >= 0;
    built = built && spa_pod_builder_pop(&builder, &array) != nullptr;
    built = built && spa_pod_builder_prop(&builder, SPA_PROP_mute, 0) >= 0
            && spa_pod_builder_bool(&builder, muted) >= 0;
    if (!built) {
        qCWarning(quantum::app::audioLog) << "could not build a Props write; the volume was not set";
        return;
    }
    pod_ = static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &props));
    if (pod_ == nullptr)
        qCWarning(quantum::app::audioLog) << "could not build a Props write; the volume was not set";
}

}  // namespace quantum::audio
