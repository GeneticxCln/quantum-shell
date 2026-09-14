// Integration test: the niri IPC client against the compositor actually running this session.
//
// This is the only test in the project that talks to a real niri, so it is the only one that can catch
// the client being wrong about the real protocol rather than wrong about a test double. It is
// registered with ctest when $NIRI_SOCKET names a socket (see CMakeLists.txt); outside a niri session
// there is nothing for it to talk to, and it says so rather than pretending to pass.
#include "niri/NiriIPC.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

using quantum::niri::NiriCapabilities;
using quantum::niri::NiriFeature;
using quantum::niri::NiriIPC;
using quantum::niri::NiriVersion;
using quantum::niri::Reply;
using quantum::niri::availabilityName;
using quantum::niri::classifyProbeReply;
using quantum::niri::featureName;
using quantum::niri::niriSocketPath;
using quantum::niri::probedFeatures;
using quantum::niri::requestName;

namespace {

// What niri 26.04 answers a request it does not know. classifyProbeReply depends on this exact text,
// so a compositor that changes it must fail here rather than silently report a feature as present.
const QLatin1String unknownRequestError{"error parsing request"};

}  // namespace

class NiriLiveTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void reportsTheVersionTheCompositorActuallyRuns();
    void probesEveryReadOnlyRequestAsSupported();
    void distinguishesACompositorErrorFromAnUnknownRequest();
    void reportsAnUnknownRequestAsUnsupportedAndStaysUsable();

private:
    NiriIPC client_;
    NiriVersion version_;
    NiriCapabilities capabilities_;
    QString detectionFailure_;
    bool versionSeen_ = false;
    bool capabilitiesSeen_ = false;
    qint64 detectionMilliseconds_ = 0;
};

void NiriLiveTest::initTestCase() {
    if (niriSocketPath().isEmpty()) {
        QSKIP("$NIRI_SOCKET is not set: this test needs a running niri session");
    }

    QString transportFailure;
    connect(&client_, &NiriIPC::transportError, this,
            [&transportFailure](const QString& reason) { transportFailure = reason; });

    QSignalSpy connected(&client_, &NiriIPC::connected);
    client_.connectToCompositor();
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_VERIFY_WITH_TIMEOUT(connected.count() == 1 || !transportFailure.isEmpty(), 5000);
    QVERIFY2(connected.count() == 1, qPrintable(QStringLiteral("no connection to %1: %2")
                                                   .arg(niriSocketPath(), transportFailure)));

    connect(&client_, &NiriIPC::versionDetected, this, [this](const NiriVersion& reported) {
        versionSeen_ = true;
        version_ = reported;
    });
    connect(&client_, &NiriIPC::capabilitiesDetected, this,
            [this](const NiriCapabilities& known) {
                capabilitiesSeen_ = true;
                capabilities_ = known;
            });
    connect(&client_, &NiriIPC::detectionFailed, this,
            [this](const QString& reason) { detectionFailure_ = reason; });

    QElapsedTimer timer;
    timer.start();
    client_.detect();
    QTRY_VERIFY_WITH_TIMEOUT(capabilitiesSeen_ || !detectionFailure_.isEmpty(), 10000);
    detectionMilliseconds_ = timer.elapsed();

    qInfo("niri %s on %s: detection %lld ms", qPrintable(version_.toString()),
          qPrintable(niriSocketPath()), static_cast<long long>(detectionMilliseconds_));
    qInfo("%s", qPrintable(capabilities_.describe()));
}

void NiriLiveTest::reportsTheVersionTheCompositorActuallyRuns() {
    QVERIFY2(detectionFailure_.isEmpty(), qPrintable(detectionFailure_));
    QVERIFY(versionSeen_);
    QVERIFY(version_.isValid());
    // The version string came from the compositor, so it is also what proves the floor is met.
    QVERIFY2(version_.isSupported(),
             qPrintable(QStringLiteral("this machine runs niri %1, the floor is %2.%3")
                            .arg(version_.toString())
                            .arg(NiriVersion::minimumYear)
                            .arg(NiriVersion::minimumMonth, 2, 10, QLatin1Char('0'))));
    // The reported form round-trips: what the compositor said parses into this version and prints
    // back unchanged, commit suffix and all, so nothing is dropped between the wire and the shell.
    const NiriVersion reparsed = NiriVersion::parse(version_.toString());
    QVERIFY(reparsed.isValid());
    QCOMPARE(reparsed.year(), version_.year());
    QCOMPARE(reparsed.month(), version_.month());
    QCOMPARE(reparsed.commit(), version_.commit());
}

void NiriLiveTest::probesEveryReadOnlyRequestAsSupported() {
    QVERIFY2(detectionFailure_.isEmpty(), qPrintable(detectionFailure_));
    QVERIFY(capabilitiesSeen_);
    QVERIFY(capabilities_.version().isValid());

    for (const NiriFeature feature : probedFeatures()) {
        const QString detail = QStringLiteral("%1 (%2) came back %3")
                                   .arg(featureName(feature), requestName(feature),
                                        availabilityName(capabilities_.availability(feature)));
        QVERIFY2(capabilities_.isSupported(feature), qPrintable(detail));
    }
    QVERIFY(capabilities_.isSupported(NiriFeature::version));
    // niri 26.04 is the release that exposes ext-background-effect, which is also the version floor.
    QVERIFY(capabilities_.isSupported(NiriFeature::compositorBackgroundEffect));
}

void NiriLiveTest::distinguishesACompositorErrorFromAnUnknownRequest() {
    // ReturnError is niri's own request for testing error handling: a real error from a request the
    // compositor understood is not a missing capability.
    int calls = 0;
    Reply reply;
    client_.send(QStringLiteral("ReturnError"), [&calls, &reply](const Reply& received) {
        calls += 1;
        reply = received;
    });
    QTRY_VERIFY_WITH_TIMEOUT(calls == 1, 5000);

    QVERIFY(!reply.isOk());
    QVERIFY(!reply.text.isEmpty());
    QVERIFY(reply.text != unknownRequestError);
    QCOMPARE(availabilityName(classifyProbeReply(QStringLiteral("Version"), reply)),
             QStringLiteral("supported"));
}

void NiriLiveTest::reportsAnUnknownRequestAsUnsupportedAndStaysUsable() {
    // The falsifier for capability detection: a request name that does not exist must come back
    // classified as unsupported, which is how the shell notices a feature this build lacks.
    int calls = 0;
    Reply reply;
    client_.send(QStringLiteral("QuantumShellProbe"), [&calls, &reply](const Reply& received) {
        calls += 1;
        reply = received;
    });
    QTRY_VERIFY_WITH_TIMEOUT(calls == 1, 5000);

    QVERIFY(!reply.isOk());
    QCOMPARE(reply.text, unknownRequestError);
    QCOMPARE(availabilityName(classifyProbeReply(QStringLiteral("Casts"), reply)),
             QStringLiteral("unsupported"));

    // A rejected request does not desynchronise the connection: the next reply still matches.
    int versionCalls = 0;
    Reply versionReply;
    client_.send(QStringLiteral("Version"), [&versionCalls, &versionReply](const Reply& received) {
        versionCalls += 1;
        versionReply = received;
    });
    QTRY_VERIFY_WITH_TIMEOUT(versionCalls == 1, 5000);
    QVERIFY(versionReply.isOk());
    QVERIFY(versionReply.variant(QStringLiteral("Version")).isString());
}

QTEST_GUILESS_MAIN(NiriLiveTest)
#include "niri_live_test.moc"
