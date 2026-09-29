// The launcher's surface end to end, offscreen: a toggle opens it, keys typed into it reach the service, Enter
// starts a real process and Escape closes it.
//
// The model — parsing, ranking, scanning — is `apps-test`'s and is not repeated here. What is asserted here is
// what only exists once a surface is involved: which window `LauncherHost` creates and with what layer-shell
// properties, that a keystroke delivered to that window's own event path arrives in the service, and that the
// service and the surface agree about whether the launcher is open in every direction it can be closed.
//
// The directory the service scans is this test's own, so the entries — and the marker a launched `touch`
// creates — are the answer for files this test wrote.
#include "app/LauncherHost.h"
#include "BackdropClick.h"
#include "apps/LauncherService.h"
#include "config/Config.h"
#include "wayland/LayerShellWindow.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <memory>

namespace {

constexpr int settleMs = 5000;

// Every warning of the process is recorded and each slot asserts there were none, for the reason `osd-test`
// does: a QML component that cannot evaluate a binding warns and carries on drawing something.
QStringList warningsSeen;
QtMessageHandler previousHandler = nullptr;

void recordWarnings(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        warningsSeen.append(message);
    if (previousHandler != nullptr)
        previousHandler(type, context, message);
}

bool write(const QString& path, const QString& text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(text.toUtf8());
    return true;
}

QString entry(const QString& name, const QString& exec)
{
    return QStringLiteral("[Desktop Entry]\nType=Application\nName=%1\nExec=%2\n").arg(name, exec);
}

}  // namespace

class LauncherTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void nothingIsUpUntilTheLauncherIsOpened();
    void openingCreatesOneOverlaySurfaceThatTakesTheKeyboard();
    void keysTypedIntoTheSurfaceReachTheServiceAndEscapeCloses();
    void theArrowsMoveTheHighlightAndEnterStartsARealProcess();
    void aSurfaceTheCompositorClosesLeavesTheServiceClosed();
    void theSurfaceGrowsWithItsResultsAndSaysWhenThereAreNone();
    void aClickOutsideThePanelClosesItAndTheBackdropGoesWithIt();

private:
    QWindow* openLauncher();

    QTemporaryDir root_;
    QString marker_;
    quantum::config::Config config_;
    std::unique_ptr<quantum::apps::LauncherService> service_;
    std::unique_ptr<QQmlEngine> engine_;
    std::unique_ptr<quantum::app::LauncherHost> host_;
};

void LauncherTest::initTestCase()
{
    QVERIFY(root_.isValid());
    const QString touch = QStandardPaths::findExecutable(QStringLiteral("touch"));
    if (touch.isEmpty())
        QSKIP("no `touch` on this machine to start");
    marker_ = root_.filePath(QStringLiteral("launched marker"));
    QVERIFY(write(root_.filePath(QStringLiteral("applications/alpha.desktop")),
                  entry(QStringLiteral("Alpha Editor"), QStringLiteral("%1 \"%2\"").arg(touch, marker_))));
    QVERIFY(write(root_.filePath(QStringLiteral("applications/beta.desktop")),
                  entry(QStringLiteral("Beta Player"), QStringLiteral("/nonexistent/qs-launcher-test-binary"))));

    service_ = std::make_unique<quantum::apps::LauncherService>(QStringList{root_.path()}, QStringList{}, QString());
    service_->refresh();
    QTRY_COMPARE_WITH_TIMEOUT(service_->applicationCount(), 2, settleMs);

    engine_ = std::make_unique<QQmlEngine>();
    qmlRegisterType<QuantumShell::LayerShellWindow>("QuantumShell", 1, 0, "LayerShellWindow");
    quantum::config::Config::registerQmlSingleton(config_);
    quantum::apps::LauncherService::registerQmlSingleton(*service_);
    host_ = std::make_unique<quantum::app::LauncherHost>(*service_, *engine_,
                                                         QUrl::fromLocalFile(QStringLiteral(QS_LAUNCHER_QML)),
                                                         QUrl::fromLocalFile(QStringLiteral(QS_BACKDROP_QML)));
    QVERIFY2(host_->ready(), qPrintable(host_->componentError()));
}

void LauncherTest::init()
{
    warningsSeen.clear();
    // Every slot starts from the same place whatever ran before it in a shuffled order: closed, no query, no
    // marker, and the full list back.
    service_->setOpen(false);
    QTRY_VERIFY_WITH_TIMEOUT(host_->window() == nullptr, settleMs);
    service_->setQuery(QString());
    QFile::remove(marker_);
}

void LauncherTest::cleanup()
{
    QVERIFY2(warningsSeen.isEmpty(), qPrintable(warningsSeen.join(QStringLiteral("; "))));
}

QWindow* LauncherTest::openLauncher()
{
    service_->setOpen(true);
    if (!QTest::qWaitFor([&] { return host_->window() != nullptr && host_->window()->isVisible(); }, settleMs))
        return nullptr;
    // The rescan the opening started, so the list a slot reads is the full one and not the previous slot's.
    if (!QTest::qWaitFor([&] { return !service_->scanning() && service_->results().size() > 0; }, settleMs))
        return nullptr;
    return host_->window();
}

void LauncherTest::nothingIsUpUntilTheLauncherIsOpened()
{
    QVERIFY2(host_->window() == nullptr, "a launcher surface existed before anything opened it");
    QVERIFY(!service_->isOpen());
}

void LauncherTest::openingCreatesOneOverlaySurfaceThatTakesTheKeyboard()
{
    QWindow* window = openLauncher();
    QVERIFY(window != nullptr);
    QCOMPARE(window->property("layerNamespace").toString(), QStringLiteral("quantum-shell-launcher"));
    QCOMPARE(window->property("layer").toInt(), static_cast<int>(QuantumShell::LayerShellWindow::Overlay));
    QCOMPARE(window->property("keyboardInteractivity").toInt(),
             static_cast<int>(QuantumShell::LayerShellWindow::ExclusiveKeyboard));
    // Anchored to nothing, which is what centres it, and reserving nothing.
    QCOMPARE(window->property("anchors").toInt(), 0);
    QCOMPARE(window->property("exclusiveZone").toInt(), 0);
    QCOMPARE(window->width(), 600);
    QCOMPARE(window->screen(), QGuiApplication::primaryScreen());

    // One surface, and asking again does not make a second: the same object is still the launcher.
    service_->setOpen(true);
    QCOMPARE(host_->window(), window);

    // Closed through the service, and the surface goes with it.
    service_->setOpen(false);
    QTRY_VERIFY_WITH_TIMEOUT(host_->window() == nullptr, settleMs);
}

void LauncherTest::keysTypedIntoTheSurfaceReachTheServiceAndEscapeCloses()
{
    QWindow* window = openLauncher();
    QVERIFY(window != nullptr);
    QCOMPARE(service_->results().size(), 2);

    // Typed through the window's own event path, as a compositor's keyboard events arrive.
    for (const char key : {'a', 'l', 'p'})
        QTest::keyClick(window, key);
    // The keyboard focus is the text field's: it is what typing has to reach, and a focus on any other item
    // would leave the field empty however many keys arrived.
    auto* quick = qobject_cast<QQuickWindow*>(window);
    QVERIFY(quick != nullptr);
    QVERIFY2(quick->activeFocusItem() != nullptr
                 && quick->activeFocusItem()->objectName() == QLatin1String("launcherInput"),
             "the launcher's keyboard focus is not on its text field");
    QTRY_COMPARE_WITH_TIMEOUT(service_->query(), QStringLiteral("alp"), settleMs);
    QCOMPARE(service_->results().size(), 1);
    QCOMPARE(service_->results().at(0).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("Alpha Editor"));

    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(!service_->isOpen(), settleMs);
    QTRY_VERIFY_WITH_TIMEOUT(host_->window() == nullptr, settleMs);

    // Opening again starts from an empty query: what was typed last time is not what is wanted now.
    QVERIFY(openLauncher() != nullptr);
    QCOMPARE(service_->query(), QString());
}

void LauncherTest::theArrowsMoveTheHighlightAndEnterStartsARealProcess()
{
    QWindow* window = openLauncher();
    QVERIFY(window != nullptr);
    QCOMPARE(service_->selectedIndex(), 0);
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE_WITH_TIMEOUT(service_->selectedIndex(), 1, settleMs);
    QTest::keyClick(window, Qt::Key_Up);
    QTRY_COMPARE_WITH_TIMEOUT(service_->selectedIndex(), 0, settleMs);

    // The first result is "Alpha Editor", whose Exec is `touch "<marker>"`: the marker appearing is the process
    // having been started by Enter with the exact argument the entry named.
    QVERIFY(!QFileInfo::exists(marker_));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker_), settleMs);
    QTRY_VERIFY_WITH_TIMEOUT(!service_->isOpen(), settleMs);
    QTRY_VERIFY_WITH_TIMEOUT(host_->window() == nullptr, settleMs);
}

void LauncherTest::aSurfaceTheCompositorClosesLeavesTheServiceClosed()
{
    QWindow* window = openLauncher();
    QVERIFY(window != nullptr);
    // The compositor closing a layer surface is the window becoming invisible; the service has to hear it, or
    // the next toggle would try to close what is gone and the launcher could never be opened again.
    window->hide();
    QTRY_VERIFY_WITH_TIMEOUT(!service_->isOpen(), settleMs);
    QVERIFY(service_->toggle());
    QTRY_VERIFY_WITH_TIMEOUT(host_->window() != nullptr, settleMs);
}

void LauncherTest::theSurfaceGrowsWithItsResultsAndSaysWhenThereAreNone()
{
    QWindow* window = openLauncher();
    QVERIFY(window != nullptr);
    const int two = window->height();
    service_->setQuery(QStringLiteral("alp"));
    QTRY_VERIFY_WITH_TIMEOUT(window->height() < two, settleMs);
    const int one = window->height();
    QCOMPARE(two - one, window->property("rowHeight").toInt());

    // No match still has a row, which says so: the height does not collapse to nothing.
    service_->setQuery(QStringLiteral("zzz"));
    QTRY_COMPARE_WITH_TIMEOUT(service_->results().size(), 0, settleMs);
    QTRY_COMPARE_WITH_TIMEOUT(window->height(), one, settleMs);
}

#include "launcher_test.moc"

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    previousHandler = qInstallMessageHandler(recordWarnings);
    LauncherTest test;
    return QTest::qExec(&test, argc, argv);
}

void LauncherTest::aClickOutsideThePanelClosesItAndTheBackdropGoesWithIt()
{
    QVERIFY(host_->backdropWindow() == nullptr);
    QWindow* window = openLauncher();
    QVERIFY(window != nullptr);
    QWindow* backdrop = host_->backdropWindow();
    backdrop::verifyIsABackdrop(backdrop);
    QVERIFY(backdrop != window);

    // A press on the backdrop is a click outside the panel: it closes the launcher through the service, the way
    // Escape does, and the backdrop is taken down with the panel.
    backdrop::click(backdrop);
    QTRY_VERIFY_WITH_TIMEOUT(!service_->isOpen(), settleMs);
    QTRY_VERIFY_WITH_TIMEOUT(host_->window() == nullptr, settleMs);
    QTRY_VERIFY_WITH_TIMEOUT(host_->backdropWindow() == nullptr, settleMs);
}
