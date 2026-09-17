// The arithmetic and the parsing behind the bar's volume readout, as pure functions.
//
// Split out of `PipeWireService` for the reason `src/system/`'s /proc parsers are split out of
// `SysMonService`: everything below is a function of bytes, so every shape of input a daemon can send
// can be tested without one. `audio-test` therefore needs no PipeWire server, no socket and no display,
// and what is left in the service is plumbing — a thread loop, a registry, two subscriptions, and a
// write — which is what `audio-live-test` proves against a daemon the test starts itself.
//
// Everything here was read off the installed headers rather than recalled, because a name in this file
// that does not exist is a build error and a name that exists but means something else is worse:
//
//   * `SPA_PROP_channelVolumes`, `SPA_PROP_mute` and `SPA_PROP_volume` — `spa/param/props.h`.
//   * `spa_pod_find_prop`, `spa_pod_get_array`, `spa_pod_get_bool`, `spa_pod_get_float`,
//     `spa_pod_is_object_type` — `spa/pod/iter.h`.
//   * `spa_pod_builder_add_object`, `spa_pod_builder_prop`, `SPA_POD_Array` — `spa/pod/builder.h`
//     and `spa/pod/vararg.h`.
//
// The percentage is the one thing here that is a *convention* rather than a bit of the protocol, and it
// is verified rather than assumed: WirePlumber's own `wpctl get-volume` prints `0.35` for a sink whose
// Props param reports `channelVolumes: [0.042872, 0.042872]`, and 0.042872 is 0.35 cubed. So the number
// a person reads is the cube root of the linear factor, and the rest of the desktop agrees — taking the
// linear value for the percentage would put the bar at 4% where every other volume control says 35%.

#pragma once

#include <QString>
#include <QStringView>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

// Only ever a pointer here: the full definitions are SPA's, and a header that pulled `spa/pod/iter.h`
// into every translation unit that binds a volume property would be spreading a C API through the
// configuration schema and the QML registration for no reason. Callers that need to build or read a pod
// include SPA's headers themselves — `PipeWireService.cpp`, `audio_test.cpp`, `audio_live_test.cpp`.
struct spa_pod;

namespace quantum::audio {

// A sink's playback state, as PipeWire reports it in a `SPA_PARAM_Props` param.
struct SinkState {
    // Whether the sink is muted. Both `mute` and `softMute` exist in a Props param and they are
    // different things — `softMute` is what a client applying a crossfade uses — so this is `mute`,
    // which is what `wpctl` reads and writes.
    bool muted = false;

    // The first channel's linear volume factor, where 1.0 is no attenuation. All channels of a sink are
    // set together by every volume control on the desktop, so one channel's value is the sink's; the
    // count is kept beside it because a write has to name every channel it sets.
    double linearVolume = 0.0;
    int channels = 0;

    bool operator==(const SinkState&) const = default;
};

// Reads the volume and mute state out of one `SPA_PARAM_Props` pod. `nullopt` is a refusal — the pod is
// not a Props object, or it carries no `channelVolumes` — and never a default: a daemon that answers a
// Props request with something else has not told us a volume, and a zero standing in for "unknown" would
// be drawn as a real reading on the bar.
std::optional<SinkState> parseProps(const spa_pod* props);

// The display convention above: `percentFromLinear(0.042872)` is 35, and `linearFromPercent(35)` is
// 0.042875. Both refuse the nonsense ends — a negative linear volume is 0%, and a percentage below zero
// is 0 — rather than wrapping. A linear volume above 1.0 is not refused: PipeWire's own
// `channelmix.max-volume` defaults to 10.0, so amplifying past unity is a real state and
// `percentFromLinear` reports it (1000% at the default ceiling).
int percentFromLinear(double linear);
double linearFromPercent(int percent);

// The other honest number for the same factor: the gain in decibels, `20 * log10(linear)`.
//
// Two numbers, one reading, and they are not each other. The percentage above is WirePlumber's own cube root
// of the linear factor, which is the convention every volume control on the desktop shows; decibels are the
// physical gain the factor represents, which is what the desktop's *other* volume tool prints. Both were read
// off this session rather than recalled: for the sink whose Props carry 0.074087, `wpctl get-volume` prints
// `0.42` and `pactl list sinks` prints `27525 /  42% / -22,61 dB` — the 42% is the cube root and the -22.61 dB
// is 20*log10(0.074087). Neither is the linear factor times a hundred, which is the number that would be a
// third thing, faithful to nothing.
//
// **Silence is negative infinity rather than a floor**, and that is PulseAudio's own choice too: its
// `pa_sw_volume_snprint_dB` passes the value through `-INFINITY` and formats it, so a zero volume prints as
// `-inf dB` there. A floor — say -60 dB — would be a number for a volume that is not playing, and this shell
// draws a number only when it has one. A factor that is not positive at all is the same answer, for the reason
// `percentFromLinear` gives 0 for it: a sink reporting a negative or NaN volume is a daemon not telling us a
// volume, and silence is the honest reading of that rather than a NaN the widget would have to know about.
//
// Above unity is reported rather than clamped, like the percentage: at PipeWire's default
// `channelmix.max-volume` of 10.0 that is +20 dB, which is a real state rather than an error.
double decibelsFromLinear(double linear);

// One wheel step from `current`: `direction` is +1 for louder and -1 for quieter, and the result is
// clamped into 0..`maxPercent`. A direction of 0 is the current value. Stepping does not unmute —
// `wpctl set-volume` on a muted sink leaves it muted, and a wheel that also changed the mute state would
// be a second effect nobody asked for.
int steppedPercent(int current, int direction, int stepPercent, int maxPercent);

// **Which of the two units a wheel notch is measured in, which is also the unit the readout is drawn in.**
// One question with one answer: a person reading decibels is moved by a distance in decibels, and a person
// reading the desktop's percentage is moved by points of it, so what is drawn and what a notch does cannot
// disagree. The alternative — a fixed step in one unit whatever is drawn — is what this module did before,
// and the arithmetic is why it was worth changing: a percentage step is a *growing* distance in gain, from
// 1.28 dB at 99% to 46.7 dB at 1% for the default five points, so a readout in decibels was being moved by a
// number that depended on where it already was (both figures measured from this module's own arithmetic, and
// pinned in `audio-test`).
//
// An enum here rather than the configuration's token, because this file has no configuration to read: the
// composition root resolves the token with `wheelStepUnitFromToken` below and the token's mirror in the
// schema is held at compile time by the test that links both libraries.
enum class WheelStepUnit {
    // The desktop's own convention: `stepPercent` points of `percentFromLinear`.
    Percent,
    // The physical gain, `20 * log10`: the factor the daemon holds is multiplied by `10^(dB/20)`.
    Decibel,
};

// The token `[bar.audio].volume_scale` carries, as the unit a notch is measured in. `nullopt` for a spelling
// this module does not know — the schema refuses one before it can arrive here, and the service refuses it a
// second time by name, which is the pair of opinions every value of this module has.
std::optional<WheelStepUnit> wheelStepUnitFromToken(QStringView token);

// The token for a unit, which is how a record names the one it accepted and how the tests hold this module's
// copy of the list against the schema's. The inverse of the function above for every unit, and `audio-test`
// pins that round trip rather than leaving two spellings of one list.
//
// Defined here and `constexpr` rather than in the `.cpp`, because the check that this list is the schema's is
// a `static_assert` in the test that links both — which means the definition has to be visible to it, and a
// constant expression is what a compile-time check is made of. `std::string_view` of a literal is a view of
// static storage, so it points at something that outlives every caller, and it is comparable in a constant
// expression — which `QLatin1StringView` is not, its comparisons being ordinary functions.
constexpr std::string_view wheelStepUnitToken(WheelStepUnit unit)
{
    return unit == WheelStepUnit::Decibel ? std::string_view("decibel") : std::string_view("percent");
}

// One wheel notch, as the linear factor to write to the daemon. `unit` picks the distance: `Percent` steps
// `stepPercent` points of the percentage and writes the factor that percentage is, which is what this module
// did before the unit existed and what keeps a bar in the desktop's unit agreeing with every other volume
// control on the machine; `Decibel` multiplies the factor by `10^(direction * stepDecibels / 20)`, which is a
// fixed gain in the domain the daemon holds the value in. Both ends are clamped to the ceiling a percentage of
// `maxPercent` is, because a notch past the end of the range is a notch that is already there — the wheel
// reads an above-unity volume and never writes one.
//
// **From silence the two differ, and the difference is the unit's own.** A factor of zero is what the daemon
// really reports for a sink turned all the way down, and a ratio from zero does not exist: a louder notch
// therefore moves to the quietest level this shell writes — one percent, which is `-120 dB` — where a
// percentage notch would move by its step to five. A quieter notch from silence stays there, because
// minus infinity has no bottom. Both are the honest reading of "one notch from nothing" rather than a level
// invented to be audible.
//
// A direction of 0, a step of no size, and a factor that is negative or NaN (a daemon not telling us a volume)
// all leave the factor where it is rather than moving it to one end, which is what an unchecked
// multiplication would do.
double linearAfterWheelStep(double linear, int direction, WheelStepUnit unit, int stepPercent,
                            double stepDecibels, int maxPercent);

// The sink name inside the `default` metadata's `default.audio.sink` value, which is JSON: on this
// session that value is `{"name":"alsa_output.usb-Creative_...-00.analog-stereo-output"}` with the type
// `Spa:String:JSON`. `nullopt` when the value is not that shape. Parsed here rather than taken
// wholesale because the metadata's *other* keys use the same object form with different fields, and a
// name read out of `{"foo":1}` would be an invented device.
std::optional<QString> sinkNameFromMetadata(QStringView value);

// A `SPA_PARAM_Props` pod that sets every channel to `linearVolume` and the mute state, held together
// with the buffer it is written into. The pod points into `buffer_`, so the two must not come apart —
// which is why this type owns both rather than returning a bare pointer to a caller's buffer, and why
// the pod is only handed out as `const`.
class PropsWrite {
public:
    // The most channels this will write, and it is the buffer's own capacity rather than a judgement about
    // sinks: 64 four-byte factors and the object's own header fit in `buffer_` with room to spare, and a sink
    // wider than that is one this shell has not been asked to control. Written as a constant so the test that
    // pins both sides of the boundary — a write at the capacity, and one channel past it — names it rather
    // than repeating the number.
    static constexpr int MaxChannels = 64;

    // The buffer the pod is built into, and therefore the bound on what one write can carry. Public for the
    // reason `MaxChannels` is: the engineering spec states it, and `spec-values-test` compares the two
    // spellings of it rather than letting a resized buffer leave the document describing the old one.
    static constexpr std::size_t BufferBytes = 1024;

    PropsWrite(int channels, double linearVolume, bool muted);

    // The param to hand to `pw_node_set_param(..., SPA_PARAM_Props, ...)`, or null when the write was
    // refused — a sink with more channels than `buffer_` holds, or a builder that could not fit the pod.
    // Null is the refusal and it is the caller's to honour: writing nothing is the honest answer, and the
    // service checks every one of its three writes before it touches the daemon. `audio-test` covers both
    // sides — a write at the buffer's capacity produces a pod, and one channel past it produces none.
    const spa_pod* pod() const { return pod_; }

private:
    // A Props object with one array property is a few dozen bytes; 1 KiB is far above anything this can
    // produce even for a sink with 32 channels, and fixed so the pod cannot dangle if a
    // heap-allocating buffer ever moved.
    std::array<std::byte, BufferBytes> buffer_{};
    const spa_pod* pod_ = nullptr;
};

}  // namespace quantum::audio
