// The volume's on-screen display: when it exists, what it says, and when it goes.
//
// The host is driven the way the shell drives it — by the audio service's `volumeAdjusted` signal — against a
// real `PipeWireService` that is never started, so no daemon is involved and its reading stays the honest empty
// state. That is deliberate: what the *service* announces, and for which changes, is asserted where a daemon
// exists (`audio-live-test`). What is asserted here is what the host and the surface do with the announcement,
// and the surface's rule for turning a reading into text, which is a function of its inputs so it can be given
// the numbers a daemon sends.
//
// The platform is the offscreen one, so the surfaces are windows on a screen of this process's own and never on
// the desktop of whoever runs the test.
#include "app/OsdHost.h"
#include "audio/PipeWireService.h"
#include "config/Config.h"
#include "wayland/LayerShellWindow.h"

#include <QGuiApplication>
#include <QQmlEngine>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QWindow>

#include <memory>

using quantum::config::Config;
using quantum::config::OsdConfig;

namespace {

constexpr int settleMs = 5000;

// A QML component that cannot evaluate a binding says so with a warning and carries on drawing something,
// which is how a call to a function Qt does not have went unseen until a run's output was read: the surface
// still appeared. Every warning of the process is recorded and each slot asserts there were none.
QStringList warningsSeen;
QtMessageHandler previousHandler = nullptr;

void recordWarnings(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        warningsSeen.append(message);
    if (previousHandler != nullptr)
        previousHandler(type, context, message);
}

}  // namespace

class OsdTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void nothingIsUpBeforeAnAdjustment();
    void anAdjustmentShowsOneSurfacePerOutputAndTheClockWithdrawsThem();
    void aFurtherAdjustmentRestartsTheClockAndAddsNoSurface();
    void aDisabledDisplayIsNeverShownAndDisablingOneThatIsUpWithdrawsIt();
    void theSurfaceIsTheOverlayNamedInThePublicInterface();
    void theSurfaceSaysWhatTheReadingIs();
    void theSurfaceFollowsTheConfiguredUnit();

private:
    void adjust() { QMetaObject::invokeMethod(&audio_, "volumeAdjusted"); }
    void configure(bool show, int timeoutMs)
    {
        OsdConfig values;
        values.showOsd = show;
        values.timeoutMs = timeoutMs;
        config_.bar()->osd()->apply(values);
    }
    QVariant call(QWindow* window, const char* function, const QVariantList& args) const;

    Config config_;
    quantum::audio::PipeWireService audio_;
    std::unique_ptr<QQmlEngine> engine_;
    std::unique_ptr<quantum::app::OsdHost> host_;
};

void OsdTest::initTestCase()
{
    engine_ = std::make_unique<QQmlEngine>();
    qmlRegisterType<QuantumShell::LayerShellWindow>("QuantumShell", 1, 0, "LayerShellWindow");
    quantum::config::Config::registerQmlSingleton(config_);
    quantum::audio::PipeWireService::registerQmlSingleton(audio_);
    host_ = std::make_unique<quantum::app::OsdHost>(audio_, config_, *engine_,
                                                    QUrl::fromLocalFile(QStringLiteral(QS_OSD_QML)));
    QVERIFY2(host_->ready(), qPrintable(host_->componentError()));
}

void OsdTest::cleanup()
{
    QVERIFY2(warningsSeen.isEmpty(), qPrintable(warningsSeen.join(QStringLiteral("; "))));
}

void OsdTest::init()
{
    warningsSeen.clear();
    // Every slot starts from the same place whatever ran before it in a shuffled order: the display enabled at
    // its shortest timeout, and nothing up. Switching it off withdraws a display that is up, which is the
    // behaviour one slot asserts and the reset every other slot relies on.
    configure(false, 500);
    QTRY_VERIFY_WITH_TIMEOUT(host_->windows().isEmpty(), settleMs);
    configure(true, 500);
}

QVariant OsdTest::call(QWindow* window, const char* function, const QVariantList& args) const
{
    QVariant result;
    const bool ok = [&] {
        switch (args.size()) {
        case 3:
            return QMetaObject::invokeMethod(window, function, Q_RETURN_ARG(QVariant, result),
                                             Q_ARG(QVariant, args[0]), Q_ARG(QVariant, args[1]),
                                             Q_ARG(QVariant, args[2]));
        default:
            return QMetaObject::invokeMethod(window, function, Q_RETURN_ARG(QVariant, result),
                                             Q_ARG(QVariant, args[0]), Q_ARG(QVariant, args[1]),
                                             Q_ARG(QVariant, args[2]), Q_ARG(QVariant, args[3]),
                                             Q_ARG(QVariant, args[4]));
        }
    }();
    if (!ok)
        qWarning() << "no such function on the surface:" << function;
    return result;
}

void OsdTest::nothingIsUpBeforeAnAdjustment()
{
    // A display exists for a change and for nothing else: a host that showed one at construction would put a
    // volume bar on every start of the shell.
    QVERIFY(host_->windows().isEmpty());
}

void OsdTest::anAdjustmentShowsOneSurfacePerOutputAndTheClockWithdrawsThem()
{
    const int outputs = QGuiApplication::screens().size();
    QVERIFY2(outputs > 0, "no screen for a display to be created against");

    adjust();
    QTRY_COMPARE_WITH_TIMEOUT(host_->windows().size(), outputs, settleMs);
    for (QWindow* window : host_->windows())
        QVERIFY2(window->isVisible(), "a display was created for an output it is not on");

    // Withdrawn by its own clock: the timeout is the configuration's 500 ms here, so this waits for an event
    // and not for a duration this test chose.
    QTRY_COMPARE_WITH_TIMEOUT(host_->windows().size(), 0, settleMs);
}

void OsdTest::aFurtherAdjustmentRestartsTheClockAndAddsNoSurface()
{
    configure(true, 1000);
    adjust();
    QTRY_VERIFY_WITH_TIMEOUT(!host_->windows().isEmpty(), settleMs);
    const QList<QWindow*> first = host_->windows();

    // A second change 600 ms in, when the first clock has 400 ms left: the surfaces are the same objects, and
    // they are still up 600 ms after that, which the first clock alone would not allow.
    QTest::qWait(600);
    QCOMPARE(host_->windows(), first);
    adjust();
    QCOMPARE(host_->windows(), first);
    QTest::qWait(600);
    QCOMPARE(host_->windows(), first);

    QTRY_COMPARE_WITH_TIMEOUT(host_->windows().size(), 0, settleMs);
}

void OsdTest::aDisabledDisplayIsNeverShownAndDisablingOneThatIsUpWithdrawsIt()
{
    configure(false, 500);
    adjust();
    QTest::qWait(150);
    QVERIFY2(host_->windows().isEmpty(), "a display switched off was shown");

    configure(true, 5000);
    adjust();
    QTRY_VERIFY_WITH_TIMEOUT(!host_->windows().isEmpty(), settleMs);
    // Well inside its five seconds: what takes it down is the switch and not the clock.
    configure(false, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(host_->windows().size(), 0, 1000);
}

void OsdTest::theSurfaceIsTheOverlayNamedInThePublicInterface()
{
    adjust();
    QTRY_VERIFY_WITH_TIMEOUT(!host_->windows().isEmpty(), settleMs);
    for (QWindow* window : host_->windows()) {
        QCOMPARE(window->property("layerNamespace").toString(), QStringLiteral("quantum-shell-osd"));
        QCOMPARE(window->property("layer").toInt(), static_cast<int>(QuantumShell::LayerShellWindow::Overlay));
        QCOMPARE(window->property("anchors").toInt(),
                 static_cast<int>(QuantumShell::LayerShellWindow::BottomEdge));
        QCOMPARE(window->property("keyboardInteractivity").toInt(),
                 static_cast<int>(QuantumShell::LayerShellWindow::NoKeyboard));
        // Reserves nothing, and names both sizes: anchored to one edge it would otherwise be proposed the
        // whole output.
        QCOMPARE(window->property("exclusiveZone").toInt(), 0);
        QCOMPARE(window->width(), 300);
        QCOMPARE(window->height(), 132);
    }
}

void OsdTest::theSurfaceSaysWhatTheReadingIs()
{
    adjust();
    QTRY_VERIFY_WITH_TIMEOUT(!host_->windows().isEmpty(), settleMs);
    QWindow* window = host_->windows().first();

    // The rule, given the numbers a daemon sends. Percent: the number and a sign; a mute is not a level; silence
    // in decibels is negative infinity and is drawn as that symbol; no reading is a dash.
    const auto text = [&](const QString& scale, bool available, bool muted, int percent, double decibels) {
        return call(window, "valueText", {scale, available, muted, percent, decibels}).toString();
    };
    QCOMPARE(text(QStringLiteral("percent"), true, false, 42, -22.61), QStringLiteral("42%"));
    QCOMPARE(text(QStringLiteral("decibel"), true, false, 42, -22.61), QStringLiteral("-22.6 dB"));
    QCOMPARE(text(QStringLiteral("decibel"), true, false, 0, -std::numeric_limits<double>::infinity()),
             QStringLiteral("-∞ dB"));
    QCOMPARE(text(QStringLiteral("percent"), true, true, 42, -22.61), QStringLiteral("MUTE"));
    QCOMPARE(text(QStringLiteral("decibel"), true, true, 42, -22.61), QStringLiteral("MUTE"));
    QCOMPARE(text(QStringLiteral("percent"), false, false, 42, -22.61), QStringLiteral("—"));

    // The bar's fill: a position in the range while there is a level, and nothing for a mute or no reading.
    const auto fill = [&](bool available, bool muted, int percent) {
        return call(window, "fillFor", {available, muted, percent}).toDouble();
    };
    QCOMPARE(fill(true, false, 42), 0.42);
    QCOMPARE(fill(true, false, 0), 0.0);
    QCOMPARE(fill(true, false, 100), 1.0);
    QCOMPARE(fill(true, false, 130), 1.0);
    QCOMPARE(fill(true, true, 42), 0.0);
    QCOMPARE(fill(false, false, 42), 0.0);

    // And it is bound to the service rather than to anything the host hands it: the service has no reading, so
    // the surface shows the dash and an empty bar.
    QVERIFY(!audio_.available());
    QCOMPARE(window->property("valueLabel").toString(), QStringLiteral("—"));
    QCOMPARE(window->property("fill").toDouble(), 0.0);
}

void OsdTest::theSurfaceFollowsTheConfiguredUnit()
{
    // The unit is the bar's own `[bar.audio] volume_scale`, read by the surface from the configuration — the
    // one key names the unit for the readout, the wheel's step and this. Switched while the surface is up: the
    // value the surface holds for the unit follows without the surface being rebuilt.
    adjust();
    QTRY_VERIFY_WITH_TIMEOUT(!host_->windows().isEmpty(), settleMs);
    QWindow* window = host_->windows().first();
    quantum::config::AudioConfig audio = config_.bar()->audio()->values();
    const QString before = audio.volumeScale;
    audio.volumeScale = QStringLiteral("decibel");
    config_.bar()->audio()->apply(audio);
    QCOMPARE(call(window, "valueText",
                  {config_.bar()->audio()->volumeScale(), true, false, 42, -22.61}).toString(),
             QStringLiteral("-22.6 dB"));
    audio.volumeScale = before;
    config_.bar()->audio()->apply(audio);
    QCOMPARE(call(window, "valueText",
                  {config_.bar()->audio()->volumeScale(), true, false, 42, -22.61}).toString(),
             QStringLiteral("42%"));
}

#include "osd_test.moc"

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    previousHandler = qInstallMessageHandler(recordWarnings);
    OsdTest test;
    return QTest::qExec(&test, argc, argv);
}
