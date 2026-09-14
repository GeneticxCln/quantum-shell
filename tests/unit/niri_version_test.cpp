// Unit tests for the niri version parser and the capability model.
//
// The version strings are the shapes a real compositor reports; the one marked "captured" is the
// exact string this project's niri 26.04 answered to Request::Version over IPC.
#include "niri/NiriVersion.h"

#include <QTest>

using quantum::niri::Availability;
using quantum::niri::NiriCapabilities;
using quantum::niri::NiriFeature;
using quantum::niri::NiriVersion;
using quantum::niri::availabilityName;
using quantum::niri::featureName;
using quantum::niri::probedFeatures;
using quantum::niri::requestName;
using quantum::niri::responseVariant;

class NiriVersionTest : public QObject {
    Q_OBJECT

private slots:
    void parsesTheVersionNiriReports();
    void parsesFormsWithoutABuildCommit();
    void rejectsAnythingThatIsNotAYearAndMonth();
    void comparesAcrossYearsAndMonths();
    void decidesTheVersionGatedCapabilityFromTheVersion();
    void leavesUnansweredCapabilitiesUnknown();
    void statesTheRequestNamesTheProtocolUses();
    void probesOnlyReadOnlyRequests();
};

void NiriVersionTest::parsesTheVersionNiriReports() {
    // Captured from the running compositor: {"Ok":{"Version":"26.04 (8ed0da4)"}}.
    const NiriVersion version = NiriVersion::parse(QStringLiteral("26.04 (8ed0da4)"));

    QVERIFY(version.isValid());
    QCOMPARE(version.year(), 26);
    QCOMPARE(version.month(), 4);
    QCOMPARE(version.commit(), QStringLiteral("8ed0da4"));
    QVERIFY(version.isSupported());
    QCOMPARE(version.toString(), QStringLiteral("26.04 (8ed0da4)"));
}

void NiriVersionTest::parsesFormsWithoutABuildCommit() {
    const NiriVersion bare = NiriVersion::parse(QStringLiteral("26.04"));

    QVERIFY(bare.isValid());
    QCOMPARE(bare.commit(), QString());
    QCOMPARE(bare.toString(), QStringLiteral("26.04"));
    QVERIFY(bare.isSupported());

    // Surrounding whitespace is not part of the version.
    const NiriVersion padded = NiriVersion::parse(QStringLiteral("  25.11  "));
    QVERIFY(padded.isValid());
    QCOMPARE(padded.year(), 25);
    QCOMPARE(padded.month(), 11);
    QCOMPARE(padded.toString(), QStringLiteral("25.11"));
    QVERIFY(!padded.isSupported());
}

void NiriVersionTest::rejectsAnythingThatIsNotAYearAndMonth() {
    // A version this shell does not recognise must stay unrecognised: reading it as some other
    // version is how a feature gets enabled on a compositor that cannot provide it.
    const QStringList rejected{
        QString(),
        QStringLiteral("niri 26.04"),
        QStringLiteral("v26.04"),
        QStringLiteral("26"),
        QStringLiteral("26.04 extra"),
        QStringLiteral("2026.04"),
        QStringLiteral("2.6"),
        QStringLiteral("26.13"),  // month 13
        QStringLiteral("26.00"),  // month 0
        QStringLiteral("nightly"),
        QStringLiteral("26.04 (8ed0da4) trailing"),
    };

    for (const QString& raw : rejected) {
        const NiriVersion version = NiriVersion::parse(raw);
        QVERIFY2(!version.isValid(), qPrintable(QStringLiteral("accepted \"%1\"").arg(raw)));
        QVERIFY2(!version.isSupported(), qPrintable(QStringLiteral("supported \"%1\"").arg(raw)));
        QVERIFY2(version.toString().isEmpty(), qPrintable(QStringLiteral("printed \"%1\"").arg(raw)));
        QVERIFY2(!version.atLeast(1, 1), qPrintable(QStringLiteral("ranked \"%1\"").arg(raw)));
    }
}

void NiriVersionTest::comparesAcrossYearsAndMonths() {
    const NiriVersion version = NiriVersion::parse(QStringLiteral("26.04"));

    QVERIFY(version.atLeast(26, 4));
    QVERIFY(version.atLeast(26, 3));
    QVERIFY(version.atLeast(25, 12));
    QVERIFY(!version.atLeast(26, 5));
    QVERIFY(!version.atLeast(27, 1));

    // Months are numbers, not text: 26.10 is after 26.05 even though "10" < "5" as strings.
    const NiriVersion october = NiriVersion::parse(QStringLiteral("26.10 (ae00000)"));
    QVERIFY(october.atLeast(26, 5));
    QVERIFY(october.atLeast(26, 10));
    QVERIFY(!october.atLeast(26, 11));

    // The floor is the version floor in SYSTEM_PROMPT.md, not a value that drifted into the code.
    QCOMPARE(NiriVersion::minimumYear, 26);
    QCOMPARE(NiriVersion::minimumMonth, 4);
    QVERIFY(NiriVersion::parse(QStringLiteral("25.12")).atLeast(22, 1));
    QVERIFY(!NiriVersion::parse(QStringLiteral("25.12")).isSupported());
    QVERIFY(NiriVersion::parse(QStringLiteral("26.04")).isSupported());
    QVERIFY(NiriVersion::parse(QStringLiteral("27.01")).isSupported());
}

void NiriVersionTest::decidesTheVersionGatedCapabilityFromTheVersion() {
    const NiriCapabilities atTheFloor{NiriVersion::parse(QStringLiteral("26.04 (8ed0da4)"))};
    QCOMPARE(availabilityName(atTheFloor.availability(NiriFeature::compositorBackgroundEffect)),
             QStringLiteral("supported"));

    const NiriCapabilities belowTheFloor{NiriVersion::parse(QStringLiteral("25.11 (abc0000)"))};
    QCOMPARE(availabilityName(belowTheFloor.availability(NiriFeature::compositorBackgroundEffect)),
             QStringLiteral("unsupported"));

    // Nothing has reported a version yet, so nothing is decided.
    NiriCapabilities untouched;
    QCOMPARE(availabilityName(untouched.availability(NiriFeature::compositorBackgroundEffect)),
             QStringLiteral("unknown"));
    QVERIFY(!untouched.isSupported(NiriFeature::compositorBackgroundEffect));

    // Reconnecting to a different compositor re-decides what the version governs.
    NiriCapabilities reconnected{NiriVersion::parse(QStringLiteral("26.04"))};
    reconnected.setVersion(NiriVersion::parse(QStringLiteral("25.11")));
    QCOMPARE(availabilityName(reconnected.availability(NiriFeature::compositorBackgroundEffect)),
             QStringLiteral("unsupported"));
    QCOMPARE(reconnected.version().toString(), QStringLiteral("25.11"));
}

void NiriVersionTest::leavesUnansweredCapabilitiesUnknown() {
    NiriCapabilities capabilities{NiriVersion::parse(QStringLiteral("26.04 (8ed0da4)"))};

    // A probe has not run: "unknown" is not "unsupported", which is the difference between "this
    // build cannot do it" and "we have not asked".
    QCOMPARE(availabilityName(capabilities.availability(NiriFeature::workspaces)),
             QStringLiteral("unknown"));
    QVERIFY(!capabilities.isSupported(NiriFeature::workspaces));

    capabilities.recordProbe(NiriFeature::workspaces, Availability::supported);
    QVERIFY(capabilities.isSupported(NiriFeature::workspaces));

    capabilities.recordProbe(NiriFeature::casts, Availability::unsupported);
    QVERIFY(!capabilities.isSupported(NiriFeature::casts));

    const QString described = capabilities.describe();
    QVERIFY(described.contains(QStringLiteral("version 26.04 (8ed0da4)")));
    QVERIFY(described.contains(QStringLiteral("workspaces=supported")));
    QVERIFY(described.contains(QStringLiteral("casts=unsupported")));
    QVERIFY(described.contains(QStringLiteral("windows=unknown")));
}

void NiriVersionTest::statesTheRequestNamesTheProtocolUses() {
    // The spellings niri-ipc v26.04 defines, each answered by a running niri 26.04 during
    // development. A name that drifts here is a request that compositor would reject.
    QCOMPARE(requestName(NiriFeature::version), QStringLiteral("Version"));
    QCOMPARE(requestName(NiriFeature::outputs), QStringLiteral("Outputs"));
    QCOMPARE(requestName(NiriFeature::workspaces), QStringLiteral("Workspaces"));
    QCOMPARE(requestName(NiriFeature::windows), QStringLiteral("Windows"));
    QCOMPARE(requestName(NiriFeature::layers), QStringLiteral("Layers"));
    QCOMPARE(requestName(NiriFeature::keyboardLayouts), QStringLiteral("KeyboardLayouts"));
    QCOMPARE(requestName(NiriFeature::focusedOutput), QStringLiteral("FocusedOutput"));
    QCOMPARE(requestName(NiriFeature::focusedWindow), QStringLiteral("FocusedWindow"));
    QCOMPARE(requestName(NiriFeature::overviewState), QStringLiteral("OverviewState"));
    QCOMPARE(requestName(NiriFeature::casts), QStringLiteral("Casts"));

    QCOMPARE(responseVariant(NiriFeature::overviewState), QStringLiteral("OverviewState"));
    QCOMPARE(responseVariant(NiriFeature::version), QStringLiteral("Version"));
    QCOMPARE(featureName(NiriFeature::focusedWindow), QStringLiteral("focused-window"));

    // A version-gated feature is not a request, so there is no name to send for it.
    QVERIFY(requestName(NiriFeature::compositorBackgroundEffect).isEmpty());
    QVERIFY(responseVariant(NiriFeature::compositorBackgroundEffect).isEmpty());
    QCOMPARE(featureName(NiriFeature::compositorBackgroundEffect),
             QStringLiteral("compositor-background-effect"));
}

void NiriVersionTest::probesOnlyReadOnlyRequests() {
    const QList<NiriFeature> probed = probedFeatures();

    QCOMPARE(probed.size(), 9);
    QVERIFY(!probed.contains(NiriFeature::version));  // detection asks for it first
    QVERIFY(!probed.contains(NiriFeature::compositorBackgroundEffect));  // decided by the version

    // Probing must never cause an effect or take the pointer: anything interactive, blocking or
    // state-changing is excluded on purpose.
    const QStringList forbidden{
        QStringLiteral("Action"),      QStringLiteral("Output"),
        QStringLiteral("PickWindow"),  QStringLiteral("PickColor"),
        QStringLiteral("EventStream"), QStringLiteral("ReturnError"),
    };
    for (const NiriFeature feature : probed) {
        const QString request = requestName(feature);
        QVERIFY2(!request.isEmpty(), qPrintable(featureName(feature)));
        QVERIFY2(!forbidden.contains(request), qPrintable(request));
        QVERIFY2(!responseVariant(feature).isEmpty(), qPrintable(request));
    }
}

QTEST_GUILESS_MAIN(NiriVersionTest)
#include "niri_version_test.moc"
