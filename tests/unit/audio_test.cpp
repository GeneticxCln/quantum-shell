// The arithmetic and the parsing behind the bar's volume readout, with no daemon, no socket and no display.
//
// Everything here is a pure function of bytes, so every shape of input a PipeWire daemon can send is a case
// in this file — including the shapes a healthy daemon never sends, which is the half a live test cannot
// cover. The pods are built here with the library's own builder rather than assembled by hand, because a
// hand-built pod would be a second spelling of the protocol that agrees with this test and nothing else;
// `aWrittenPodIsTheShapeTheReaderExpects` then reads a pod the *service* built, which is what ties the two
// halves of the module together.
//
// What is deliberately *not* here: anything that needs a daemon. The subscription, the writes as they reach
// a real sink, the default-sink switch and the reconnect are `audio-live-test`'s, behind its own opt-in,
// because the only honest way to check them is against a PipeWire daemon — and one this test started itself,
// so that it never reads or changes the volume of whoever is running it.
#include "audio/AudioVolume.h"
#include "audio/PipeWireService.h"

#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/type-info.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <spa/pod/vararg.h>
#include <spa/utils/type.h>

#include <QTest>

#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <vector>

using quantum::audio::decibelsFromLinear;
using quantum::audio::linearAfterWheelStep;
using quantum::audio::linearFromPercent;
using quantum::audio::parseProps;
using quantum::audio::percentFromLinear;
using quantum::audio::PropsWrite;
using quantum::audio::SinkState;
using quantum::audio::sinkNameFromMetadata;
using quantum::audio::steppedPercent;
using quantum::audio::WheelStepUnit;
using quantum::audio::wheelStepUnitFromToken;
using quantum::audio::wheelStepUnitToken;

namespace {

// The QML name this service is registered under, mirrored here rather than read out of the header, for the
// reason `sysmon_test.cpp` mirrors its own: a rename in `src/audio/` does not build until this file agrees,
// and `qml/Volume.qml` is the other place the name is written.
constexpr auto coveredServiceTypeName = "PipeWireService";
static_assert(std::string_view(quantum::audio::PipeWireService::QmlTypeName) ==
                  std::string_view(coveredServiceTypeName),
              "the QML type name changed: update the mirror above, and every QML file that binds to it");

// The default the service applies and the shortest step it accepts, mirrored for the same reason and held
// against the configuration file's copies in `config_test.cpp`. They are written here too because a default
// that moved without this file being read would otherwise only show up as a number in a document.
constexpr int coveredDefaultStepPercent = 5;
constexpr int coveredMinimumStepPercent = 1;
constexpr double coveredDefaultStepDecibels = 1.0;
constexpr double coveredMinimumStepDecibels = 0.1;
static_assert(quantum::audio::PipeWireService::DefaultStepPercent == coveredDefaultStepPercent,
              "the volume step's default changed: update the mirror above and every document naming it");
static_assert(quantum::audio::PipeWireService::MinimumStepPercent == coveredMinimumStepPercent,
              "the volume step's floor changed: update the mirror above and every document naming it");
static_assert(quantum::audio::PipeWireService::DefaultStepDecibels == coveredDefaultStepDecibels,
              "the decibel step's default changed: update the mirror above and every document naming it");
static_assert(quantum::audio::PipeWireService::MinimumStepDecibels == coveredMinimumStepDecibels,
              "the decibel step's floor changed: update the mirror above and every document naming it");

// The tokens a wheel's unit is named by, mirrored here for the reason the QML type name is above: the schema
// validates a file against its own copy of these, the widget switches on them, and this module resolves them.
// The compile-time half of that three-way agreement is in `bar_interaction_test.cpp`, which links both the
// configuration and this library; what is pinned here is this library's own copy.
constexpr std::string_view coveredPercentToken = "percent";
constexpr std::string_view coveredDecibelToken = "decibel";
static_assert(wheelStepUnitToken(WheelStepUnit::Percent) == coveredPercentToken,
              "the percentage token changed: update the mirror above, the schema, and every document naming it");
static_assert(wheelStepUnitToken(WheelStepUnit::Decibel) == coveredDecibelToken,
              "the decibel token changed: update the mirror above, the schema, and every document naming it");

// A buffer to build one pod in, kept alive by the caller for as long as the pod is read — the pod points
// into it, which is the whole reason `PropsWrite` owns its own.
struct PodBuffer {
    std::array<std::byte, 1024> bytes{};

    // A builder over this buffer. SPA's vararg macros (`SPA_POD_BUILDER_INIT`, `spa_pod_builder_add_object`)
    // would write these pods in one statement each, and they are why this file — like `AudioVolume.cpp`, whose
    // comment says the same — did not build under clang: they expand to compound literals and GNU statement
    // expressions, which the CI matrix's clang job refuses with `-Werror` as extensions. The functions below
    // are what those macros expand to, named one step at a time, so the pods are the library's own and the
    // build depends on no extension.
    spa_pod_builder makeBuilder()
    {
        spa_pod_builder builder{};
        builder.data = bytes.data();
        builder.size = static_cast<uint32_t>(bytes.size());
        return builder;
    }

    // A `SPA_PARAM_Props` object carrying `channelVolumes` as floats and a mute flag, which is the shape the
    // daemon sends and therefore the shape the reader is written against.
    spa_pod* props(const std::vector<float>& volumes, bool muted)
    {
        spa_pod_builder builder = makeBuilder();
        spa_pod_frame props{};
        spa_pod_frame array{};
        spa_pod_builder_push_object(&builder, &props, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
        spa_pod_builder_prop(&builder, SPA_PROP_channelVolumes, 0);
        spa_pod_builder_push_array(&builder, &array);
        for (const float volume : volumes)
            spa_pod_builder_float(&builder, volume);
        spa_pod_builder_pop(&builder, &array);
        spa_pod_builder_prop(&builder, SPA_PROP_mute, 0);
        spa_pod_builder_bool(&builder, muted);
        return static_cast<spa_pod*>(spa_pod_builder_pop(&builder, &props));
    }

    // The same object with no `channelVolumes` at all: a Props object that says nothing about volume.
    spa_pod* propsWithoutVolumes(bool muted)
    {
        spa_pod_builder builder = makeBuilder();
        spa_pod_frame props{};
        spa_pod_builder_push_object(&builder, &props, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
        spa_pod_builder_prop(&builder, SPA_PROP_mute, 0);
        spa_pod_builder_bool(&builder, muted);
        return static_cast<spa_pod*>(spa_pod_builder_pop(&builder, &props));
    }

    // A `channelVolumes` that is not an array of floats. The daemon does not send this; the point is that the
    // reader refuses it rather than reading forty integers as forty floats and publishing a volume nothing
    // sent.
    spa_pod* propsWithIntegerVolumes(const std::vector<int32_t>& volumes)
    {
        spa_pod_builder builder = makeBuilder();
        spa_pod_frame props{};
        spa_pod_frame array{};
        spa_pod_builder_push_object(&builder, &props, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
        spa_pod_builder_prop(&builder, SPA_PROP_channelVolumes, 0);
        spa_pod_builder_push_array(&builder, &array);
        // The array's own element type is the point, and a builder writes it from the first element pushed:
        // pushing ints is how this pod comes to carry `SPA_TYPE_Int` rather than `SPA_TYPE_Float`.
        for (const int32_t volume : volumes)
            spa_pod_builder_int(&builder, volume);
        spa_pod_builder_pop(&builder, &array);
        return static_cast<spa_pod*>(spa_pod_builder_pop(&builder, &props));
    }

    // An object of a different type entirely: a Props request answered with something else.
    spa_pod* notProps()
    {
        spa_pod_builder builder = makeBuilder();
        spa_pod_frame props{};
        spa_pod_builder_push_object(&builder, &props, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
        // A Format object rather than a Props one, with a media type in it: the shape a Props request
        // answered with the wrong param would have.

        spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType, 0);
        spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_audio);
        return static_cast<spa_pod*>(spa_pod_builder_pop(&builder, &props));
    }
};

// The `default.audio.sink` value exactly as this session's `pw-metadata -n default` printed it, trimmed to
// one device name. A parse tested only against a string this file made up would be a parse tested against
// itself; this one is the shape a real WirePlumber publishes, with the spaces it publishes.
constexpr auto sessionMetadataValue =
    R"({ "name": "alsa_output.usb-Creative_Technology_Ltd_SB_Omni_Surround_5.1_0000001b-00.analog-stereo-output" })";

}  // namespace

class AudioTest : public QObject {
    Q_OBJECT

private slots:
    void aPropsParamBecomesAReading();
    void aPropsParamWithoutAFloatVolumeIsRefused();
    void thePercentageIsTheCubeRootNotTheLinearFactor();
    void theDecibelsAreTheGainTheFactorRepresents();
    void theLinearFactorComesBackFromThePercentage();
    void steppingClampsAtBothEnds();
    void aStepOfNoDirectionOrNoSizeDoesNothing();
    void theUnitIsResolvedFromItsTokenAndBack();
    void aPercentageNotchIsAGrowingDistanceInDecibels();
    void aDecibelNotchIsAFixedGain();
    void aDecibelNotchFromSilenceLandsOnTheQuietestLevel();
    void theDefaultSinkIsReadFromTheMetadatasObject();
    void anythingThatIsNotThatObjectIsNotASinkName();
    void aWrittenPodIsTheShapeTheReaderExpects();
    void aWriteForMoreChannelsThanItHoldsBuildsNothing();
};

void AudioTest::aPropsParamBecomesAReading()
{
    PodBuffer buffer;
    const std::optional<SinkState> reading = parseProps(buffer.props({0.042872f, 0.042872f}, true));
    QVERIFY2(reading.has_value(), "a volume and a mute flag were not read as a reading");
    QCOMPARE(reading->channels, 2);
    QCOMPARE(reading->muted, true);
    // Compared against the float the pod holds widened to double, not against a rounded decimal: the value
    // is the daemon's and no rounding happens between the pod and the reading.
    QCOMPARE(reading->linearVolume, static_cast<double>(0.042872f));

    // Mute is optional in a Props object, and its absence is the default rather than unknown: a sink that
    // never mentions mute is not muted.
    const std::optional<SinkState> withoutMute = parseProps(buffer.propsWithoutVolumes(false));
    QVERIFY2(!withoutMute.has_value(), "a Props object with no volume was read as a reading");
    PodBuffer other;
    const std::optional<SinkState> mono = parseProps(other.props({0.5f}, false));
    QVERIFY2(mono.has_value(), "a single-channel sink was refused");
    QCOMPARE(mono->channels, 1);
    QCOMPARE(mono->muted, false);
}

void AudioTest::aPropsParamWithoutAFloatVolumeIsRefused()
{
    PodBuffer volumes;
    PodBuffer integers;
    PodBuffer none;
    PodBuffer foreign;
    PodBuffer empty;

    QVERIFY2(!parseProps(nullptr).has_value(), "a null param was read as a reading");
    QVERIFY2(!parseProps(foreign.notProps()).has_value(), "a Format object was read as a volume");
    QVERIFY2(!parseProps(none.propsWithoutVolumes(true)).has_value(),
             "a Props object with no channelVolumes was read as a volume");
    QVERIFY2(!parseProps(empty.props({}, false)).has_value(), "an empty volume array was read as a volume");
    // The one that would otherwise be silently wrong rather than merely absent: the array body of integers is
    // four bytes per element just like a float, so a reader that skipped the element type would report a
    // volume built out of bit patterns.
    QVERIFY2(!parseProps(integers.propsWithIntegerVolumes({1, 1})).has_value(),
             "an integer channelVolumes array was read as a volume");

    // And the refusal is not a fallback: the good pod after the bad ones still reads.
    QVERIFY2(parseProps(volumes.props({0.25f, 0.25f}, false)).has_value(),
             "the reader gave up after a refusal");
}

void AudioTest::thePercentageIsTheCubeRootNotTheLinearFactor()
{
    // The measurement the convention rests on, and the reason this is a test rather than a comment: on this
    // session `wpctl get-volume 70` prints 0.35 while that sink's Props report
    // `channelVolumes: [0.042872, 0.042872]`, and 0.042872 is 0.35³. A reader that published the linear
    // factor would put the bar at 4% where the rest of the desktop says 35%.
    QCOMPARE(percentFromLinear(0.042872), 35);
    QCOMPARE(percentFromLinear(0.125), 50);  // 0.5³, the worked example of the same identity
    QCOMPARE(percentFromLinear(1.0), 100);
    QCOMPARE(percentFromLinear(0.0), 0);
    // Above unity is a real state — PipeWire's own `channelmix.max-volume` defaults to a linear 10.0 — so it is
    // reported rather than clamped: cbrt(10) is 2.154, which is 215%.
    QCOMPARE(percentFromLinear(10.0), 215);
    // A negative factor cannot come from a sink and has no percentage at all; it is 0 rather than a negative
    // reading, which the widget would draw as a number.
    QCOMPARE(percentFromLinear(-1.0), 0);
}

void AudioTest::theDecibelsAreTheGainTheFactorRepresents()
{
    // The second convention, and it is a *different number* for the same reading rather than another spelling
    // of the first. Measured on this session the way the percentage was: for the sink whose Props carry
    // 0.074087, `pactl list sinks` prints `27525 /  42% / -22,61 dB` — the 42% is the cube root the slot above
    // pins, and the -22.61 dB is 20*log10(0.074087), which is -22.605. The third candidate is the one nothing
    // prints: the linear factor itself would be 7.4, which no volume control on the desktop shows a person.
    QCOMPARE(percentFromLinear(0.074087), 42);
    QVERIFY2(std::abs(decibelsFromLinear(0.074087) + 22.605) < 0.001,
             qPrintable(QString::number(decibelsFromLinear(0.074087), 'f', 3)));
    // And the same pair on the sink the project's own percentage example comes from: the factor that is 35% is
    // worth -27.36 dB, which is the number `pactl` prints beside the 35%.
    QCOMPARE(percentFromLinear(0.042872), 35);
    QVERIFY2(std::abs(decibelsFromLinear(0.042872) + 27.357) < 0.001,
             qPrintable(QString::number(decibelsFromLinear(0.042872), 'f', 3)));

    // The two fixed points that make this a conversion rather than a scale of the shell's choosing: unity is
    // zero decibels, which is what a factor of 1.0 means — no attenuation — and halving the amplitude is
    // -6.02 dB, which is the arithmetic a person checking the number by hand would do.
    QCOMPARE(decibelsFromLinear(1.0), 0.0);
    QVERIFY2(std::abs(decibelsFromLinear(0.5) + 6.0206) < 0.0001,
             qPrintable(QString::number(decibelsFromLinear(0.5), 'f', 4)));
    // Above unity is amplification rather than an error, reported like the percentage reports 215%: PipeWire's
    // default `channelmix.max-volume` is a linear 10.0, which is +20 dB.
    QCOMPARE(decibelsFromLinear(10.0), 20.0);

    // Silence is negative infinity rather than a floor, and that is PulseAudio's own choice rather than this
    // shell's: its `pa_sw_volume_snprint_dB` passes the value through `-INFINITY` and formats it, so a zero
    // volume prints as `-inf dB` there. A floor — say -60 dB — would be a volume the sink is not playing at.
    QVERIFY2(std::isinf(decibelsFromLinear(0.0)) && decibelsFromLinear(0.0) < 0.0,
             qPrintable(QString::number(decibelsFromLinear(0.0))));
    // And a factor that is not positive at all is that same answer, for the reason the percentage gives 0 for
    // it: a sink reporting a negative or NaN volume is a daemon not telling us a volume. Infinity is a reading
    // of silence, which a widget draws as one symbol — where a NaN would be a value every drawing tool has to
    // know to avoid.
    QVERIFY(std::isinf(decibelsFromLinear(-1.0)));
    QVERIFY(std::isinf(decibelsFromLinear(std::nan(""))));
}

void AudioTest::theLinearFactorComesBackFromThePercentage()
{
    // The write path's half of the same convention, and the property that makes the two one convention: what
    // is written for a percentage reads back as that percentage.
    QCOMPARE(percentFromLinear(linearFromPercent(35)), 35);
    QCOMPARE(percentFromLinear(linearFromPercent(0)), 0);
    QCOMPARE(percentFromLinear(linearFromPercent(100)), 100);
    QCOMPARE(percentFromLinear(linearFromPercent(7)), 7);
    // A percentage below zero is silence rather than a negative factor.
    QCOMPARE(linearFromPercent(-20), 0.0);
}

void AudioTest::steppingClampsAtBothEnds()
{
    QCOMPARE(steppedPercent(40, 1, 5, 100), 45);
    QCOMPARE(steppedPercent(40, -1, 5, 100), 35);
    // The clamp, and it is the reason there is no ceiling on the configured step: a step larger than the
    // range is a person asking for one notch to reach the end of it.
    QCOMPARE(steppedPercent(98, 1, 5, 100), 100);
    QCOMPARE(steppedPercent(2, -1, 5, 100), 0);
    QCOMPARE(steppedPercent(0, -1, 5, 100), 0);
    QCOMPARE(steppedPercent(100, 1, 5, 100), 100);
    QCOMPARE(steppedPercent(40, -1, 1000, 100), 0);
}

void AudioTest::aStepOfNoDirectionOrNoSizeDoesNothing()
{
    // A direction the widget never sends and a step the schema refuses: both leave the volume alone rather
    // than moving it to one end, which is what an unchecked multiplication would do.
    QCOMPARE(steppedPercent(40, 0, 5, 100), 40);
    QCOMPARE(steppedPercent(40, 1, 0, 100), 40);
    QCOMPARE(steppedPercent(40, -1, 0, 100), 40);
}

void AudioTest::theUnitIsResolvedFromItsTokenAndBack()
{
    // The token is the configuration's spelling and the enum is this module's, so the mapping is where the two
    // meet — and the round trip is what says they are one list rather than two that happen to agree on the
    // spellings somebody happened to test. The schema validates a file against its own copy of the same list
    // and `bar_interaction_test.cpp` compares the two copies at compile time.
    // Compared through `QVERIFY` on the equality rather than with `QCOMPARE`, because the two types involved —
    // an `std::optional` of this module's enum and a `std::string_view` — are ones the failure formatter would
    // otherwise be guessing at, and what matters here is the answer rather than the rendering of it.
    const auto percent = wheelStepUnitFromToken(QStringLiteral("percent"));
    const auto decibel = wheelStepUnitFromToken(QStringLiteral("decibel"));
    QVERIFY2(percent.has_value() && *percent == WheelStepUnit::Percent, "percent was not resolved");
    QVERIFY2(decibel.has_value() && *decibel == WheelStepUnit::Decibel, "decibel was not resolved");
    QVERIFY2(wheelStepUnitToken(*percent) == coveredPercentToken, "the round trip changed the token");
    QVERIFY2(wheelStepUnitToken(*decibel) == coveredDecibelToken, "the round trip changed the token");

    // A spelling this module does not know is refused rather than defaulted, and the near miss is refused too:
    // `decibels` is the word a person reaches for, and the token is the singular. The schema refuses it first;
    // this is the second opinion, which is the shape every value of this module has.
    QVERIFY(!wheelStepUnitFromToken(QStringLiteral("decibels")).has_value());
    QVERIFY(!wheelStepUnitFromToken(QStringLiteral("Percent")).has_value());
    QVERIFY(!wheelStepUnitFromToken(QString()).has_value());
}

void AudioTest::aPercentageNotchIsAGrowingDistanceInDecibels()
{
    // The measurement the second step key exists for, and it is this module's own arithmetic rather than a
    // reading off a machine: a percentage step is `60 * log10((p + step) / p)` decibels, because the percentage
    // is the cube root of the factor. The default five points is 1.28 dB at 99% and 46.69 dB at 1% — so a
    // person reading decibels and moving by percentage points was being moved by a distance that depended on
    // where they already were, by a factor of thirty-six. Both figures are asserted rather than quoted: a
    // comment cannot fail a build when a step or a convention changes under it.
    const auto spanFor = [](int percent, int stepPercent) {
        const double before = decibelsFromLinear(linearFromPercent(percent));
        const double after = decibelsFromLinear(linearFromPercent(percent + stepPercent));
        return after - before;
    };
    QVERIFY2(std::abs(spanFor(99, coveredDefaultStepPercent) - 1.28) < 0.01,
             qPrintable(QString::number(spanFor(99, coveredDefaultStepPercent), 'f', 4)));
    QVERIFY2(std::abs(spanFor(1, coveredDefaultStepPercent) - 46.69) < 0.01,
             qPrintable(QString::number(spanFor(1, coveredDefaultStepPercent), 'f', 4)));
    // And one point, which is the shortest percentage step the schema accepts, still spans 0.26 dB at the top
    // of the range and 18.06 dB at the bottom of it: the growth is the convention's, not the step's size.
    QVERIFY2(std::abs(spanFor(99, coveredMinimumStepPercent) - 0.26) < 0.01,
             qPrintable(QString::number(spanFor(99, coveredMinimumStepPercent), 'f', 4)));
    QVERIFY2(std::abs(spanFor(1, coveredMinimumStepPercent) - 18.06) < 0.01,
             qPrintable(QString::number(spanFor(1, coveredMinimumStepPercent), 'f', 4)));
}

void AudioTest::aDecibelNotchIsAFixedGain()
{
    // The same notch in the unit the gain is measured in, and the claim is the property itself: the factor is
    // multiplied by `10^(dB/20)`, so the *decibels* move by exactly the step wherever the volume was. The
    // factor is the daemon's own example from the slots above, and the assertion is on the difference rather
    // than on the factor, because the difference being the step is the whole point of the setting.
    const double start = 0.074087;
    for (const double step : {0.1, 1.0, 3.0, 6.0}) {
        const double up = linearAfterWheelStep(start, 1, WheelStepUnit::Decibel, coveredDefaultStepPercent,
                                               step, 100);
        const double down = linearAfterWheelStep(start, -1, WheelStepUnit::Decibel, coveredDefaultStepPercent,
                                                 step, 100);
        QVERIFY2(std::abs((decibelsFromLinear(up) - decibelsFromLinear(start)) - step) < 0.0001,
                 qPrintable(QString::number(decibelsFromLinear(up), 'f', 6)));
        QVERIFY2(std::abs((decibelsFromLinear(start) - decibelsFromLinear(down)) - step) < 0.0001,
                 qPrintable(QString::number(decibelsFromLinear(down), 'f', 6)));
    }

    // And it is the linear factor that is written, not a percentage rounded to whole points: a one-decibel step
    // near the top of the range moves the daemon's factor by 1 dB while the *percentage* moves four points,
    // which is the reason a step cannot be both units at once. The same four points written as a percentage
    // step would be 1.34 dB, and the 0.26 dB that one *point* is worth up there is the shortest step the
    // percentage unit has — a quarter of the decibel the person asked for.
    const double high = linearAfterWheelStep(linearFromPercent(95), 1, WheelStepUnit::Decibel,
                                             coveredDefaultStepPercent, 1.0, 100);
    QVERIFY2(std::abs((decibelsFromLinear(high) - decibelsFromLinear(linearFromPercent(95))) - 1.0) < 0.0001,
             qPrintable(QString::number(decibelsFromLinear(high), 'f', 6)));
    QCOMPARE(percentFromLinear(linearFromPercent(95)), 95);
    QCOMPARE(percentFromLinear(high), 99);
    // The same starting point and the same knob, in the other unit: five points take the percentage to full
    // scale, which is 1.34 dB away. Two distances from one gesture is exactly what the two keys are for — the
    // same wheel cannot be a fixed gain and a fixed number of points, and the readout says which one it is.
    QCOMPARE(linearAfterWheelStep(linearFromPercent(95), 1, WheelStepUnit::Percent,
                                  coveredDefaultStepPercent, 1.0, 100),
             linearFromPercent(100));
    QVERIFY2(std::abs((decibelsFromLinear(linearFromPercent(100)) - decibelsFromLinear(linearFromPercent(95)))
                      - 1.34)
                 < 0.01,
             qPrintable(QString::number(decibelsFromLinear(linearFromPercent(100))
                                            - decibelsFromLinear(linearFromPercent(95)),
                                        'f', 4)));

    // Both ends clamp to the ceiling the wheel is willing to write, which is what a percentage of `maxPercent`
    // is: a notch past full scale is a notch that is already there, and the wheel reads an above-unity volume
    // without ever writing one. A step larger than the whole range lands on the ceiling rather than beyond it.
    QCOMPARE(linearAfterWheelStep(0.95, 1, WheelStepUnit::Decibel, 5, 3.0, 100), 1.0);
    QCOMPARE(linearAfterWheelStep(0.95, 1, WheelStepUnit::Decibel, 5, 100.0, 100), 1.0);
    QCOMPARE(linearAfterWheelStep(10.0, 1, WheelStepUnit::Decibel, 5, 1.0, 100), 1.0);
    // And there is no lower clamp to test, which is a property rather than an omission: a quieter notch never
    // reaches silence from a factor that is not already silence, because a ratio does not reach zero. What it
    // does is get very small — a hundred-decibel step from a factor of a ten-thousandth is 1e-9, ten orders of
    // magnitude below the quietest level the percentage unit writes — and that is the honest thing to write to
    // a daemon rather than a silence nobody asked for. Silence is entered from silence, one slot below.
    const double quieter = linearAfterWheelStep(0.0001, -1, WheelStepUnit::Decibel, 5, 100.0, 100);
    QVERIFY2(quieter > 0.0, qPrintable(QString::number(quieter, 'e', 3)));
    QVERIFY2(std::abs((decibelsFromLinear(0.0001) - decibelsFromLinear(quieter)) - 100.0) < 0.0001,
             qPrintable(QString::number(decibelsFromLinear(quieter), 'f', 6)));

    // The percentage unit is the arithmetic this module already had, reached through the same door: a step of
    // five points from 42% is 47% and not a rounded gain, and the clamp still holds.
    QCOMPARE(linearAfterWheelStep(0.074087, 1, WheelStepUnit::Percent, 5, 1.0, 100),
             linearFromPercent(47));
    QCOMPARE(linearAfterWheelStep(0.074087, -1, WheelStepUnit::Percent, 5, 1.0, 100),
             linearFromPercent(37));
    QCOMPARE(linearAfterWheelStep(linearFromPercent(98), 1, WheelStepUnit::Percent, 5, 1.0, 100),
             linearFromPercent(100));
}

void AudioTest::aDecibelNotchFromSilenceLandsOnTheQuietestLevel()
{
    // Zero has no ratio, so the case cannot be arithmetic and has to be a decision: a louder notch from a sink
    // turned all the way down moves to the quietest level this shell writes, which is one percent — the level
    // the module's own `linearFromPercent` bottoms out at, and therefore the only one reachable from nothing.
    // A quieter notch is already as quiet as it gets.
    QCOMPARE(linearAfterWheelStep(0.0, 1, WheelStepUnit::Decibel, 5, 1.0, 100), linearFromPercent(1));
    QCOMPARE(linearAfterWheelStep(0.0, -1, WheelStepUnit::Decibel, 5, 1.0, 100), 0.0);
    // A negative or NaN factor is the same case — a daemon not telling us a volume — and not a NaN a widget
    // would have to draw or a negative factor the daemon would have to interpret.
    QCOMPARE(linearAfterWheelStep(-1.0, 1, WheelStepUnit::Decibel, 5, 1.0, 100), linearFromPercent(1));
    QCOMPARE(linearAfterWheelStep(std::nan(""), 1, WheelStepUnit::Decibel, 5, 1.0, 100),
             linearFromPercent(1));
    QCOMPARE(linearAfterWheelStep(std::nan(""), -1, WheelStepUnit::Decibel, 5, 1.0, 100), 0.0);
    // The percentage unit moves by its step from silence, which is the difference the module's comment states:
    // five points rather than one, because a step of points has a starting point at zero and a gain has none.
    QCOMPARE(linearAfterWheelStep(0.0, 1, WheelStepUnit::Percent, 5, 1.0, 100), linearFromPercent(5));

    // A direction the widget never sends and a step of no size leave the factor where it is in either unit,
    // rather than moving it to one end — what `steppedPercent` already did for the percentage.
    QCOMPARE(linearAfterWheelStep(0.5, 0, WheelStepUnit::Decibel, 5, 1.0, 100), 0.5);
    QCOMPARE(linearAfterWheelStep(0.5, 0, WheelStepUnit::Percent, 5, 1.0, 100), 0.5);
    QCOMPARE(linearAfterWheelStep(0.5, 1, WheelStepUnit::Decibel, 5, 0.0, 100), 0.5);
    QCOMPARE(linearAfterWheelStep(0.5, -1, WheelStepUnit::Decibel, 5, 0.0, 100), 0.5);
    // Including a NaN step, which is not a small step: the floor exists because a step below the readout's own
    // resolution is one nobody can see, and a step that is not a number is not a step at all.
    QCOMPARE(linearAfterWheelStep(0.5, 1, WheelStepUnit::Decibel, 5, std::nan(""), 100), 0.5);
    // And the decibel unit never writes above the ceiling a reading may exceed: an above-unity factor is read
    // at 215%, and one notch from it lands on full scale rather than on 241%.
    QCOMPARE(linearAfterWheelStep(10.0, -1, WheelStepUnit::Decibel, 5, 1.0, 100), 1.0);
}

void AudioTest::theDefaultSinkIsReadFromTheMetadatasObject()
{
    const std::optional<QString> name = sinkNameFromMetadata(QString::fromUtf8(sessionMetadataValue));
    QVERIFY2(name.has_value(), "the metadata value a real WirePlumber publishes was not read");
    QCOMPARE(*name, QStringLiteral("alsa_output.usb-Creative_Technology_Ltd_SB_Omni_Surround_5.1_0000001b-00."
                                   "analog-stereo-output"));

    // The same value without the spaces, because JSON is JSON and the daemon's own writer is not this test's.
    const std::optional<QString> compact =
        sinkNameFromMetadata(QStringLiteral(R"({"name":"bluez_output.AC_12_2F_00_00_00.a2dp-sink"})"));
    QVERIFY2(compact.has_value(), "a compact metadata value was not read");
    QCOMPARE(*compact, QStringLiteral("bluez_output.AC_12_2F_00_00_00.a2dp-sink"));
}

void AudioTest::anythingThatIsNotThatObjectIsNotASinkName()
{
    // Every one of these is a refusal rather than a fallback, and the alternative — taking the value
    // wholesale — would turn the *other* keys of the same metadata object into device names.
    QVERIFY2(!sinkNameFromMetadata(QStringLiteral(R"({"foo":1})")).has_value(),
             "an object with no name was read as a sink name");
    QVERIFY2(!sinkNameFromMetadata(QStringLiteral(R"({"name":7})")).has_value(),
             "a non-string name was read as a sink name");
    QVERIFY2(!sinkNameFromMetadata(QStringLiteral(R"({"name":""})")).has_value(),
             "an empty name was read as a sink name");
    QVERIFY2(!sinkNameFromMetadata(QString()).has_value(), "an empty value was read as a sink name");
    QVERIFY2(!sinkNameFromMetadata(QStringLiteral("not json at all")).has_value(),
             "a bare string was read as a sink name");
    QVERIFY2(!sinkNameFromMetadata(QStringLiteral(R"(["a","b"])")).has_value(),
             "a JSON array was read as a sink name");
    // The value the JSON type name is not: WirePlumber writes `Spa:String:JSON`, and this is what a metadata
    // entry of another type looks like when it reaches this function.
    QVERIFY2(!sinkNameFromMetadata(QStringLiteral("840")).has_value(),
             "a bare number was read as a sink name");
}

void AudioTest::aWrittenPodIsTheShapeTheReaderExpects()
{
    // The round trip that ties the two halves of the module together: the pod this module writes is read
    // back by the same reader that reads the daemon's, so a write that changed shape would fail here rather
    // than arriving at a daemon as a param it does not understand.
    const PropsWrite write(2, 0.125, true);
    QVERIFY2(write.pod() != nullptr, "a two-channel write produced no pod");

    const std::optional<SinkState> readBack = parseProps(write.pod());
    QVERIFY2(readBack.has_value(), "the pod this module writes is not one this module can read");
    QCOMPARE(readBack->channels, 2);
    QCOMPARE(readBack->muted, true);
    QCOMPARE(readBack->linearVolume, static_cast<double>(0.125f));

    // Every channel set, not just the first: a write that named one channel would unbalance a stereo sink.
    const spa_pod_prop* volumes =
        spa_pod_find_prop(write.pod(), nullptr, SPA_PROP_channelVolumes);
    QVERIFY(volumes != nullptr);
    uint32_t count = 0;
    const void* values = spa_pod_get_array(&volumes->value, &count);
    QVERIFY(values != nullptr);
    QCOMPARE(count, 2u);
    QCOMPARE(static_cast<const float*>(values)[0], 0.125f);
    QCOMPARE(static_cast<const float*>(values)[1], 0.125f);

    // And unmuted, because the mute flag is written as the state asked for rather than as a default.
    const PropsWrite unmuted(1, 1.0, false);
    QVERIFY2(unmuted.pod() != nullptr, "a one-channel write produced no pod");
    const std::optional<SinkState> unmutedReadBack = parseProps(unmuted.pod());
    QVERIFY(unmutedReadBack.has_value());
    QCOMPARE(unmutedReadBack->muted, false);
    QCOMPARE(unmutedReadBack->channels, 1);
}

void AudioTest::aWriteForMoreChannelsThanItHoldsBuildsNothing()
{
    // A sink with more channels than the writer holds is refused rather than written in part: writing the
    // first `MaxChannels` of a wider sink would leave the rest at volumes the person did not choose. The
    // number comes from the type rather than being repeated here, so the boundary this checks is the one the
    // writer draws.
    const PropsWrite tooMany(PropsWrite::MaxChannels + 1, 0.5, false);
    QVERIFY2(tooMany.pod() == nullptr, "a write for more channels than the writer holds produced a pod");

    // The boundary on the other side, so the refusal is one channel past the capacity and not at some lower
    // number: a write at the capacity itself is built, and it is still a Props object with one float per
    // channel in it.
    const PropsWrite atCapacity(PropsWrite::MaxChannels, 0.5, false);
    QVERIFY2(atCapacity.pod() != nullptr, "a write at the writer's full capacity produced no pod");
    const std::optional<SinkState> atCapacityReadBack = parseProps(atCapacity.pod());
    QVERIFY2(atCapacityReadBack.has_value(), "a write at capacity is not a Props object the reader accepts");
    QCOMPARE(atCapacityReadBack->channels, PropsWrite::MaxChannels);
}

QTEST_GUILESS_MAIN(AudioTest)

#include "audio_test.moc"
