// Unit tests for the parsing of one output, as niri reports it in its `Outputs` reply.
//
// The values come from NiriProtocolTestData.h: wire-shaped and synthetic. The real ones are read from
// the running compositor by tests/integration/niri_live_stream_test.cpp.
#include "niri/NiriOutput.h"

#include "NiriProtocolTestData.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QTest>

using quantum::niri::NiriLogicalOutput;
using quantum::niri::NiriOutput;
using quantum::niri::NiriOutputMode;
using qstest::logicalObject;
using qstest::modeObject;
using qstest::outputObject;

namespace {

// One enabled output with two modes, a fractional scale and VRR on: the shape a real reply has.
QJsonObject enabledOutput(const QString& name, int currentMode = 1) {
    return outputObject(name, {modeObject(3840, 2160, 59997, true), modeObject(2560, 1440, 119880)},
                        QJsonValue(currentMode), QJsonValue(logicalObject(0, 0, 3072, 1728, 1.25)),
                        QStringLiteral("MO32U"), true, true);
}

}  // namespace

class NiriOutputTest : public QObject {
    Q_OBJECT

private slots:
    void parsesEveryReadingTheShellUses();
    void keepsAFractionalScaleExactly();
    void aDisabledOutputIsPresentButNotEnabled();
    void anIndexPastTheEndOfTheModeListResolvesToNoMode();
    void keepsATransformThisBuildDoesNotKnow();
    void refusesAnObjectWithoutAUsableName();
    void treatsAnOmittedFieldAsAbsentRatherThanGuessed();
    void comparesByValue();
    void describesWhatItHolds();
};

void NiriOutputTest::parsesEveryReadingTheShellUses() {
    const NiriOutput output = NiriOutput::fromJson(enabledOutput(QStringLiteral("DP-3")));

    QVERIFY(output.isValid());
    QCOMPARE(output.name(), QStringLiteral("DP-3"));
    QCOMPARE(output.make(), QStringLiteral("MAKE"));
    QCOMPARE(output.model(), QStringLiteral("MO32U"));
    QCOMPARE(output.modes().size(), 2);
    QVERIFY(output.isVrrSupported());
    QVERIFY(output.isVrrEnabled());
    QVERIFY(output.isEnabled());

    QVERIFY(output.currentModeIndex().has_value());
    QCOMPARE(*output.currentModeIndex(), 1);
    const std::optional<NiriOutputMode> mode = output.currentMode();
    QVERIFY(mode.has_value());
    QCOMPARE(mode->width, quint16{2560});
    QCOMPARE(mode->height, quint16{1440});
    // Millihertz, exactly as niri reports it: nothing rounds this to 120.
    QCOMPARE(mode->refreshRate, quint32{119880});
    QVERIFY(!mode->isPreferred);
    QCOMPARE(output.modes().at(0).isPreferred, true);

    const std::optional<NiriLogicalOutput> logical = output.logical();
    QVERIFY(logical.has_value());
    QCOMPARE(logical->x, 0);
    QCOMPARE(logical->y, 0);
    QCOMPARE(logical->width, quint32{3072});
    QCOMPARE(logical->height, quint32{1728});
    QVERIFY(logical->transformIsKnown());
    QCOMPARE(logical->transform, QStringLiteral("Normal"));

    // The three fields of the wire struct this build does not parse are present in the input and
    // simply ignored, so a reply carrying them parses exactly like one without.
    QCOMPARE(output.name(), QStringLiteral("DP-3"));
    QVERIFY(output.modes().size() == 2);
}

void NiriOutputTest::keepsAFractionalScaleExactly() {
    const NiriOutput output = NiriOutput::fromJson(enabledOutput(QStringLiteral("DP-3")));
    const std::optional<NiriLogicalOutput> logical = output.logical();
    QVERIFY(logical.has_value());
    // 1.25 is the scale on the machine this was verified against. An implementation that assumed an
    // integer would land on 1.0 here and fail.
    QCOMPARE(logical->scale, 1.25);
    QVERIFY(logical->scale != 1.0);

    const NiriOutput half = NiriOutput::fromJson(outputObject(
        QStringLiteral("DP-4"), {}, QJsonValue(0), QJsonValue(logicalObject(0, 0, 2560, 1440, 2.0))));
    QVERIFY(half.logical().has_value());
    QCOMPARE(half.logical()->scale, 2.0);
    QVERIFY(half.logical()->scale != logical->scale);
}

void NiriOutputTest::aDisabledOutputIsPresentButNotEnabled() {
    // niri reports an output that is disabled or unmapped with a null current mode and a null logical
    // output. That is a real state, not a missing output.
    const NiriOutput output = NiriOutput::fromJson(
        outputObject(QStringLiteral("HDMI-A-1"), {}, QJsonValue(), QJsonValue()));

    QVERIFY(output.isValid());
    QCOMPARE(output.name(), QStringLiteral("HDMI-A-1"));
    QVERIFY(!output.isEnabled());
    QVERIFY(!output.logical().has_value());
    QVERIFY(!output.currentModeIndex().has_value());
    QVERIFY(!output.currentMode().has_value());
    QVERIFY(output.modes().isEmpty());
    QVERIFY(!output.isVrrEnabled());
}

void NiriOutputTest::anIndexPastTheEndOfTheModeListResolvesToNoMode() {
    const NiriOutput output =
        NiriOutput::fromJson(outputObject(QStringLiteral("DP-1"), {modeObject(1920, 1080, 60000)},
                                          QJsonValue(9),
                                          QJsonValue(logicalObject(0, 0, 1920, 1080, 1.0))));

    QVERIFY(output.isValid());
    // Kept as reported, so a reply that disagrees with itself is visible rather than corrected.
    QCOMPARE(output.currentModeIndex().value_or(-1), 9);
    // ...and it resolves to no mode, rather than silently to the first or the last one.
    QVERIFY(!output.currentMode().has_value());
}

void NiriOutputTest::keepsATransformThisBuildDoesNotKnow() {
    // niri-ipc v26.04 defines exactly these eight, and only `Normal` could be observed on this
    // machine, so an unrecognised name must stay visible instead of becoming "normal".
    QCOMPARE(NiriOutput::knownTransforms().size(), 8);
    QVERIFY(NiriOutput::knownTransforms().contains(QStringLiteral("Normal")));
    QVERIFY(NiriOutput::knownTransforms().contains(QStringLiteral("Flipped270")));

    const NiriOutput output = NiriOutput::fromJson(outputObject(
        QStringLiteral("DP-5"), {}, QJsonValue(0),
        QJsonValue(logicalObject(0, 0, 1920, 1080, 1.0, QStringLiteral("Diagonal")))));

    QVERIFY(output.logical().has_value());
    QCOMPARE(output.logical()->transform, QStringLiteral("Diagonal"));
    QVERIFY(!output.logical()->transformIsKnown());
    // And the description says so rather than presenting it as a known transform.
    QVERIFY(output.describe().contains(QStringLiteral("unknown transform")));
}

void NiriOutputTest::refusesAnObjectWithoutAUsableName() {
    QVERIFY(!NiriOutput::fromJson(QJsonObject{}).isValid());
    QVERIFY(!NiriOutput::fromJson(
                 QJsonObject{{QStringLiteral("name"), QJsonValue(QString())}})
                 .isValid());
    QVERIFY(!NiriOutput::fromJson(QJsonObject{{QStringLiteral("name"), QJsonValue(3)}}).isValid());

    // A complete output minus its name is still not an output: without the name it cannot be matched
    // against the output names the workspaces carry.
    QJsonObject nameless = enabledOutput(QStringLiteral("DP-3"));
    nameless.remove(QStringLiteral("name"));
    QVERIFY(!NiriOutput::fromJson(nameless).isValid());
}

void NiriOutputTest::treatsAnOmittedFieldAsAbsentRatherThanGuessed() {
    const NiriOutput output =
        NiriOutput::fromJson(QJsonObject{{QStringLiteral("name"), QJsonValue(QStringLiteral("DP-9"))}});

    QVERIFY(output.isValid());
    QCOMPARE(output.name(), QStringLiteral("DP-9"));
    QVERIFY(output.make().isEmpty());
    QVERIFY(output.model().isEmpty());
    QVERIFY(output.modes().isEmpty());
    QVERIFY(!output.isEnabled());
    QVERIFY(!output.isVrrSupported());
    QVERIFY(!output.isVrrEnabled());
}

void NiriOutputTest::comparesByValue() {
    const NiriOutput first = NiriOutput::fromJson(enabledOutput(QStringLiteral("DP-3")));
    const NiriOutput same = NiriOutput::fromJson(enabledOutput(QStringLiteral("DP-3")));
    QVERIFY(first == same);
    QVERIFY(!(first != same));

    // Each of these changes one reading the shell acts on, and each must be a difference: the state
    // model uses this comparison to decide whether to wake its consumers.
    const NiriOutput otherName = NiriOutput::fromJson(enabledOutput(QStringLiteral("DP-4")));
    QVERIFY(first != otherName);

    const NiriOutput otherMode =
        NiriOutput::fromJson(outputObject(QStringLiteral("DP-3"),
                                          {modeObject(3840, 2160, 59997, true)},
                                          QJsonValue(0),
                                          QJsonValue(logicalObject(0, 0, 3072, 1728, 1.25)),
                                          QStringLiteral("MO32U"), true, true));
    QVERIFY(first != otherMode);

    const NiriOutput otherScale = NiriOutput::fromJson(outputObject(
        QStringLiteral("DP-3"), {}, QJsonValue(0),
        QJsonValue(logicalObject(0, 0, 3072, 1728, 1.5)), QStringLiteral("MO32U"), true, true));
    QVERIFY(first != otherScale);

    const NiriOutput noVrr = NiriOutput::fromJson(outputObject(
        QStringLiteral("DP-3"), {modeObject(3840, 2160, 59997, true), modeObject(2560, 1440, 119880)},
        QJsonValue(1), QJsonValue(logicalObject(0, 0, 3072, 1728, 1.25)), QStringLiteral("MO32U"),
        true, false));
    QVERIFY(first != noVrr);
}

void NiriOutputTest::describesWhatItHolds() {
    const NiriOutput output = NiriOutput::fromJson(enabledOutput(QStringLiteral("DP-3")));
    const QString description = output.describe();
    QVERIFY(description.contains(QStringLiteral("DP-3")));
    QVERIFY(description.contains(QStringLiteral("MO32U")));
    // The logical size and the fractional scale, as readings rather than rounded ones.
    QVERIFY(description.contains(QStringLiteral("3072x1728")));
    QVERIFY(description.contains(QStringLiteral("1.25")));
    // Millihertz rendered as hertz.
    QVERIFY(description.contains(QStringLiteral("119.88Hz")));
    QVERIFY(description.contains(QStringLiteral("vrr")));

    const NiriOutput disabled = NiriOutput::fromJson(
        outputObject(QStringLiteral("HDMI-A-1"), {}, QJsonValue(), QJsonValue()));
    QVERIFY(disabled.describe().contains(QStringLiteral("disabled")));

    QVERIFY(NiriOutput{}.describe().contains(QStringLiteral("invalid")));
}

QTEST_GUILESS_MAIN(NiriOutputTest)
#include "niri_output_test.moc"
