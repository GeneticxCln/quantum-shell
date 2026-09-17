// The configuration as QML reads it: the schema's rules, the diffing that decides what a change emits,
// and a binding evaluated in a real QML engine with no display.
//
// The schema is tested by handing it text, which is the whole reason it is a pure function of the file:
// every rule below is a case in a string rather than a file on disk. The watcher — the part that has to
// talk to a filesystem — is a separate binary, `config-watcher-test`.
#include "config/Config.h"
#include "config/ConfigSchema.h"

// The two services are included for one reason each: the sampling interval is written down twice — once as
// the configuration file's default and floor, once as the numbers `SysMonService` refuses outside of — and
// the volume step twice the same way, against the constants `PipeWireService` applies. This is the only
// translation unit both halves of each pair are visible in, and they are compared below at compile time,
// which is the same guard the key names and the QML type names are held to.
#include "audio/PipeWireService.h"
#include "system/SysMonService.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>

using quantum::config::BarConfig;
using quantum::config::Config;
using quantum::config::ConfigAudio;
using quantum::config::ConfigBar;
using quantum::config::ConfigSystem;
using quantum::config::ConfigValues;
using quantum::config::parseConfig;
using quantum::config::ParseResult;

namespace {

// The shell's shipped values, written out here independently of the schema and compared with it below.
// The duplication is the point: these are what the documents and the live layer-shell test quote, so a
// default that moves has to move in all three places at once rather than only in the code.
constexpr int coveredDefaultHeight = 32;
constexpr auto coveredDefaultNamespace = "quantum-shell-bar";

// The sampling interval's shipped default and its floor, mirrored independently of both headers and then
// compared with each of them below.
constexpr int coveredDefaultSampleIntervalMs = 2000;
constexpr int coveredMinSampleIntervalMs = 10;

// The system readout's shipped content and memory form, mirrored the same way: `[bar.system]`'s defaults as
// the documents state them.
constexpr bool coveredDefaultShowCpu = true;
constexpr bool coveredDefaultShowMemory = true;
constexpr auto coveredDefaultMemoryFormat = "used_of_total";

// The audio readout's shipped settings, mirrored independently of the schema and of the service that applies
// them, and compared with both below. The step's floor is here for the same reason the interval's is: it is
// refused in two places and quoted in the documents, so a third place that disagreed would be a lie about
// what a file may say.
constexpr bool coveredDefaultShowVolume = true;
constexpr auto coveredDefaultVolumeScale = "percent";
constexpr int coveredDefaultVolumeStepPercent = 5;
constexpr int coveredMinVolumeStepPercent = 1;
// The second step, and its floor is the resolution of the readout it is measured against rather than a
// judgement about hearing: the bar draws decibels to one decimal place, so a shorter step is one nobody could
// see. That is the same rule the percentage floor is, which is one point of a readout drawn as whole points.
constexpr double coveredDefaultVolumeStepDecibels = 1.0;
constexpr double coveredMinVolumeStepDecibels = 0.1;

// The two headers against each other: `ConfigSchema.h` refuses a file that asks for something below the
// kernel's tick and tells the user what `SysMonService` will accept, so the two must not be able to disagree.
static_assert(quantum::config::DefaultSampleIntervalMs ==
                  quantum::system::SysMonService::DefaultSampleIntervalMs,
              "the sampling interval's default moved in one of the two headers; they have to move together");
static_assert(quantum::config::MinSampleIntervalMs ==
                  quantum::system::SysMonService::MinimumSampleIntervalMs,
              "the sampling interval's floor moved in one of the two headers; they have to move together");

// And both against what the documents tell a person the default and the floor are, so the three cannot
// drift apart silently.
static_assert(quantum::config::DefaultSampleIntervalMs == coveredDefaultSampleIntervalMs,
              "the sampling interval's default changed: update the mirror above, and every document naming it");
static_assert(quantum::config::MinSampleIntervalMs == coveredMinSampleIntervalMs,
              "the sampling interval's floor changed: update the mirror above, and every document naming it");

// The same two guards for the volume step, against the service that applies it. A file that asks for a step
// of zero is refused by both spellings, and the default in the file is the step the shell uses when the file
// says nothing, so the two must be one number written twice with a guard rather than two numbers that happen
// to agree today.
static_assert(quantum::config::DefaultVolumeStepPercent ==
                  quantum::audio::PipeWireService::DefaultStepPercent,
              "the volume step's default moved in one of the two headers; they have to move together");
static_assert(quantum::config::MinVolumeStepPercent == quantum::audio::PipeWireService::MinimumStepPercent,
              "the volume step's floor moved in one of the two headers; they have to move together");

// And all three against what the documents tell a person they are, so the code and the prose cannot drift
// apart either.
static_assert(quantum::config::DefaultVolumeStepPercent == coveredDefaultVolumeStepPercent,
              "the volume step's default changed: update the mirror above, and every document naming it");
static_assert(quantum::config::MinVolumeStepPercent == coveredMinVolumeStepPercent,
              "the volume step's floor changed: update the mirror above, and every document naming it");

// And the same pair for the step in the other unit, against the service that applies it and against the
// documents — the same three-way guard, because this one is refused in two places and quoted in the same
// documents, and a second step that nothing held would be the one place a file could ask for something the
// service quietly ignores.
static_assert(quantum::config::DefaultVolumeStepDecibels ==
                  quantum::audio::PipeWireService::DefaultStepDecibels,
              "the decibel step's default moved in one of the two headers; they have to move together");
static_assert(quantum::config::MinVolumeStepDecibels ==
                  quantum::audio::PipeWireService::MinimumStepDecibels,
              "the decibel step's floor moved in one of the two headers; they have to move together");
static_assert(quantum::config::DefaultVolumeStepDecibels == coveredDefaultVolumeStepDecibels,
              "the decibel step's default changed: update the mirror above, and every document naming it");
static_assert(quantum::config::MinVolumeStepDecibels == coveredMinVolumeStepDecibels,
              "the decibel step's floor changed: update the mirror above, and every document naming it");

// The key paths the shell offers to `qsctl config get`, mirrored the same way and for a sharper reason: a
// path is what a script types. It is interface, so a rename has to be acknowledged here — and in the
// document that lists the keys — rather than being free to happen in the schema alone.
constexpr std::array<const char*, 17> coveredKeyPaths{"bar.height",
                                                       "bar.layerNamespace",
                                                       "bar.system.sample_interval_ms",
                                                       "bar.system.show_cpu",
                                                       "bar.system.show_memory",
                                                       "bar.system.memory_format",
                                                       "bar.audio.show_volume",
                                                       "bar.audio.volume_scale",
                                                       "bar.audio.step_percent",
                                                       "bar.audio.step_decibels",
                                                       "bar.network.show_status",
                                                       "bar.network.show_name",
                                                       "bar.network.show_strength",
                                                       "bar.battery.show_status",
                                                       "bar.battery.show_percentage",
                                                       "bar.battery.show_time",
                                                       "bar.media.show_media"};

template <std::size_t Declared, std::size_t Covered>
constexpr bool samePaths(const std::array<const char*, Declared>& declared,
                         const std::array<const char*, Covered>& covered) {
    if (Declared != Covered) {
        return false;
    }
    for (std::size_t index = 0; index < Declared; ++index) {
        if (std::string_view(declared[index]) != std::string_view(covered[index])) {
            return false;
        }
    }
    return true;
}

static_assert(samePaths(quantum::config::KeyPaths, coveredKeyPaths),
              "the configuration key paths changed: update the mirror above, and every document that names them");

// The memory forms the schema accepts, mirrored the same way and for a sharper reason: each token is written
// in a file by a user and switched on by name in `qml/SystemMonitor.qml`, so a token renamed here without the
// widget being changed would leave a form that validates, resolves and then draws the fallback. Holding this
// list to the widget's own four is what makes that a build failure rather than a picture nobody looks at.
constexpr std::array<const char*, 4> coveredMemoryFormats{"used_of_total", "used", "available", "percent"};

static_assert(samePaths(quantum::config::MemoryFormats, coveredMemoryFormats),
              "the memory formats changed: update the mirror above, `qml/SystemMonitor.qml`, and every document "
              "that names them");

// The units the volume readout can be drawn in, mirrored for the same reason the memory forms are: each is
// written in a file by a user and switched on by name in `qml/Volume.qml`, so a token renamed on one side and
// not the other would validate, resolve, and then draw whichever unit the widget's default branch draws.
constexpr std::array<const char*, 2> coveredVolumeScales{"percent", "decibel"};

static_assert(samePaths(quantum::config::VolumeScales, coveredVolumeScales),
              "the volume scales changed: update the mirror above, `qml/Volume.qml`, and every document that "
              "names them");


bool mentions(const QStringList& messages, const QString& key) {
    for (const QString& message : messages) {
        if (message.contains(key))
            return true;
    }
    return false;
}

}  // namespace

class ConfigTest : public QObject {
    Q_OBJECT

private slots:
    void defaultsAreWhatTheShellShipsWithoutAFile();
    void readsEveryKeyTheBarUses();
    void warnsAboutEveryUnknownKeyByName();
    void aValueOfTheWrongTypeIsReportedAndTheDefaultKept();
    void refusesANamespaceOutsideTheFrozenPrefix();
    void refusesAHeightTheBarCannotHave();
    void refusesASamplingIntervalItCannotSampleAt();
    void refusesASystemSettingItCannotHonour();
    void refusesAnAudioSettingItCannotHonour();
    void refusesABatterySettingItCannotHonour();
    void refusesAFileFromASchemaVersionItDoesNotKnow();
    void warnsWhenTheSchemaVersionIsMissingButStillUsesTheFile();
    void refusesAFileThatIsNotToml();
    void onlyThePropertyThatChangedEmits();
    void everyPropertyIsOneABindingNeeds();
    void qmlReadsTheSameValuesAndFollowsAChange();
    void everyKeyPathTheSchemaOffersResolvesAndNothingElseDoes();
};

void ConfigTest::defaultsAreWhatTheShellShipsWithoutAFile() {
    // A file that says nothing — empty, or holding only comments — is a valid TOML document with no keys
    // in it, so every value falls back to its default and the only thing to report is the missing
    // `schema_version`. A file that is not there at all is a different case and not one this function
    // sees: the watcher turns that into the defaults without a word, which is where `aMissingFileIsNotAProblem`
    // checks it.
    for (const QByteArray& text : {QByteArray(""), QByteArray("# only a comment\n")}) {
        const ParseResult result = parseConfig(text);
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join(QStringLiteral("; "))));
        QCOMPARE(result.warnings.size(), 1);
        QVERIFY(mentions(result.warnings, QStringLiteral("schema_version")));
        QCOMPARE(result.values.bar.height, coveredDefaultHeight);
        QCOMPARE(result.values.bar.layerNamespace, QString::fromLatin1(coveredDefaultNamespace));
        QCOMPARE(result.values.bar.system.sampleIntervalMs, coveredDefaultSampleIntervalMs);
        // Both readouts are drawn by default: a shell that hid half of what it measures until someone edited
        // a file would be shipping a configuration nobody asked for.
        QCOMPARE(result.values.bar.system.showCpu, coveredDefaultShowCpu);
        QCOMPARE(result.values.bar.system.showMemory, coveredDefaultShowMemory);
        QCOMPARE(result.values.bar.system.memoryFormat, QString::fromLatin1(coveredDefaultMemoryFormat));
        // The volume readout is drawn and steps by the shipped amount, for the same reason the two system
        // readouts are drawn: a shell that hid a readout until someone edited a file would be shipping a
        // configuration nobody asked for.
        QCOMPARE(result.values.bar.audio.showVolume, coveredDefaultShowVolume);
        // The desktop's convention rather than the shell's opinion, which is why it is the default: a bar that
        // showed decibels until someone edited a file would be the only volume control on the desktop reading a
        // different unit than the rest of them.
        QCOMPARE(result.values.bar.audio.volumeScale, QString::fromLatin1(coveredDefaultVolumeScale));
        QCOMPARE(result.values.bar.audio.stepPercent, coveredDefaultVolumeStepPercent);
        // One decibel, which is a mixer's own coarse step: a shell that had no decibel step until someone
        // edited a file would be a shell whose second unit is drawn with a wheel that does nothing.
        QCOMPARE(result.values.bar.audio.stepDecibels, coveredDefaultVolumeStepDecibels);
    }

    // And the objects the shell actually starts with hold those same values, rather than leaving them to
    // the schema: `Config` is what a binding reads before any file has been parsed.
    Config config;
    QCOMPARE(config.bar()->height(), coveredDefaultHeight);
    QCOMPARE(config.bar()->layerNamespace(), QString::fromLatin1(coveredDefaultNamespace));
    QCOMPARE(config.bar()->system()->sampleIntervalMs(), coveredDefaultSampleIntervalMs);
    QCOMPARE(config.bar()->system()->showCpu(), coveredDefaultShowCpu);
    QCOMPARE(config.bar()->system()->showMemory(), coveredDefaultShowMemory);
    QCOMPARE(config.bar()->system()->memoryFormat(), QString::fromLatin1(coveredDefaultMemoryFormat));
    QCOMPARE(config.bar()->audio()->volumeScale(), QString::fromLatin1(coveredDefaultVolumeScale));
    QCOMPARE(config.bar()->audio()->stepDecibels(), coveredDefaultVolumeStepDecibels);
}

void ConfigTest::readsEveryKeyTheBarUses() {
    const ParseResult result = parseConfig(QByteArray(
        "schema_version = 1\n"
        "\n"
        "[bar]\n"
        "height = 44\n"
        "namespace = \"quantum-shell-bar-alt\"\n"
        "\n"
        "[bar.system]\n"
        "sample_interval_ms = 1500\n"
        "show_cpu = false\n"
        "show_memory = true\n"
        "memory_format = \"percent\"\n"
        "\n"
        "[bar.audio]\n"
        "show_volume = false\n"
        "volume_scale = \"decibel\"\n"
        "step_percent = 12\n"
        "step_decibels = 2.5\n"
        "\n"
        "[bar.battery]\n"
        "show_status = true\n"
        "show_percentage = false\n"
        "show_time = true\n"));

    QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join(QStringLiteral("; "))));
    QVERIFY2(result.warnings.isEmpty(), qPrintable(result.warnings.join(QStringLiteral("; "))));
    QCOMPARE(result.values.bar.height, 44);
    QCOMPARE(result.values.bar.layerNamespace, QStringLiteral("quantum-shell-bar-alt"));
    // The file's snake_case keys land in the camelCase fields the widget is bound to, which is the one
    // spelling change in the schema and the one a test has to pin.
    QCOMPARE(result.values.bar.system.sampleIntervalMs, 1500);
    QCOMPARE(result.values.bar.system.showCpu, false);
    QCOMPARE(result.values.bar.system.showMemory, true);
    QCOMPARE(result.values.bar.system.memoryFormat, QStringLiteral("percent"));
    QCOMPARE(result.values.bar.audio.showVolume, false);
    QCOMPARE(result.values.bar.audio.volumeScale, QStringLiteral("decibel"));
    QCOMPARE(result.values.bar.audio.stepPercent, 12);
    QCOMPARE(result.values.bar.audio.stepDecibels, 2.5);
    // The fourth table, whose keys are the same snake_case shape one level deeper. It is read in the same pass
    // as the three above, so a table that arrived without being dispatched would show here as three defaults.
    QCOMPARE(result.values.bar.battery.showStatus, true);
    QCOMPARE(result.values.bar.battery.showPercentage, false);
    QCOMPARE(result.values.bar.battery.showTime, true);
}

void ConfigTest::refusesABatterySettingItCannotHonour() {
    // The fourth table follows the same rule as the three above it: a value of the wrong type is reported by its
    // own path and the default stands. `show_time = 1` is the case `as_boolean` exists for — toml++'s
    // `value<bool>()` coerces an integer to a boolean, so a file saying `1` would set the flag with nothing said
    // about it — and `show_time = "no"` is the plainer mistake.
    for (const QByteArray& text : {QByteArrayLiteral("schema_version = 1\n[bar.battery]\nshow_time = 1\n"),
                                   QByteArrayLiteral("schema_version = 1\n[bar.battery]\nshow_time = \"no\"\n")}) {
        const ParseResult result = parseConfig(text);
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join(QStringLiteral("; "))));
        QCOMPARE(result.warnings.size(), 1);
        QVERIFY(mentions(result.warnings, QStringLiteral("bar.battery.show_time")));
        QVERIFY(mentions(result.warnings, QStringLiteral("a boolean")));
        QVERIFY(mentions(result.warnings, QStringLiteral("true")));
        QCOMPARE(result.values.bar.battery.showTime, true);
    }

    // A key inside the table this build does not read is named with the whole list, which is what tells a user
    // which spelling to look for — and the keys beside it are read whatever it says, which is the half that
    // keeps one typo from discarding a whole table.
    const ParseResult unknownKey = parseConfig(QByteArrayLiteral(
        "schema_version = 1\n[bar.battery]\nshow_level = false\nshow_percentage = false\n"));
    QVERIFY(unknownKey.errors.isEmpty());
    QCOMPARE(unknownKey.warnings.size(), 1);
    QVERIFY(mentions(unknownKey.warnings, QStringLiteral("bar.battery.show_level")));
    QVERIFY(mentions(unknownKey.warnings, QStringLiteral("show_status")));
    QVERIFY(mentions(unknownKey.warnings, QStringLiteral("show_percentage")));
    QCOMPARE(unknownKey.values.bar.battery.showPercentage, false);
    QCOMPARE(unknownKey.values.bar.battery.showStatus, true);

    // And a number where the table itself belongs is reported at the table's own path, because `bar.battery` is
    // the line in the file to go and look at.
    const ParseResult notATable = parseConfig(QByteArrayLiteral("schema_version = 1\n[bar]\nbattery = 3\n"));
    QVERIFY(notATable.errors.isEmpty());
    QCOMPARE(notATable.warnings.size(), 1);
    QVERIFY(mentions(notATable.warnings, QStringLiteral("bar.battery")));
    QVERIFY(mentions(notATable.warnings, QStringLiteral("a table")));
    QCOMPARE(notATable.values.bar.battery.showStatus, true);
    QCOMPARE(notATable.values.bar.battery.showPercentage, true);
    QCOMPARE(notATable.values.bar.battery.showTime, true);
}

void ConfigTest::refusesAnAudioSettingItCannotHonour() {
    // `show_volume = 1` is the case `as_boolean` exists for: toml++'s `value<bool>()` coerces an integer to a
    // boolean, so a file saying `1` would set the flag with nothing said about it — the quiet acceptance this
    // schema refuses everywhere else. `show_volume = "no"` is the plainer mistake, and both are reported with
    // the value that was kept.
    const ParseResult flags = parseConfig(QByteArray("schema_version = 1\n"
                                                     "[bar.audio]\n"
                                                     "show_volume = \"no\"\n"));
    QVERIFY(flags.errors.isEmpty());
    QCOMPARE(flags.warnings.size(), 1);
    QVERIFY(mentions(flags.warnings, QStringLiteral("bar.audio.show_volume")));
    QVERIFY(mentions(flags.warnings, QStringLiteral("a boolean")));
    QCOMPARE(flags.values.bar.audio.showVolume, coveredDefaultShowVolume);

    const ParseResult coerced =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.audio]\nshow_volume = 1\n"));
    QCOMPARE(coerced.warnings.size(), 1);
    QVERIFY(mentions(coerced.warnings, QStringLiteral("bar.audio.show_volume")));
    QCOMPARE(coerced.values.bar.audio.showVolume, coveredDefaultShowVolume);

    // A flag turned off is a value like any other.
    const ParseResult off =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.audio]\nshow_volume = false\n"));
    QVERIFY2(off.warnings.isEmpty(), qPrintable(off.warnings.join(QStringLiteral("; "))));
    QVERIFY(!off.values.bar.audio.showVolume);

    // The unit the readout is drawn in: one of the two the shell can draw and nothing else, refused with the
    // list named — the widget switches on the token, so one it has no branch for would be a configuration
    // value that does nothing, which is the failure this schema exists to report instead. `decibels` is the
    // near miss worth naming: it is the word a person reaches for, and the token is the singular.
    const ParseResult unknownScale = parseConfig(
        QByteArrayLiteral("schema_version = 1\n[bar.audio]\nvolume_scale = \"decibels\"\n"));
    QVERIFY(unknownScale.errors.isEmpty());
    QCOMPARE(unknownScale.warnings.size(), 1);
    QVERIFY(mentions(unknownScale.warnings, QStringLiteral("bar.audio.volume_scale")));
    QVERIFY(mentions(unknownScale.warnings, QStringLiteral("decibels")));
    QVERIFY(mentions(unknownScale.warnings, QStringLiteral("percent")));
    QCOMPARE(unknownScale.values.bar.audio.volumeScale, QString::fromLatin1(coveredDefaultVolumeScale));

    const ParseResult wrongScaleType =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.audio]\nvolume_scale = 2\n"));
    QCOMPARE(wrongScaleType.warnings.size(), 1);
    QVERIFY(mentions(wrongScaleType.warnings, QStringLiteral("a string")));
    QCOMPARE(wrongScaleType.values.bar.audio.volumeScale, QString::fromLatin1(coveredDefaultVolumeScale));

    // And every unit the schema accepts is accepted, which is what makes the refusal above about the list
    // rather than about one spelling.
    for (const char* token : quantum::config::VolumeScales) {
        const QByteArray text = QByteArrayLiteral("schema_version = 1\n[bar.audio]\nvolume_scale = \"")
            + QByteArray(token) + "\"\n";
        const ParseResult result = parseConfig(text);
        QVERIFY2(result.warnings.isEmpty(),
                 qPrintable(QStringLiteral("%1: %2")
                                .arg(QString::fromLatin1(token),
                                     result.warnings.join(QStringLiteral("; ")))));
        QCOMPARE(result.values.bar.audio.volumeScale, QString::fromLatin1(token));
    }

    // A key inside the table this build does not read is named with the whole list, which grew with this
    // change — the list is what tells a user which spelling to look for. `volume_scales` is the plural a
    // person types after reading the token above.
    const ParseResult unknownKey =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.audio]\nvolume_scales = \"percent\"\n"));
    QCOMPARE(unknownKey.warnings.size(), 1);
    QVERIFY(mentions(unknownKey.warnings, QStringLiteral("bar.audio.volume_scales")));
    QVERIFY(mentions(unknownKey.warnings, QStringLiteral("volume_scale")));
    QVERIFY(mentions(unknownKey.warnings, QStringLiteral("step_percent")));
    QVERIFY(mentions(unknownKey.warnings, QStringLiteral("step_decibels")));

    // The step: a zero is a wheel that does nothing at all, refused by name and by the number that was kept,
    // and a negative one is a wheel that turns backwards. Neither is a small step, which is why they are
    // refused rather than clamped — a person who wrote 0 meant something this shell does not do.
    for (const char* text : {"step_percent = 0\n", "step_percent = -5\n"}) {
        const ParseResult step = parseConfig(
            QByteArray("schema_version = 1\n[bar.audio]\n") + QByteArray(text));
        QVERIFY(step.errors.isEmpty());
        QCOMPARE(step.warnings.size(), 1);
        QVERIFY(mentions(step.warnings, QStringLiteral("bar.audio.step_percent")));
        QVERIFY(mentions(step.warnings, QStringLiteral("a wheel that does nothing")));
        QVERIFY(mentions(step.warnings, QString::number(coveredDefaultVolumeStepPercent)));
        QCOMPARE(step.values.bar.audio.stepPercent, coveredDefaultVolumeStepPercent);
    }

    // The number that would not fit, refused before the narrowing rather than wrapping into a negative step.
    const ParseResult tooLarge = parseConfig(
        QByteArrayLiteral("schema_version = 1\n[bar.audio]\nstep_percent = 99999999999\n"));
    QCOMPARE(tooLarge.warnings.size(), 1);
    QCOMPARE(tooLarge.values.bar.audio.stepPercent, coveredDefaultVolumeStepPercent);

    const ParseResult wrongType =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.audio]\nstep_percent = \"5\"\n"));
    QCOMPARE(wrongType.warnings.size(), 1);
    QVERIFY(mentions(wrongType.warnings, QStringLiteral("an integer")));
    QCOMPARE(wrongType.values.bar.audio.stepPercent, coveredDefaultVolumeStepPercent);

    // There is no ceiling on the step, and that is asserted rather than left to be inferred: a step larger
    // than the range is a person asking for one notch to reach the end of it, and the service clamps the
    // result instead of refusing the wish.
    const ParseResult large =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.audio]\nstep_percent = 1000\n"));
    QVERIFY2(large.warnings.isEmpty(), qPrintable(large.warnings.join(QStringLiteral("; "))));
    QCOMPARE(large.values.bar.audio.stepPercent, 1000);

    // The step in the other unit, whose refusals are its own: a step shorter than the readout can draw is a
    // notch that changes nothing on screen, refused by name and by the floor that was kept — the rule the
    // percentage step follows, applied to the unit that draws tenths. A zero and a negative step are the same
    // refusal, because neither is a small step, and a string is the plainer mistake.
    for (const char* text : {"step_decibels = 0\n", "step_decibels = -1.5\n", "step_decibels = 0.05\n"}) {
        const ParseResult step =
            parseConfig(QByteArray("schema_version = 1\n[bar.audio]\n") + QByteArray(text));
        QVERIFY(step.errors.isEmpty());
        QCOMPARE(step.warnings.size(), 1);
        QVERIFY(mentions(step.warnings, QStringLiteral("bar.audio.step_decibels")));
        QVERIFY(mentions(step.warnings, QString::number(coveredMinVolumeStepDecibels)));
        QCOMPARE(step.values.bar.audio.stepDecibels, coveredDefaultVolumeStepDecibels);
    }

    const ParseResult wrongStepType =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.audio]\nstep_decibels = \"1\"\n"));
    QCOMPARE(wrongStepType.warnings.size(), 1);
    QVERIFY(mentions(wrongStepType.warnings, QStringLiteral("a number")));
    QCOMPARE(wrongStepType.values.bar.audio.stepDecibels, coveredDefaultVolumeStepDecibels);

    // Both of a number's spellings are numbers — `2` and `2.0` are the same step — and the floor itself is a
    // step rather than a refusal: it is the shortest one the readout can show, not the shortest worth having.
    const ParseResult integerSpelling =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.audio]\nstep_decibels = 2\n"));
    QVERIFY2(integerSpelling.warnings.isEmpty(),
             qPrintable(integerSpelling.warnings.join(QStringLiteral("; "))));
    QCOMPARE(integerSpelling.values.bar.audio.stepDecibels, 2.0);

    const ParseResult atTheFloor =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.audio]\nstep_decibels = 0.1\n"));
    QVERIFY2(atTheFloor.warnings.isEmpty(), qPrintable(atTheFloor.warnings.join(QStringLiteral("; "))));
    QCOMPARE(atTheFloor.values.bar.audio.stepDecibels, coveredMinVolumeStepDecibels);

    // And no ceiling, for the reason the percentage step has none: a step larger than the range is a person
    // asking for one notch to reach the end of it, which the arithmetic clamps.
    const ParseResult noCeiling =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.audio]\nstep_decibels = 1000.0\n"));
    QVERIFY2(noCeiling.warnings.isEmpty(), qPrintable(noCeiling.warnings.join(QStringLiteral("; "))));
    QCOMPARE(noCeiling.values.bar.audio.stepDecibels, 1000.0);

    // A key inside the table this build does not read is named, with the list, and a value where the table
    // belongs is reported as the wrong shape rather than silently ignored.
    const ParseResult unknown = parseConfig(QByteArray("schema_version = 1\n"
                                                       "[bar.audio]\n"
                                                       "output = \"speakers\"\n"));
    QCOMPARE(unknown.warnings.size(), 1);
    QVERIFY(mentions(unknown.warnings, QStringLiteral("bar.audio.output")));
    QVERIFY(mentions(unknown.warnings, QStringLiteral("show_volume")));

    const ParseResult notATable =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar]\naudio = 4\n"));
    QCOMPARE(notATable.warnings.size(), 1);
    QVERIFY(mentions(notATable.warnings, QStringLiteral("bar.audio")));
    QVERIFY(mentions(notATable.warnings, QStringLiteral("a table")));
    QCOMPARE(notATable.values.bar.audio.stepPercent, coveredDefaultVolumeStepPercent);
    QCOMPARE(notATable.values.bar.audio.stepDecibels, coveredDefaultVolumeStepDecibels);
}

void ConfigTest::warnsAboutEveryUnknownKeyByName() {
    const ParseResult result = parseConfig(QByteArray(
        "schema_version = 1\n"
        "transparency = 0.5\n"
        "\n"
        "[bar]\n"
        "height = 40\n"
        "widht = 3\n"));

    QVERIFY(result.errors.isEmpty());
    // Each unknown key is named by the path a user would search the file for, and each is a warning
    // rather than an error: the keys the shell does read were usable and have been applied.
    QCOMPARE(result.warnings.size(), 2);
    QVERIFY(mentions(result.warnings, QStringLiteral("transparency")));
    QVERIFY(mentions(result.warnings, QStringLiteral("bar.widht")));
    QCOMPARE(result.values.bar.height, 40);
}

void ConfigTest::aValueOfTheWrongTypeIsReportedAndTheDefaultKept() {
    const ParseResult result = parseConfig(QByteArray(
        "schema_version = 1\n"
        "\n"
        "[bar]\n"
        "height = \"44\"\n"
        "namespace = 7\n"));

    QVERIFY(result.errors.isEmpty());
    QCOMPARE(result.warnings.size(), 2);
    QVERIFY(mentions(result.warnings, QStringLiteral("bar.height")));
    QVERIFY(mentions(result.warnings, QStringLiteral("an integer")));
    QVERIFY(mentions(result.warnings, QStringLiteral("bar.namespace")));
    QVERIFY(mentions(result.warnings, QStringLiteral("a string")));

    // Not coerced. `height = "44"` is a mistake, and reading it as 44 would hide it.
    QCOMPARE(result.values.bar.height, coveredDefaultHeight);
    QCOMPARE(result.values.bar.layerNamespace, QString::fromLatin1(coveredDefaultNamespace));

    // The nested table follows the same rule one level down: a string where the number belongs, a key this
    // build does not read, and a number where the table itself belongs are each reported by their own full
    // path, because `bar.system` is the line in the file to go and look at.
    const ParseResult nested = parseConfig(QByteArray(
        "schema_version = 1\n"
        "[bar.system]\n"
        "sample_interval_ms = \"1000\"\n"
        "sampl_interval_ms = 1000\n"));
    QVERIFY(nested.errors.isEmpty());
    QCOMPARE(nested.warnings.size(), 2);
    QVERIFY(mentions(nested.warnings, QStringLiteral("bar.system.sample_interval_ms")));
    QVERIFY(mentions(nested.warnings, QStringLiteral("an integer")));
    QVERIFY(mentions(nested.warnings, QStringLiteral("bar.system.sampl_interval_ms")));
    QCOMPARE(nested.values.bar.system.sampleIntervalMs, coveredDefaultSampleIntervalMs);

    const ParseResult notATable = parseConfig(QByteArrayLiteral("schema_version = 1\n[bar]\nsystem = 3\n"));
    QVERIFY(notATable.errors.isEmpty());
    QCOMPARE(notATable.warnings.size(), 1);
    QVERIFY(mentions(notATable.warnings, QStringLiteral("bar.system")));
    QVERIFY(mentions(notATable.warnings, QStringLiteral("a table")));
    QCOMPARE(notATable.values.bar.system.sampleIntervalMs, coveredDefaultSampleIntervalMs);
}

void ConfigTest::refusesANamespaceOutsideTheFrozenPrefix() {
    // AGENTS.md freezes the shell's layer-shell namespaces as `quantum-shell-*`. The integration refuses
    // a foreign name before it reaches the protocol; this is the earlier refusal, and the one that can
    // name the key that has to be edited.
    const ParseResult result = parseConfig(QByteArray(
        "schema_version = 1\n"
        "\n"
        "[bar]\n"
        "namespace = \"noctalia-bar\"\n"));

    QVERIFY(result.errors.isEmpty());
    QCOMPARE(result.warnings.size(), 1);
    QVERIFY(mentions(result.warnings, QStringLiteral("bar.namespace")));
    QVERIFY(mentions(result.warnings, QString::fromLatin1(coveredDefaultNamespace)));
    QCOMPARE(result.values.bar.layerNamespace, QString::fromLatin1(coveredDefaultNamespace));
}

void ConfigTest::refusesAHeightTheBarCannotHave() {
    // The logical height must be positive and representable by the int property QML reads. A TOML
    // integer is wider: validating only the lower bound lets a positive input wrap during narrowing.
    const qint64 largestHeight = std::numeric_limits<int>::max();
    for (const qint64 height : {qint64{0}, qint64{-3}, largestHeight + 1,
                               std::numeric_limits<qint64>::max()}) {
        const ParseResult result = parseConfig(QByteArray("schema_version = 1\n[bar]\nheight = ")
                                               + QByteArray::number(height) + '\n');
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.warnings.size(), 1);
        QVERIFY(mentions(result.warnings, QStringLiteral("bar.height")));
        QCOMPARE(result.values.bar.height, coveredDefaultHeight);
    }

    // The representable upper boundary is still accepted; the guard must not reject it by one.
    const ParseResult boundary = parseConfig(QByteArray("schema_version = 1\n[bar]\nheight = ")
                                             + QByteArray::number(largestHeight) + '\n');
    QVERIFY(boundary.errors.isEmpty());
    QVERIFY(boundary.warnings.isEmpty());
    QCOMPARE(boundary.values.bar.height, std::numeric_limits<int>::max());
}

void ConfigTest::refusesASamplingIntervalItCannotSampleAt() {
    // Below the kernel's tick. `/proc/stat` counts CPU time in `USER_HZ`, which is 100 Hz, so two readings
    // less than 10 ms apart can find no tick between them and a percentage over no interval is not a
    // reading — `SysMonService` refuses these same values for that reason, and the guard at the top of this
    // file is what keeps the two floors equal. A negative interval is the worse shape of the same problem:
    // it is not "very fast", it is a timer that fires continuously.
    for (const int interval : {0, -2000, coveredMinSampleIntervalMs - 1}) {
        const ParseResult result =
            parseConfig(QByteArray("schema_version = 1\n[bar.system]\nsample_interval_ms = ")
                        + QByteArray::number(interval) + '\n');
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join(QStringLiteral("; "))));
        QCOMPARE(result.warnings.size(), 1);
        QVERIFY(mentions(result.warnings, QStringLiteral("bar.system.sample_interval_ms")));
        // The value that was kept is named, and it is the default rather than the refused number.
        QCOMPARE(result.values.bar.system.sampleIntervalMs, coveredDefaultSampleIntervalMs);
    }

    // The floor itself is a value a person may ask for, and the widest interval an `int` holds is too: the
    // guard must reject neither of them by one.
    const ParseResult atFloor =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.system]\nsample_interval_ms = 10\n"));
    QVERIFY2(atFloor.warnings.isEmpty(), qPrintable(atFloor.warnings.join(QStringLiteral("; "))));
    QCOMPARE(atFloor.values.bar.system.sampleIntervalMs, coveredMinSampleIntervalMs);

    const qint64 widest = std::numeric_limits<int>::max();
    const ParseResult atCeiling = parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.system]\nsample_interval_ms = ")
                                             + QByteArray::number(widest) + '\n');
    QVERIFY2(atCeiling.warnings.isEmpty(), qPrintable(atCeiling.warnings.join(QStringLiteral("; "))));
    QCOMPARE(atCeiling.values.bar.system.sampleIntervalMs, static_cast<int>(widest));

    // One past it is not an interval the property can hold: narrowed to `int` it would be negative, so it is
    // refused rather than wrapped into a timer that fires immediately.
    const ParseResult tooLarge = parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.system]\nsample_interval_ms = ")
                                            + QByteArray::number(widest + 1) + '\n');
    QVERIFY(mentions(tooLarge.warnings, QStringLiteral("bar.system.sample_interval_ms")));
    QCOMPARE(tooLarge.values.bar.system.sampleIntervalMs, coveredDefaultSampleIntervalMs);
}

void ConfigTest::refusesASystemSettingItCannotHonour() {
    // Whether a readout is drawn is a boolean and nothing else: `show_cpu = "no"` is a mistake, and reading
    // it as true would hide the mistake on screen rather than report it. Each flag is reported by its own
    // path, and the warning names the value that was kept.
    const ParseResult flags = parseConfig(QByteArray(
        "schema_version = 1\n"
        "[bar.system]\n"
        "show_cpu = \"no\"\n"
        "show_memory = 1\n"));
    QVERIFY(flags.errors.isEmpty());
    QCOMPARE(flags.warnings.size(), 2);
    QVERIFY(mentions(flags.warnings, QStringLiteral("bar.system.show_cpu")));
    QVERIFY(mentions(flags.warnings, QStringLiteral("bar.system.show_memory")));
    QVERIFY(mentions(flags.warnings, QStringLiteral("a boolean")));
    QCOMPARE(flags.values.bar.system.showCpu, coveredDefaultShowCpu);
    QCOMPARE(flags.values.bar.system.showMemory, coveredDefaultShowMemory);

    // A flag turned off is a value like any other, and what it was set to is what the tree holds.
    const ParseResult off = parseConfig(
        QByteArrayLiteral("schema_version = 1\n[bar.system]\nshow_cpu = false\nshow_memory = false\n"));
    QVERIFY2(off.warnings.isEmpty(), qPrintable(off.warnings.join(QStringLiteral("; "))));
    QVERIFY(!off.values.bar.system.showCpu);
    QVERIFY(!off.values.bar.system.showMemory);

    // The memory form is one of the tokens the shell can draw and nothing else, refused with the list named:
    // the widget has no branch for anything outside it, so a token it silently ignored would be a
    // configuration value that does nothing — which is the failure this file exists to report instead.
    const ParseResult unknownForm = parseConfig(
        QByteArrayLiteral("schema_version = 1\n[bar.system]\nmemory_format = \"gigabytes\"\n"));
    QVERIFY(unknownForm.errors.isEmpty());
    QCOMPARE(unknownForm.warnings.size(), 1);
    QVERIFY(mentions(unknownForm.warnings, QStringLiteral("bar.system.memory_format")));
    QVERIFY(mentions(unknownForm.warnings, QStringLiteral("gigabytes")));
    QVERIFY(mentions(unknownForm.warnings, QStringLiteral("used_of_total")));
    QCOMPARE(unknownForm.values.bar.system.memoryFormat, QString::fromLatin1(coveredDefaultMemoryFormat));

    const ParseResult wrongType =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.system]\nmemory_format = 4\n"));
    QCOMPARE(wrongType.warnings.size(), 1);
    QVERIFY(mentions(wrongType.warnings, QStringLiteral("a string")));
    QCOMPARE(wrongType.values.bar.system.memoryFormat, QString::fromLatin1(coveredDefaultMemoryFormat));

    // And every token the schema accepts is accepted, which is what makes the refusal above about the list
    // rather than about one spelling — the same shape as the sampling interval's boundaries.
    for (const char* token : quantum::config::MemoryFormats) {
        const QByteArray text = QByteArrayLiteral("schema_version = 1\n[bar.system]\nmemory_format = \"")
            + QByteArray(token) + "\"\n";
        const ParseResult result = parseConfig(text);
        QVERIFY2(result.warnings.isEmpty(),
                 qPrintable(QStringLiteral("%1: %2")
                                .arg(QString::fromLatin1(token),
                                     result.warnings.join(QStringLiteral("; ")))));
        QCOMPARE(result.values.bar.system.memoryFormat, QString::fromLatin1(token));
    }

    // A key inside the table this build does not read is named with the whole list, which grew with this
    // change. The list is what tells a user which spelling to look for.
    const ParseResult unknownKey =
        parseConfig(QByteArrayLiteral("schema_version = 1\n[bar.system]\nmemory_formats = \"used\"\n"));
    QCOMPARE(unknownKey.warnings.size(), 1);
    QVERIFY(mentions(unknownKey.warnings, QStringLiteral("bar.system.memory_formats")));
    QVERIFY(mentions(unknownKey.warnings, QStringLiteral("memory_format")));
    QVERIFY(mentions(unknownKey.warnings, QStringLiteral("show_cpu")));
}

void ConfigTest::refusesAFileFromASchemaVersionItDoesNotKnow() {
    // A file written for a newer shell is refused as a whole rather than partly understood: a key that
    // has changed meaning is a value this build would be inventing, and this build's defaults are at
    // least a configuration somebody reasoned about.
    const ParseResult result = parseConfig(QByteArray(
        "schema_version = 2\n"
        "[bar]\n"
        "height = 44\n"));

    QVERIFY(result.warnings.isEmpty());
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(mentions(result.errors, QStringLiteral("schema_version")));
    QVERIFY(mentions(result.errors, QStringLiteral("not applied")));
    QCOMPARE(result.values.bar.height, coveredDefaultHeight);

    // The same is true of a version that is not a version: the value is unusable rather than merely
    // unknown, and guessing at it would be worse than refusing the file.
    const ParseResult wrongType = parseConfig(QByteArray("schema_version = \"1\"\n[bar]\nheight = 44\n"));
    QCOMPARE(wrongType.errors.size(), 1);
    QCOMPARE(wrongType.values.bar.height, coveredDefaultHeight);
}

void ConfigTest::warnsWhenTheSchemaVersionIsMissingButStillUsesTheFile() {
    // Missing keys fall back to defaults, and this one has a default like any other — but a file without
    // it is a file written before the field existed, which is exactly the case a migration is written
    // for, so it is worth saying out loud rather than passing over.
    const ParseResult result = parseConfig(QByteArray("[bar]\nheight = 44\n"));

    QVERIFY(result.errors.isEmpty());
    QCOMPARE(result.warnings.size(), 1);
    QVERIFY(mentions(result.warnings, QStringLiteral("schema_version")));
    QCOMPARE(result.values.bar.height, 44);
}

void ConfigTest::refusesAFileThatIsNotToml() {
    const ParseResult result = parseConfig(QByteArray("this is not toml {{{"));

    QVERIFY(result.warnings.isEmpty());
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(mentions(result.errors, QStringLiteral("TOML")));
    QCOMPARE(result.values.bar.height, coveredDefaultHeight);
}

void ConfigTest::onlyThePropertyThatChangedEmits() {
    Config config;
    QSignalSpy heightSpy(config.bar(), &ConfigBar::heightChanged);
    QSignalSpy namespaceSpy(config.bar(), &ConfigBar::layerNamespaceChanged);
    QSignalSpy intervalSpy(config.bar()->system(), &ConfigSystem::sampleIntervalMsChanged);
    QSignalSpy showCpuSpy(config.bar()->system(), &ConfigSystem::showCpuChanged);
    QSignalSpy showMemorySpy(config.bar()->system(), &ConfigSystem::showMemoryChanged);
    QSignalSpy formatSpy(config.bar()->system(), &ConfigSystem::memoryFormatChanged);
    QSignalSpy showVolumeSpy(config.bar()->audio(), &ConfigAudio::showVolumeChanged);
    QSignalSpy scaleSpy(config.bar()->audio(), &ConfigAudio::volumeScaleChanged);
    QSignalSpy stepSpy(config.bar()->audio(), &ConfigAudio::stepPercentChanged);
    QSignalSpy decibelStepSpy(config.bar()->audio(), &ConfigAudio::stepDecibelsChanged);

    // A whole value set applied twice: the first changes one property, the second changes nothing.
    ConfigValues values;
    values.bar.height = 40;
    config.apply(values);
    QCOMPARE(heightSpy.count(), 1);
    QCOMPARE(namespaceSpy.count(), 0);
    QCOMPARE(intervalSpy.count(), 0);
    QCOMPARE(config.bar()->height(), 40);

    config.apply(values);
    QCOMPARE(heightSpy.count(), 1);
    QCOMPARE(namespaceSpy.count(), 0);
    QCOMPARE(intervalSpy.count(), 0);

    // And the other property on its own, to show neither is being carried by the other's signal.
    values.bar.layerNamespace = QStringLiteral("quantum-shell-bar-alt");
    config.apply(values);
    QCOMPARE(heightSpy.count(), 1);
    QCOMPARE(namespaceSpy.count(), 1);
    QCOMPARE(intervalSpy.count(), 0);
    QCOMPARE(config.bar()->layerNamespace(), QStringLiteral("quantum-shell-bar-alt"));

    // The nested table on its own: one edit to one key emits one signal, and the two properties the bar
    // resizes for are not woken by it. This matters more here than elsewhere because the object that
    // connects to this signal re-arms a timer when it fires.
    values.bar.system.sampleIntervalMs = 250;
    config.apply(values);
    QCOMPARE(intervalSpy.count(), 1);
    QCOMPARE(heightSpy.count(), 1);
    QCOMPARE(namespaceSpy.count(), 1);
    QCOMPARE(config.bar()->system()->sampleIntervalMs(), 250);

    // And the same value again changes nothing, so nothing is re-armed.
    config.apply(values);
    QCOMPARE(intervalSpy.count(), 1);

    // The nested object reports the same values the rest of the tree holds, rather than a copy of its own:
    // what `qsctl config get bar.system.sample_interval_ms` resolves is what a binding reads.
    QCOMPARE(config.values().bar.system.sampleIntervalMs, 250);

    // The three settings the widget draws from, each on its own signal and none carried by another: hiding
    // the CPU readout must not re-evaluate the memory readout's form, and changing the form must not resize
    // anything. The service listens to the interval's signal alone, so a cadence is never re-armed by an edit
    // to what is drawn.
    values.bar.system.showCpu = false;
    config.apply(values);
    QCOMPARE(showCpuSpy.count(), 1);
    QCOMPARE(showMemorySpy.count(), 0);
    QCOMPARE(formatSpy.count(), 0);
    QCOMPARE(intervalSpy.count(), 1);
    QVERIFY(!config.bar()->system()->showCpu());

    values.bar.system.memoryFormat = QStringLiteral("percent");
    values.bar.system.showMemory = false;
    config.apply(values);
    QCOMPARE(showCpuSpy.count(), 1);
    QCOMPARE(showMemorySpy.count(), 1);
    QCOMPARE(formatSpy.count(), 1);
    QCOMPARE(config.bar()->system()->memoryFormat(), QStringLiteral("percent"));
    QVERIFY(!config.bar()->system()->showMemory());
    QCOMPARE(heightSpy.count(), 1);
    QCOMPARE(namespaceSpy.count(), 1);

    // Applied again, and nothing at all speaks.
    config.apply(values);
    QCOMPARE(showCpuSpy.count(), 1);
    QCOMPARE(showMemorySpy.count(), 1);
    QCOMPARE(formatSpy.count(), 1);
    QCOMPARE(intervalSpy.count(), 1);
    QCOMPARE(showVolumeSpy.count(), 0);
    QCOMPARE(stepSpy.count(), 0);

    // The audio table, which is a second nested object and therefore a second chance for a signal to be
    // carried by the wrong one. The service listens to the step alone, so an edit to whether the readout is
    // drawn must not re-apply the step — and none of the system table's signals may fire for either.
    values.bar.audio.stepPercent = 10;
    config.apply(values);
    QCOMPARE(stepSpy.count(), 1);
    QCOMPARE(showVolumeSpy.count(), 0);
    QCOMPARE(intervalSpy.count(), 1);
    QCOMPARE(showCpuSpy.count(), 1);
    QCOMPARE(config.bar()->audio()->stepPercent(), 10);
    // The tree and the struct are one value, not two: what `qsctl config get bar.audio.step_percent`
    // resolves is what a binding reads.
    QCOMPARE(config.values().bar.audio.stepPercent, 10);

    values.bar.audio.showVolume = false;
    config.apply(values);
    QCOMPARE(showVolumeSpy.count(), 1);
    QCOMPARE(scaleSpy.count(), 0);
    QCOMPARE(stepSpy.count(), 1);
    QVERIFY(!config.bar()->audio()->showVolume());

    // The third audio setting, and its own signal: the *widget* reads the unit and the *service* reads the
    // step, so an edit to one may not speak on the other's behalf — the service re-arming a wheel that was not
    // touched is the cost of getting this wrong.
    values.bar.audio.volumeScale = QStringLiteral("decibel");
    config.apply(values);
    QCOMPARE(scaleSpy.count(), 1);
    QCOMPARE(showVolumeSpy.count(), 1);
    QCOMPARE(stepSpy.count(), 1);
    // The unit speaks for itself and for nothing else, and this is the signal that matters most now that the
    // unit selects which step applies: the composition root re-sends the unit on it, and a step key waking it
    // would have the running shell re-measuring a wheel that was not touched.
    QCOMPARE(decibelStepSpy.count(), 0);
    QCOMPARE(config.bar()->audio()->volumeScale(), QStringLiteral("decibel"));
    // One value and not two, the same way the step is checked above: what a `qsctl config get` resolves is
    // what a binding reads.
    QCOMPARE(config.values().bar.audio.volumeScale, QStringLiteral("decibel"));

    // The fourth audio setting, on its own signal like the other three: editing the decibel step must not
    // re-send the percentage one, because the two are what apply in the two units and neither is the other's
    // trigger.
    values.bar.audio.stepDecibels = 3.0;
    config.apply(values);
    QCOMPARE(decibelStepSpy.count(), 1);
    QCOMPARE(stepSpy.count(), 1);
    QCOMPARE(scaleSpy.count(), 1);
    QCOMPARE(showVolumeSpy.count(), 1);
    QCOMPARE(config.bar()->audio()->stepDecibels(), 3.0);
    QCOMPARE(config.values().bar.audio.stepDecibels, 3.0);

    config.apply(values);
    QCOMPARE(showVolumeSpy.count(), 1);
    QCOMPARE(scaleSpy.count(), 1);
    QCOMPARE(stepSpy.count(), 1);
    QCOMPARE(decibelStepSpy.count(), 1);
}

void ConfigTest::everyPropertyIsOneABindingNeeds() {
    // The QML-visible property set, written out here rather than read from the classes, so that a
    // property added to `Config` or `ConfigBar` without a decision about what it is for fails this test
    // instead of being noticed by an audit later.
    QStringList expected{QStringLiteral("Config.bar"),
                         QStringLiteral("Config.bar.height"),
                         QStringLiteral("Config.bar.layerNamespace"),
                         QStringLiteral("Config.bar.system"),
                         QStringLiteral("Config.bar.system.sampleIntervalMs"),
                         QStringLiteral("Config.bar.system.showCpu"),
                         QStringLiteral("Config.bar.system.showMemory"),
                         QStringLiteral("Config.bar.system.memoryFormat"),
                         QStringLiteral("Config.bar.audio"),
                         QStringLiteral("Config.bar.audio.showVolume"),
                         QStringLiteral("Config.bar.audio.volumeScale"),
                         QStringLiteral("Config.bar.audio.stepPercent"),
                         QStringLiteral("Config.bar.audio.stepDecibels"),
                         QStringLiteral("Config.bar.network"),
                         QStringLiteral("Config.bar.network.showStatus"),
                         QStringLiteral("Config.bar.network.showName"),
                         QStringLiteral("Config.bar.network.showStrength"),
                         QStringLiteral("Config.bar.battery"),
                         QStringLiteral("Config.bar.battery.showStatus"),
                         QStringLiteral("Config.bar.battery.showPercentage"),
                         QStringLiteral("Config.bar.battery.showTime"),
                         QStringLiteral("Config.bar.media"),
                         QStringLiteral("Config.bar.media.showMedia")};

    Config config;
    const QMetaObject* rootMeta = config.metaObject();
    const QMetaObject* barMeta = config.bar()->metaObject();
    const QMetaObject* systemMeta = config.bar()->system()->metaObject();
    const QMetaObject* audioMeta = config.bar()->audio()->metaObject();
    const QMetaObject* networkMeta = config.bar()->network()->metaObject();
    const QMetaObject* batteryMeta = config.bar()->battery()->metaObject();
    const QMetaObject* mediaMeta = config.bar()->media()->metaObject();

    QStringList actual;
    for (int index = rootMeta->propertyOffset(); index < rootMeta->propertyCount(); ++index) {
        actual.append(QStringLiteral("Config.") + QString::fromLatin1(rootMeta->property(index).name()));
    }
    for (int index = barMeta->propertyOffset(); index < barMeta->propertyCount(); ++index) {
        const QMetaProperty property = barMeta->property(index);
        actual.append(QStringLiteral("Config.bar.") + QString::fromLatin1(property.name()));
        // A property a bar binds to without a NOTIFY signal is one that never updates: the binding is
        // evaluated once and the bar keeps showing the value it was built with. Only the leaves are
        // checked, because `Config.bar` and `Config.bar.system` are deliberately CONSTANT — the objects
        // behind them do not change, the values inside them do.
        if (!property.isConstant())
            QVERIFY2(property.hasNotifySignal(), qPrintable(property.name()));
    }
    for (int index = systemMeta->propertyOffset(); index < systemMeta->propertyCount(); ++index) {
        const QMetaProperty property = systemMeta->property(index);
        actual.append(QStringLiteral("Config.bar.system.") + QString::fromLatin1(property.name()));
        QVERIFY2(property.hasNotifySignal(), qPrintable(property.name()));
    }

    for (int index = audioMeta->propertyOffset(); index < audioMeta->propertyCount(); ++index) {
        const QMetaProperty property = audioMeta->property(index);
        actual.append(QStringLiteral("Config.bar.audio.") + QString::fromLatin1(property.name()));
        QVERIFY2(property.hasNotifySignal(), qPrintable(property.name()));
    }

    for (int index = networkMeta->propertyOffset(); index < networkMeta->propertyCount(); ++index) {
        const QMetaProperty property = networkMeta->property(index);
        actual.append(QStringLiteral("Config.bar.network.") + QString::fromLatin1(property.name()));
        QVERIFY2(property.hasNotifySignal(), qPrintable(property.name()));
    }

    for (int index = batteryMeta->propertyOffset(); index < batteryMeta->propertyCount(); ++index) {
        const QMetaProperty property = batteryMeta->property(index);
        actual.append(QStringLiteral("Config.bar.battery.") + QString::fromLatin1(property.name()));
        QVERIFY2(property.hasNotifySignal(), qPrintable(property.name()));
    }
    for (int index = mediaMeta->propertyOffset(); index < mediaMeta->propertyCount(); ++index) {
        const QMetaProperty property = mediaMeta->property(index);
        actual.append(QStringLiteral("Config.bar.media.") + QString::fromLatin1(property.name()));
        QVERIFY2(property.hasNotifySignal(), qPrintable(property.name()));
    }

    actual.sort();
    expected.sort();
    QCOMPARE(actual, expected);
}

void ConfigTest::qmlReadsTheSameValuesAndFollowsAChange() {
    Config config;
    Config::registerQmlSingleton(config);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    // The two bindings the bar itself makes, written the way `qml/Main.qml` writes them. The import line
    // is spelled out rather than substituted, because it is the interface being checked: a module URI
    // that changes breaks this line exactly as it would break the bar.
    const QString source =
        QStringLiteral("import QtQml\n"
                       "import QuantumShell 1.0\n"
                       "QtObject {\n"
                       "    property int barHeight: Config.bar.height\n"
                       "    property string layerNamespace: Config.bar.layerNamespace\n"
                       "    property int sampleIntervalMs: Config.bar.system.sampleIntervalMs\n"
                       "    property bool showCpu: Config.bar.system.showCpu\n"
                       "    property bool showMemory: Config.bar.system.showMemory\n"
                       "    property string memoryFormat: Config.bar.system.memoryFormat\n"
                       "    property bool showVolume: Config.bar.audio.showVolume\n"
                       "    property string volumeScale: Config.bar.audio.volumeScale\n"
                       "    property int stepPercent: Config.bar.audio.stepPercent\n"
                       "    property real stepDecibels: Config.bar.audio.stepDecibels\n"
                       "    property bool showBattery: Config.bar.battery.showStatus\n"
                       "    property bool showBatteryLevel: Config.bar.battery.showPercentage\n"
                       "    property bool showBatteryTime: Config.bar.battery.showTime\n"
                       "}\n");
    component.setData(source.toUtf8(), QUrl(QStringLiteral("qrc:/test/ConfigBinding.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    std::unique_ptr<QObject> bound(component.create());
    QVERIFY2(bound != nullptr, qPrintable(component.errorString()));

    QCOMPARE(bound->property("barHeight").toInt(), coveredDefaultHeight);
    QCOMPARE(bound->property("layerNamespace").toString(),
             QString::fromLatin1(coveredDefaultNamespace));
    QCOMPARE(bound->property("sampleIntervalMs").toInt(), coveredDefaultSampleIntervalMs);
    QCOMPARE(bound->property("showCpu").toBool(), coveredDefaultShowCpu);
    QCOMPARE(bound->property("showMemory").toBool(), coveredDefaultShowMemory);
    QCOMPARE(bound->property("memoryFormat").toString(), QString::fromLatin1(coveredDefaultMemoryFormat));
    QCOMPARE(bound->property("showVolume").toBool(), coveredDefaultShowVolume);
    QCOMPARE(bound->property("volumeScale").toString(), QString::fromLatin1(coveredDefaultVolumeScale));
    QCOMPARE(bound->property("stepPercent").toInt(), coveredDefaultVolumeStepPercent);
    QCOMPARE(bound->property("stepDecibels").toDouble(), coveredDefaultVolumeStepDecibels);
    QCOMPARE(bound->property("showBattery").toBool(), true);
    QCOMPARE(bound->property("showBatteryLevel").toBool(), true);
    QCOMPARE(bound->property("showBatteryTime").toBool(), true);

    // Now the configuration changes and nothing in the QML is touched: the bindings re-evaluate because
    // the notify signals fired. Both directions are checked — the edited value follows, and the value
    // nobody edited is still what the bar was built with.
    ConfigValues values;
    values.bar.height = 44;
    config.apply(values);

    QCOMPARE(bound->property("barHeight").toInt(), 44);
    QCOMPARE(bound->property("layerNamespace").toString(),
             QString::fromLatin1(coveredDefaultNamespace));

    // And the nested table, through the same binding path a QML file would use: one level deeper is still
    // reached by a binding, and a change to it re-evaluates without the height being re-read as a whole.
    values.bar.system.sampleIntervalMs = 1500;
    config.apply(values);
    QCOMPARE(bound->property("sampleIntervalMs").toInt(), 1500);
    QCOMPARE(bound->property("barHeight").toInt(), 44);

    // The three the widget draws from, through the same path. `memoryFormat` arrives in QML as the token the
    // file wrote, which is what the widget switches on — so this is also where a schema that renamed a token
    // without the widget being changed would show up, as a binding that stopped matching.
    values.bar.system.showCpu = false;
    values.bar.system.showMemory = true;
    values.bar.system.memoryFormat = QStringLiteral("percent");
    config.apply(values);
    QCOMPARE(bound->property("showCpu").toBool(), false);
    QCOMPARE(bound->property("showMemory").toBool(), true);
    QCOMPARE(bound->property("memoryFormat").toString(), QStringLiteral("percent"));

    // And the audio table through the same path: a second nested object is a second chance for a binding to
    // be written against the wrong parent, which is what comparing the property set above cannot see.
    values.bar.audio.showVolume = false;
    values.bar.audio.volumeScale = QStringLiteral("decibel");
    values.bar.audio.stepPercent = 25;
    values.bar.audio.stepDecibels = 2.5;
    config.apply(values);
    QCOMPARE(bound->property("showVolume").toBool(), false);
    QCOMPARE(bound->property("volumeScale").toString(), QStringLiteral("decibel"));
    QCOMPARE(bound->property("stepPercent").toInt(), 25);
    // The second step binds as a real rather than as an integer, which is the one thing about it a binding can
    // get wrong without any value being wrong: a QML `int` property bound to a double would round 2.5 to 3 in
    // the binding and the widget would never say so.
    QCOMPARE(bound->property("stepDecibels").toDouble(), 2.5);
    QCOMPARE(bound->property("memoryFormat").toString(), QStringLiteral("percent"));

    // And the fourth table through the same path: three booleans, each on its own binding, so a table whose
    // object was registered but whose leaves were not reaches QML as three undefined properties.
    values.bar.battery.showStatus = true;
    values.bar.battery.showPercentage = false;
    values.bar.battery.showTime = false;
    config.apply(values);
    QCOMPARE(bound->property("showBattery").toBool(), true);
    QCOMPARE(bound->property("showBatteryLevel").toBool(), false);
    QCOMPARE(bound->property("showBatteryTime").toBool(), false);
}

void ConfigTest::everyKeyPathTheSchemaOffersResolvesAndNothingElseDoes() {
    const ConfigValues defaults;

    // Every path the schema offers resolves, and to the value the shell would use: a path offered to a
    // script that resolved to nothing would be a name that lies about what this build reads.
    for (const char* path : quantum::config::KeyPaths) {
        const QString key = QString::fromLatin1(path);
        const std::optional<QVariant> value = quantum::config::configValueForPath(defaults, key);
        QVERIFY2(value.has_value(), qPrintable(key));
    }

    // Resolved from the values it is given rather than from anything remembered, which is checked by asking
    // twice with a file's values in between.
    const ParseResult parsed = parseConfig(QByteArrayLiteral("schema_version = 1\n"
                                                            "[bar]\n"
                                                            "height = 44\n"
                                                            "namespace = \"quantum-shell-bar-two\"\n"
                                                            "\n"
                                                            "[bar.system]\n"
                                                            "sample_interval_ms = 750\n"
                                                            "show_cpu = false\n"
                                                            "show_memory = true\n"
                                                            "memory_format = \"available\"\n"
                                                            "\n"
                                                            "[bar.audio]\n"
                                                            "volume_scale = \"decibel\"\n"));
    QVERIFY2(parsed.errors.isEmpty(), qPrintable(parsed.errors.join(QStringLiteral("; "))));
    QCOMPARE(quantum::config::configValueForPath(parsed.values, QStringLiteral("bar.height"))->toInt(), 44);
    QCOMPARE(quantum::config::configValueForPath(parsed.values, QStringLiteral("bar.layerNamespace"))
                 ->toString(),
             QStringLiteral("quantum-shell-bar-two"));
    // The file's key path and the type a script receives: a JSON number rather than a string, which is what
    // makes `x=$(qsctl config get bar.system.sample_interval_ms)` usable as a number.
    const QVariant interval =
        *quantum::config::configValueForPath(parsed.values, QStringLiteral("bar.system.sample_interval_ms"));
    QCOMPARE(interval.toInt(), 750);
    QVERIFY2(interval.metaType().id() == QMetaType::Int, qPrintable(interval.metaType().name()));

    // And the other three of the table, each with the type a caller would branch on: a JSON boolean for the
    // two flags and the token itself for the form.
    const QVariant showCpu =
        *quantum::config::configValueForPath(parsed.values, QStringLiteral("bar.system.show_cpu"));
    QCOMPARE(showCpu.toBool(), false);
    QVERIFY2(showCpu.metaType().id() == QMetaType::Bool, qPrintable(showCpu.metaType().name()));
    const QVariant showMemory =
        *quantum::config::configValueForPath(parsed.values, QStringLiteral("bar.system.show_memory"));
    QCOMPARE(showMemory.toBool(), true);
    QVERIFY2(showMemory.metaType().id() == QMetaType::Bool, qPrintable(showMemory.metaType().name()));
    const QVariant format =
        *quantum::config::configValueForPath(parsed.values, QStringLiteral("bar.system.memory_format"));
    QCOMPARE(format.toString(), QStringLiteral("available"));
    QVERIFY2(format.metaType().id() == QMetaType::QString, qPrintable(format.metaType().name()));

    // The audio table's second token, resolved the same way: the unit is a string a person writes and the
    // widget switches on, so what a script reads back is the token rather than a number derived from it.
    const QVariant volumeScale =
        *quantum::config::configValueForPath(parsed.values, QStringLiteral("bar.audio.volume_scale"));
    QCOMPARE(volumeScale.toString(), QStringLiteral("decibel"));
    QVERIFY2(volumeScale.metaType().id() == QMetaType::QString, qPrintable(volumeScale.metaType().name()));

    // And the two steps, which are the shell's two numeric shapes: points are a whole number and a decibel
    // step is a *number* — the first non-integer value this configuration carries — so a script does
    // arithmetic on `x=$(qsctl config get bar.audio.step_decibels)` rather than comparing a string. A key
    // published as the wrong type is a wrong answer for the caller rather than a missing one, which is why
    // the type is asserted and not only the value.
    const QVariant stepPercent =
        *quantum::config::configValueForPath(parsed.values, QStringLiteral("bar.audio.step_percent"));
    QCOMPARE(stepPercent.toInt(), coveredDefaultVolumeStepPercent);
    QVERIFY2(stepPercent.metaType().id() == QMetaType::Int, qPrintable(stepPercent.metaType().name()));
    const QVariant stepDecibels =
        *quantum::config::configValueForPath(parsed.values, QStringLiteral("bar.audio.step_decibels"));
    QCOMPARE(stepDecibels.toDouble(), coveredDefaultVolumeStepDecibels);
    QVERIFY2(stepDecibels.metaType().id() == QMetaType::Double,
             qPrintable(stepDecibels.metaType().name()));

    // And every shape of path that is not one of those keys is nothing, never an empty string or a default:
    // the server turns "nothing" into a refusal naming the path, and a plausible value into a wrong answer.
    // The three near-misses below are the ones a person types: the table without its key, the key without
    // its table, and the key with a unit appended.
    for (const char* path : {"bar", "bar.widht", "bar.height.px", "bar.system", "bar.sample_interval_ms",
                            "bar.system.sample_interval", "bar.system.sample_interval_ms.px", "bar.system.show",
                            "bar.system.show_cpus", "bar.system.memory_formats", "bar.audio.volume_scales",
                            "bar.audio.scale", "bar.audio.step_decibel", "bar.audio.steps", "schema_version",
                            "theme", ""}) {
        QVERIFY2(!quantum::config::configValueForPath(parsed.values, QString::fromLatin1(path)).has_value(), path);
    }
}

QTEST_GUILESS_MAIN(ConfigTest)

#include "config_test.moc"
