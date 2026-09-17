// The audio module against a PipeWire daemon the test starts, owns and stops.
//
// Nothing here talks to the session's PipeWire. The daemon is a second instance with its own core name, so
// its socket is a different path that a shell or a mixer pointed at the real desktop can never reach, and
// no volume this test changes is anyone's but a null sink's. That is the same reason `niri-live-session-
// test` starts a nested compositor, and it is what makes this test safe to run while the person who started
// it is listening to something.
//
// **Why the test's changes come from other programs.** Everything the shell is asked whether it noticed is
// done by `pw-cli` and `pw-metadata` and read back by `pw-dump` — three separate programs from the same
// package, each an independent client of the daemon. A test that changed the volume through the shell's own
// write path and then asked the shell what the volume was would be checking that a value round-trips
// through one implementation, which is the failure mode the rules call a test agreeing with itself. Here
// the writer, the reader and the code under test are three things.
//
// **The daemon's config is the distribution's own**, copied into a scratch directory and patched: the core
// name is changed so the socket cannot collide with the session's, and the two null sinks and the `default`
// metadata object are appended to `context.objects`. A hand-written minimal config was tried first and its
// clients saw no globals at all — the daemon started, created its socket and answered `pw-cli` with an empty
// registry — so rather than debug PipeWire's own configuration inside this repository, this test runs the
// daemon the way a session does. If the stock config is not where this distribution keeps it, the test says
// so by name instead of failing somewhere downstream.
//
// It is opt-in, behind `QS_AUDIO_TESTS`, because it starts a daemon: a plain test run should not leave a
// second audio daemon on the machine of whoever ran it.
#include "audio/AudioVolume.h"
#include "audio/PipeWireService.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <optional>

using quantum::audio::linearFromPercent;
using quantum::audio::PipeWireService;
using quantum::audio::SinkState;
using quantum::audio::WheelStepUnit;

namespace {

// The token the composition root hands the service for a unit, spelled by the module that resolves it rather
// than written out here: this binary links the audio library and nothing else, and the agreement between this
// spelling and the schema's own token list is a compile-time check in `bar-interaction-test`, which links
// both. A literal here would be a third copy of a word that has to stay one word.
QString tokenFor(WheelStepUnit unit)
{
    const std::string_view text = quantum::audio::wheelStepUnitToken(unit);
    return QString::fromLatin1(text.data(), static_cast<qsizetype>(text.size()));
}

}  // namespace

namespace {

// Where the distribution keeps the daemon's own configuration. Read from the package rather than shipped
// here, so the test runs the daemon the way this machine's session does.
constexpr auto stockConfigDirectory = "/usr/share/pipewire";

// The two sinks the daemon is started with, and the name the daemon's core is given. The sinks are the
// adapter around `support.null-audio-sink`, which is a real PipeWire node with a real Props param and no
// hardware behind it: it accepts a volume and a mute from any client, which is exactly what a sink does.
constexpr auto firstSink = "qs-audio-test-sink-one";
constexpr auto secondSink = "qs-audio-test-sink-two";

// How long a wait for the shell to notice something is given. Generous: the assertion is about *whether* an
// event arrives, not about how fast, and a machine building this tree in parallel is a slow machine. It is
// also the cost of a *failing* case and therefore of a falsification run, which is why it is seconds and not
// a minute: eight cases that each wait it out is the difference between a run someone reads and one they
// abandon.
constexpr int eventTimeoutMs = 8000;

struct CommandResult {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;

    bool ok() const { return exitCode == 0; }
};

CommandResult run(const QString& program, const QStringList& args)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.start();
    CommandResult result;
    if (!process.waitForStarted(5000)) {
        result.err = "could not start " + program.toUtf8();
        return result;
    }
    if (!process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(2000);
        result.err = program.toUtf8() + " did not finish";
        return result;
    }
    result.exitCode = process.exitCode();
    result.out = process.readAllStandardOutput();
    result.err = process.readAllStandardError();
    return result;
}

}  // namespace

class AudioLiveTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void theDaemonThisTestStartedIsTheOneTheShellReads();
    void anExternalVolumeChangeArrivesWithoutTheShellAsking();
    void anExternalMuteArrivesTheSameWay();
    void theShellsOwnWriteReachesTheDaemon();
    void theWheelStepIsTheConfiguredStepAndClampsAtFullScale();
    void aNotchInDecibelsMovesTheDaemonByThatGain();
    void aStepWhileMutedChangesTheVolumeAndLeavesItMuted();
    void switchingTheDefaultSinkMovesTheReading();
    void theDaemonGoingAwayWithdrawsTheReadingAndComingBackRestoresIt();

private:
    bool startDaemon();
    void stopDaemon();
    // The daemon's own state, read by a fresh `pw-dump`: the test's reading of the daemon, taken by a
    // different program than the one under test.
    std::optional<SinkState> readSink(const QString& nodeName);
    // A volume and mute written by `pw-cli`, as another client on the desktop would.
    bool writeSink(const QString& nodeName, double linearVolume, bool muted);
    bool setDefaultSink(const QString& nodeName);
    quint32 nodeId(const QString& nodeName);
    // Whether a fresh reading of the daemon says this sink's factor is the one a percentage means. Used where
    // the assertion is about what the *daemon* holds rather than about what the shell says it holds — those
    // are two different claims, and only the pair of them is evidence.
    bool sinkFactorIs(const QString& nodeName, int percent);

    QTemporaryDir scratch_;
    QString remote_;
    QProcess daemon_;
};

void AudioLiveTest::initTestCase()
{
    const QString stock = QStringLiteral("%1/pipewire.conf").arg(QLatin1String(stockConfigDirectory));
    if (!QFile::exists(stock)) {
        QSKIP(qPrintable(QStringLiteral("the distribution's daemon config is not at %1, so there is no "
                                        "daemon this test can start")
                             .arg(stock)));
    }
    // The core name carries this process's id, so two runs of this test — or a run beside a developer's own
    // — cannot collide on one socket path.
    remote_ = QStringLiteral("qs-audio-test-%1").arg(QCoreApplication::applicationPid());

    QVERIFY2(scratch_.isValid(), "no scratch directory for the daemon's configuration");
    QVERIFY2(QDir(scratch_.path()).mkpath(QStringLiteral("conf")), "cannot create the config directory");

    // The daemon needs its whole configuration directory, not just the one file: the stock config's modules
    // are resolved through `context.spa-libs` and the drop-in directory beside it. Copying the directory and
    // pointing `PIPEWIRE_CONFIG_DIR` at the copy is how a second instance is run without touching the
    // system's own.
    const QString configDirectory = QStringLiteral("%1/conf").arg(scratch_.path());
    QFile stockFile(stock);
    QVERIFY(stockFile.open(QIODevice::ReadOnly));
    QByteArray config = stockFile.readAll();
    stockFile.close();

    // The core name is the socket's name, so this is the line that keeps the test's daemon off the
    // session's `pipewire-0`. Asserted rather than assumed: a stock config that stopped naming its core the
    // way this expects would otherwise start a second daemon on the *session's* socket.
    QVERIFY2(config.contains("core.name   = pipewire-0"),
             "the stock config no longer names its core `pipewire-0`, so this test cannot rename it and will "
             "not start a daemon that could collide with the session's");
    config.replace("core.name   = pipewire-0", QByteArray("core.name   = ") + remote_.toUtf8());

    // The objects, appended to the ones the stock config already creates. Written as the stock config writes
    // its own so that the daemon parses them the same way.
    const QByteArray objects =
        QByteArray("\n"
                   "    { factory = spa-node-factory\n"
                   "      args = { factory.name = support.node.driver node.name = QS-Audio-Test-Driver "
                   "priority.driver = 20000 } }\n"
                   "    { factory = adapter\n"
                   "      args = { factory.name = support.null-audio-sink node.name = ") +
        firstSink +
        QByteArray("\n"
                   "               media.class = Audio/Sink audio.position = [ FL FR ] } }\n"
                   "    { factory = adapter\n"
                   "      args = { factory.name = support.null-audio-sink node.name = ") +
        secondSink +
        QByteArray("\n"
                   "               media.class = Audio/Sink audio.position = [ FL FR ] } }\n"
                   "    { factory = metadata\n"
                   "      args = { metadata.name = default } }\n");

    // Inserted before the closing bracket of `context.objects`, which is the block the stock config ends
    // with. Found rather than assumed so a config that moved it fails here rather than starting a daemon
    // with no sinks in it.
    const int objectsStart = config.indexOf("context.objects = [");
    QVERIFY2(objectsStart >= 0, "the stock config no longer has a context.objects block");
    const int objectsEnd = config.indexOf("\n]", objectsStart);
    QVERIFY2(objectsEnd >= 0, "the stock config's context.objects block is not closed as expected");
    config.insert(objectsEnd, objects);

    QFile patched(QStringLiteral("%1/pipewire.conf").arg(configDirectory));
    QVERIFY2(patched.open(QIODevice::WriteOnly), qPrintable(patched.fileName()));
    QCOMPARE(patched.write(config), static_cast<qint64>(config.size()));
    patched.close();

    QVERIFY2(startDaemon(), qPrintable(daemon_.readAllStandardError()));

    // The default is set by this test rather than by the daemon's config, because the way WirePlumber writes
    // it is the way this test has to write it to be realistic: the value is JSON and its type says so. The
    // daemon's own config format has no way to spell that string.
    QVERIFY2(setDefaultSink(QLatin1String(firstSink)), "could not name a default sink");
}

void AudioLiveTest::cleanupTestCase()
{
    stopDaemon();
}

bool AudioLiveTest::startDaemon()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PIPEWIRE_CONFIG_DIR"),
                       QStringLiteral("%1/conf").arg(scratch_.path()));
    daemon_.setProcessEnvironment(environment);
    daemon_.setProgram(QStringLiteral("pipewire"));
    daemon_.setArguments({QStringLiteral("-c"), QStringLiteral("pipewire.conf")});
    daemon_.start();
    if (!daemon_.waitForStarted(5000))
        return false;

    // The socket, not a sleep: the daemon creates it when it is ready to be talked to. The path is
    // `$XDG_RUNTIME_DIR/<core.name>`, which is PipeWire's own rule and what makes the name in the config the
    // name of the socket.
    const QString socket = QStringLiteral("%1/%2").arg(qEnvironmentVariable("XDG_RUNTIME_DIR"), remote_);
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (QFile::exists(socket))
            return true;
        if (daemon_.state() != QProcess::Running)
            return false;
        QTest::qWait(25);
    }
    return false;
}

void AudioLiveTest::stopDaemon()
{
    if (daemon_.state() == QProcess::NotRunning)
        return;
    daemon_.terminate();
    if (!daemon_.waitForFinished(5000)) {
        daemon_.kill();
        daemon_.waitForFinished(5000);
    }
}

quint32 AudioLiveTest::nodeId(const QString& nodeName)
{
    const CommandResult dump = run(QStringLiteral("pw-dump"), {QStringLiteral("-r"), remote_});
    if (!dump.ok())
        return 0;
    const QJsonArray objects = QJsonDocument::fromJson(dump.out).array();
    for (const QJsonValue& value : objects) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("type")).toString() != QStringLiteral("PipeWire:Interface:Node"))
            continue;
        const QJsonObject props = object.value(QStringLiteral("info")).toObject()
                                      .value(QStringLiteral("props"))
                                      .toObject();
        if (props.value(QStringLiteral("node.name")).toString() == nodeName)
            return object.value(QStringLiteral("id")).toInt();
    }
    return 0;
}

std::optional<SinkState> AudioLiveTest::readSink(const QString& nodeName)
{
    const CommandResult dump = run(QStringLiteral("pw-dump"), {QStringLiteral("-r"), remote_});
    if (!dump.ok()) {
        qWarning() << "pw-dump failed:" << dump.exitCode << dump.err;
        return std::nullopt;
    }
    const QJsonArray objects = QJsonDocument::fromJson(dump.out).array();
    for (const QJsonValue& value : objects) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("type")).toString() != QStringLiteral("PipeWire:Interface:Node"))
            continue;
        const QJsonObject info = object.value(QStringLiteral("info")).toObject();
        if (info.value(QStringLiteral("props")).toObject().value(QStringLiteral("node.name")).toString() !=
            nodeName)
            continue;
        // A node publishes several `Props` params — a sink has one for its channels and another for its
        // device and latency settings — so the reader looks for the one that carries a volume, exactly as
        // the service does.
        for (const QJsonValue& param :
             info.value(QStringLiteral("params")).toObject().value(QStringLiteral("Props")).toArray()) {
            const QJsonObject props = param.toObject();
            const QJsonValue volumes = props.value(QStringLiteral("channelVolumes"));
            if (!volumes.isArray() || volumes.toArray().isEmpty())
                continue;
            SinkState state;
            state.channels = volumes.toArray().size();
            state.linearVolume = volumes.toArray().first().toDouble();
            state.muted = props.value(QStringLiteral("mute")).toBool();
            return state;
        }
    }
    return std::nullopt;
}

bool AudioLiveTest::writeSink(const QString& nodeName, double linearVolume, bool muted)
{
    const quint32 id = nodeId(nodeName);
    if (id == 0)
        return false;
    // A `Props` write, which is the same call the shell makes when the wheel turns — made here by a different
    // program, so that the shell is being told about a change it did not make.
    const QString props = QStringLiteral("{ channelVolumes: [%1, %1], mute: %2 }")
                              .arg(linearVolume, 0, 'f', 6)
                              .arg(muted ? QStringLiteral("true") : QStringLiteral("false"));
    const CommandResult result = run(QStringLiteral("pw-cli"),
                                     {QStringLiteral("-r"), remote_, QStringLiteral("s"),
                                      QString::number(id), QStringLiteral("Props"), props});
    if (!result.ok())
        qWarning() << "pw-cli failed:" << result.exitCode << result.err;
    return result.ok();
}

bool AudioLiveTest::sinkFactorIs(const QString& nodeName, int percent)
{
    const std::optional<SinkState> fromDaemon = readSink(nodeName);
    return fromDaemon.has_value() &&
           std::abs(fromDaemon->linearVolume - linearFromPercent(percent)) < 0.001;
}

bool AudioLiveTest::setDefaultSink(const QString& nodeName)
{
    const QByteArray value = QJsonDocument(QJsonObject{{QStringLiteral("name"), nodeName}})
                                 .toJson(QJsonDocument::Compact);
    // The four arguments are `id`, key, value and type, in `pw-metadata`'s own order. The type is named
    // because it is the part that matters: WirePlumber writes `Spa:String:JSON`, and a value written without
    // a type is a different kind of metadata entry that this module refuses.
    const CommandResult result =
        run(QStringLiteral("pw-metadata"),
            {QStringLiteral("-r"), remote_, QStringLiteral("-n"), QStringLiteral("default"),
             QStringLiteral("0"), QStringLiteral("default.audio.sink"), QString::fromUtf8(value),
             QStringLiteral("Spa:String:JSON")});
    if (!result.ok())
        qWarning() << "pw-metadata failed:" << result.exitCode << result.err;
    return result.ok();
}

void AudioLiveTest::theDaemonThisTestStartedIsTheOneTheShellReads()
{
    // The sink starts at no attenuation, so the shell's first reading is the daemon's own value and not
    // something this test put there.
    const std::optional<SinkState> fromDaemon = readSink(QLatin1String(firstSink));
    QVERIFY2(fromDaemon.has_value(), "the daemon this test started has no readable volume on its sink");
    QCOMPARE(fromDaemon->channels, 2);
    QCOMPARE(fromDaemon->muted, false);

    PipeWireService service;
    QSignalSpy reading(&service, &PipeWireService::readingChanged);
    QVERIFY2(!service.available(), "a service that has not been started already had a reading");

    service.start(remote_);

    // Waited for as an event, not as a duration: the reading arrives because the metadata names a sink and
    // that sink answered a Props request, and the spy is what says so.
    QTRY_VERIFY_WITH_TIMEOUT(service.available(), eventTimeoutMs);
    QVERIFY2(reading.count() > 0, "the reading arrived without the service saying anything about it");
    QCOMPARE(service.volumePercent(), quantum::audio::percentFromLinear(fromDaemon->linearVolume));
    QCOMPARE(service.muted(), fromDaemon->muted);

    // Both units, from one reading, each against an independent path to the same factor: the percentage against
    // the cube-root conversion of what `pw-dump` reports, and the decibels against the gain conversion of it.
    // Attenuating nothing is 0 dB, which is also the number that tells this test's daemon from the session's —
    // see the paragraph below, where a service that attached to the wrong core would report a device that is
    // not at unity.
    QCOMPARE(service.volumeDecibels(), quantum::audio::decibelsFromLinear(fromDaemon->linearVolume));
    QCOMPARE(service.volumeDecibels(), 0.0);

    // The session's own default sink is a real device whose volume this test never touches, so what keeps the
    // two apart is not an assertion about that device: it is the core name. The daemon was started with
    // `core.name = <this test's>`, its socket is at that name and at no other path, and the service was told
    // to attach to it. A service that ignored the name would attach to `pipewire-0` and report the session's
    // device instead — whose volume is not 100% here, because the two sinks this test made are the only ones
    // at no attenuation in this comparison.
    QCOMPARE(service.volumePercent(), 100);

    service.stop();
}

void AudioLiveTest::anExternalVolumeChangeArrivesWithoutTheShellAsking()
{
    PipeWireService service;
    QSignalSpy reading(&service, &PipeWireService::readingChanged);
    service.start(remote_);
    QTRY_VERIFY_WITH_TIMEOUT(service.available(), eventTimeoutMs);
    const int before = service.volumePercent();
    QVERIFY2(before != 50, "the sink is already at the value this slot writes, so nothing is proven");

    // 0.125 is a linear 50%, and it is written by `pw-cli` — a second client of the daemon, and nothing to do
    // with the shell. The service has no timer anywhere, so the only way the number below changes is that the
    // daemon sent it.
    QVERIFY2(writeSink(QLatin1String(firstSink), linearFromPercent(50), false),
             "could not write a volume from outside the shell");

    reading.clear();
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 50, eventTimeoutMs);
    QVERIFY2(reading.count() > 0, "the volume moved without the service emitting a reading");

    // And the daemon agrees, which is the difference between the shell having been told and the shell having
    // guessed: a fresh reading by another program says the same number.
    const std::optional<SinkState> fromDaemon = readSink(QLatin1String(firstSink));
    QVERIFY(fromDaemon.has_value());
    QCOMPARE(service.volumePercent(), quantum::audio::percentFromLinear(fromDaemon->linearVolume));

    // The decibels moved with the same event and are the same factor's other number: a linear 0.125 is
    // 20*log10(0.125), which is -18.06 dB, and the readout drawn in dB is what a person would be watching when
    // a headset button or another mixer moved this sink. The factor the test wrote is exact in binary — 0.5
    // cubed — so this comparison is an equality rather than a near miss.
    QCOMPARE(service.volumeDecibels(), quantum::audio::decibelsFromLinear(fromDaemon->linearVolume));
    QVERIFY2(std::abs(service.volumeDecibels() + 18.0618) < 0.001,
             qPrintable(QString::number(service.volumeDecibels(), 'f', 4)));

    service.stop();
}

void AudioLiveTest::anExternalMuteArrivesTheSameWay()
{
    PipeWireService service;
    service.start(remote_);
    QTRY_VERIFY_WITH_TIMEOUT(service.available(), eventTimeoutMs);
    QVERIFY2(writeSink(QLatin1String(firstSink), linearFromPercent(50), false),
             "could not clear the mute from outside the shell");
    QTRY_VERIFY_WITH_TIMEOUT(!service.muted(), eventTimeoutMs);

    QVERIFY2(writeSink(QLatin1String(firstSink), linearFromPercent(50), true),
             "could not mute from outside the shell");
    QTRY_VERIFY_WITH_TIMEOUT(service.muted(), eventTimeoutMs);
    // A mute is not a volume change and the volume is still the daemon's own: the reading carries both, which
    // is what lets the widget draw the percentage behind a mute rather than an invented zero.
    QCOMPARE(service.volumePercent(), 50);

    service.stop();
}

void AudioLiveTest::theShellsOwnWriteReachesTheDaemon()
{
    PipeWireService service;
    service.start(remote_);
    QTRY_VERIFY_WITH_TIMEOUT(service.available(), eventTimeoutMs);

    service.setVolumePercent(80);
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 80, eventTimeoutMs);

    // The daemon's own answer, read by a different program: the shell's number and the sink's factor are the
    // same setting, and the factor is the cube of the percentage rather than the percentage.
    QTRY_VERIFY_WITH_TIMEOUT(sinkFactorIs(QLatin1String(firstSink), 80), eventTimeoutMs);

    const std::optional<SinkState> fromDaemon = readSink(QLatin1String(firstSink));
    QVERIFY(fromDaemon.has_value());
    // Every channel, not just the first: a write that set one channel would unbalance the sink.
    QCOMPARE(fromDaemon->channels, 2);

    service.stop();
}

void AudioLiveTest::theWheelStepIsTheConfiguredStepAndClampsAtFullScale()
{
    PipeWireService service;
    service.start(remote_);
    QTRY_VERIFY_WITH_TIMEOUT(service.available(), eventTimeoutMs);
    QVERIFY2(writeSink(QLatin1String(firstSink), linearFromPercent(50), false), "could not seed a volume");
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 50, eventTimeoutMs);

    // The step is the configuration's, and the service applies it: ten points a notch instead of the default
    // five, which is the difference between a value that was read and a value that was written.
    service.setStepPercent(10);
    QCOMPARE(service.stepPercent(), 10);
    service.stepVolume(1);
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 60, eventTimeoutMs);
    service.stepVolume(-1);
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 50, eventTimeoutMs);

    // And the clamp: the last notch before full scale reaches it and the next one stays there rather than
    // climbing past what the shell is willing to write.
    service.setVolumePercent(95);
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 95, eventTimeoutMs);
    service.stepVolume(1);
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 100, eventTimeoutMs);
    service.stepVolume(1);
    QTest::qWait(200);
    QCOMPARE(service.volumePercent(), 100);

    // A step of zero is refused by the service, and the refusal is not a change: a wheel that did nothing
    // would otherwise silently become a step of one.
    service.setStepPercent(0);
    QCOMPARE(service.stepPercent(), 10);

    service.stop();
}

void AudioLiveTest::aNotchInDecibelsMovesTheDaemonByThatGain()
{
    PipeWireService service;
    service.start(remote_);
    QTRY_VERIFY_WITH_TIMEOUT(service.available(), eventTimeoutMs);

    // The unit is what the composition root hands over from `bar.audio.volume_scale`, so it is set here the way
    // the shell sets it — through the same door, from the same token — and the step the other unit carries is
    // set to something a percentage notch would be caught by: if the unit were ignored, the volume would move
    // twelve points and this slot would see it.
    service.setStepPercent(12);
    service.setStepDecibels(1.0);
    service.setWheelStepUnit(tokenFor(WheelStepUnit::Decibel));
    QCOMPARE(service.stepDecibels(), 1.0);
    QCOMPARE(service.wheelStepUnit(), WheelStepUnit::Decibel);

    QVERIFY2(writeSink(QLatin1String(firstSink), linearFromPercent(50), false), "could not seed a volume");
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 50, eventTimeoutMs);
    const double before = service.volumeDecibels();

    // One notch, and the claim is measured on the *daemon's* answer rather than on the shell's: the factor
    // read out of a fresh `pw-dump` is a ratio of `10^(1/20)` with the one before it, which is what a decibel
    // is. A step that wrote a percentage instead would be 12 points — `10^(60*log10(1.24)/20)` is nowhere near
    // this ratio — and a step that derived the gain from the rounded percentage would miss by the rounding.
    service.stepVolume(1);
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(service.volumeDecibels() - (before + 1.0)) < 0.0001, eventTimeoutMs);
    const std::optional<SinkState> afterStep = readSink(QLatin1String(firstSink));
    QVERIFY2(afterStep.has_value(), "the daemon did not report the sink after the notch");
    const double ratio = afterStep->linearVolume / linearFromPercent(50);
    QVERIFY2(std::abs(ratio - std::pow(10.0, 1.0 / 20.0)) < 0.0001,
             qPrintable(QStringLiteral("the daemon's factor moved by a ratio of %1, not by 1 dB")
                            .arg(ratio, 0, 'f', 6)));

    // And back down, which is the same notch the other way: a step that only ever rounded up would pass the
    // assertion above and fail this one.
    service.stepVolume(-1);
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(service.volumeDecibels() - before) < 0.0001, eventTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(sinkFactorIs(QLatin1String(firstSink), 50), eventTimeoutMs);

    // A step shorter than the daemon's own answer can express is refused by the service by name, and the value
    // it had stands — the floor is the readout's resolution, and it is enforced where the arithmetic happens
    // as well as where the file is read.
    service.setStepDecibels(0.05);
    QCOMPARE(service.stepDecibels(), 1.0);

    // The unit is the same kind of value: an unknown spelling is refused and the unit stays where it was, which
    // is the second opinion on a token the schema already refused.
    service.setWheelStepUnit(tokenFor(WheelStepUnit::Decibel) + QLatin1String("s"));
    QCOMPARE(service.wheelStepUnit(), WheelStepUnit::Decibel);

    // Switching the unit switches which step a notch applies, which is the whole point of the two keys: the
    // same wheel, the same start, twelve points instead of one decibel.
    service.setWheelStepUnit(tokenFor(WheelStepUnit::Percent));
    QCOMPARE(service.wheelStepUnit(), WheelStepUnit::Percent);
    service.stepVolume(1);
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 62, eventTimeoutMs);

    service.stop();
}

void AudioLiveTest::aStepWhileMutedChangesTheVolumeAndLeavesItMuted()
{
    PipeWireService service;
    service.start(remote_);
    QTRY_VERIFY_WITH_TIMEOUT(service.available(), eventTimeoutMs);
    QVERIFY2(writeSink(QLatin1String(firstSink), linearFromPercent(50), true), "could not mute the sink");
    QTRY_VERIFY_WITH_TIMEOUT(service.muted(), eventTimeoutMs);

    service.stepVolume(-1);
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 45, eventTimeoutMs);
    // `wpctl set-volume` on a muted sink leaves it muted, and so does this. A wheel that lifted the mute would
    // make "quieter" mean "loud again", which is the surprise the module's comment rules out — asserted here
    // because a comment is not a guarantee.
    QTest::qWait(200);
    QVERIFY2(service.muted(), "stepping the volume lifted the mute");
    const std::optional<SinkState> fromDaemon = readSink(QLatin1String(firstSink));
    QVERIFY(fromDaemon.has_value());
    QVERIFY2(fromDaemon->muted, "the daemon's own sink is no longer muted, so the shell unmuted it");

    service.stop();
}

void AudioLiveTest::switchingTheDefaultSinkMovesTheReading()
{
    PipeWireService service;
    service.start(remote_);
    QTRY_VERIFY_WITH_TIMEOUT(service.available(), eventTimeoutMs);

    // Two sinks at different volumes, so "which sink is being read" is a number rather than a name.
    QVERIFY2(writeSink(QLatin1String(firstSink), linearFromPercent(30), false), "could not set sink one");
    QVERIFY2(writeSink(QLatin1String(secondSink), linearFromPercent(70), false), "could not set sink two");
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 30, eventTimeoutMs);

    // The switch is a metadata write by another program, which is what WirePlumber does when a device is
    // chosen in a panel. The shell has no request of its own for it: it follows the default because the
    // metadata says which sink the default is.
    QVERIFY2(setDefaultSink(QLatin1String(secondSink)), "could not switch the default sink");
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 70, eventTimeoutMs);

    // And back, because the switch has to work in both directions rather than being a one-way binding.
    QVERIFY2(setDefaultSink(QLatin1String(firstSink)), "could not switch the default sink back");
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 30, eventTimeoutMs);

    // The hidden half of the same claim: the sink the shell is no longer following must not be moving it.
    // Sink two is rewritten and the reading must not follow.
    QVERIFY2(writeSink(QLatin1String(secondSink), linearFromPercent(15), false), "could not set sink two");
    QTest::qWait(500);
    QCOMPARE(service.volumePercent(), 30);

    service.stop();
}

void AudioLiveTest::theDaemonGoingAwayWithdrawsTheReadingAndComingBackRestoresIt()
{
    PipeWireService service;
    service.start(remote_);
    QTRY_VERIFY_WITH_TIMEOUT(service.available(), eventTimeoutMs);
    QVERIFY2(writeSink(QLatin1String(firstSink), linearFromPercent(65), false), "could not seed a volume");
    QTRY_VERIFY_WITH_TIMEOUT(service.volumePercent() == 65, eventTimeoutMs);

    // The daemon stops. A shell showing the volume from before would be showing a number that is no longer
    // anyone's, so the reading is withdrawn rather than kept — the same rule the compositor connection
    // follows when niri goes away, and asserted here for the same reason.
    stopDaemon();
    QTRY_VERIFY_WITH_TIMEOUT(!service.available(), eventTimeoutMs);
    // Both units go with the reading, and the decibel one has no value of its own to leave standing: the
    // service publishes 0 for an absent reading rather than the -18 dB of the sink it used to follow, which is
    // what the widget's availability flag is for.
    QCOMPARE(service.volumeDecibels(), 0.0);

    // The daemon comes back on the same core name, so the same socket path, without the shell being restarted:
    // the reconnect is a backoff between attempts. Its sinks are new objects at no attenuation and its metadata
    // carries no default, so the shell is still showing nothing — which is the other half of the same rule. A
    // module that guessed a sink when none was named would put a different device's volume on the bar, and it
    // would do it here, where the daemon it just reattached to has two sinks and has named neither.
    QVERIFY2(startDaemon(), qPrintable(daemon_.readAllStandardError()));
    QTest::qWait(1000);
    QVERIFY2(!service.available(),
             "a reattached daemon that names no default sink was read as one that does");

    // Naming one is what makes the reading arrive, and the reading is the new daemon's: the sinks it created a
    // moment ago are at no attenuation, so 100% and not the 65% the daemon before it was left at.
    QVERIFY2(setDefaultSink(QLatin1String(firstSink)), "could not name the default sink again");
    QTRY_VERIFY_WITH_TIMEOUT(service.available(), eventTimeoutMs);
    QCOMPARE(service.volumePercent(), 100);
    QVERIFY2(sinkFactorIs(QLatin1String(firstSink), 100),
             "the daemon's own sink is not at full scale, so the reading came from somewhere else");

    service.stop();
}

QTEST_GUILESS_MAIN(AudioLiveTest)

#include "audio_live_test.moc"
