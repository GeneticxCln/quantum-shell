// The control centre's surface end to end, offscreen: a toggle opens it, and the controls on it reach the services
// they are drawn from.
//
// What is asserted here is what only exists once a surface is involved. Which window `ControlCenterHost` creates
// and with what layer-shell properties; that a click on the Do Not Disturb row flips the notification service's
// own property; that a click on a transport button reaches the MPRIS player being followed — a real player double
// on a private bus, so the request is observed arriving at the player rather than assumed — and that with no
// player the same buttons ask nothing; that the volume section says a dash and is inert while the audio service
// has no sink, and that its text and geometry rules give the right answers for the readings a daemon sends. The
// audio write itself (`setVolumePercent`, `toggleMute`) is proven against a real PipeWire daemon by
// `audio-live-test`, and is not repeated here.
#include "app/ControlCenterHost.h"
#include "app/CalendarHost.h"
#include "app/CalendarService.h"
#include "app/ControlCenterService.h"
#include "app/PanelGroup.h"
#include "apps/LauncherService.h"
#include "BackdropClick.h"
#include "audio/PipeWireService.h"
#include "config/Config.h"
#include "dbus/MediaService.h"
#include "dbus/NotificationService.h"
#include "wayland/LayerShellWindow.h"

#include "MprisFixtures.h"

#include <QDate>
#include <functional>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QTest>
#include <QWindow>

#include <memory>

namespace {

constexpr int settleMs = 5000;

// Every warning of the process is recorded and each slot asserts there were none, for the reason `osd-test` does.
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

class ControlCenterTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void nothingIsUpUntilItIsOpened();
    void openingCreatesOneOverlaySurfaceInTheCorner();
    void aClickOnDoNotDisturbFlipsTheNotificationServicesOwnMode();
    void withNoPlayerTheTransportButtonsAskNothing();
    void aClickOnTransportReachesTheFollowedPlayer();
    void theVolumeSectionIsInertWithoutASink();
    void theTextAndGeometryRulesAnswerForTheReadingsADaemonSends();
    void escapeClosesAndASurfaceTheCompositorClosesLeavesItClosed();
    void aClickOutsideThePanelClosesItAndTheBackdropGoesWithIt();
    void openingOnePanelClosesTheOthers();
    void theCalendarDrawsTheMonthItIsOpenedOnAndMovesByMonth();

private:
    QWindow* openPanel();
    bool click(QWindow* window, const char* objectName);
    QVariant call(QWindow* window, const char* function, const QVariantList& args) const;

    quantum::config::Config config_;
    quantum::audio::PipeWireService audio_;
    quantum::dbus::NotificationService notifications_;
    quantum::dbus::MediaService media_;
    quantum::app::ControlCenterService state_;
    std::unique_ptr<QQmlEngine> engine_;
    std::unique_ptr<quantum::app::ControlCenterHost> host_;
    std::unique_ptr<mprisfix::PrivateMediaBus> bus_;
    std::unique_ptr<mprisfix::FakeMprisPlayer> player_;
};

void ControlCenterTest::initTestCase()
{
    engine_ = std::make_unique<QQmlEngine>();
    qmlRegisterType<QuantumShell::LayerShellWindow>("QuantumShell", 1, 0, "LayerShellWindow");
    quantum::config::Config::registerQmlSingleton(config_);
    quantum::audio::PipeWireService::registerQmlSingleton(audio_);
    quantum::dbus::NotificationService::registerQmlSingleton(notifications_);
    quantum::dbus::MediaService::registerQmlSingleton(media_);
    quantum::app::ControlCenterService::registerQmlSingleton(state_);
    host_ = std::make_unique<quantum::app::ControlCenterHost>(
        state_, *engine_, QUrl::fromLocalFile(QStringLiteral(QS_CONTROL_CENTER_QML)),
        QUrl::fromLocalFile(QStringLiteral(QS_BACKDROP_QML)));
    QVERIFY2(host_->ready(), qPrintable(host_->componentError()));

    // The media service follows the private bus for the life of the binary, so a slot that needs a player creates
    // one and removes it again: no slot depends on what another left on the bus, whatever order they run in.
    bus_ = std::make_unique<mprisfix::PrivateMediaBus>();
    QVERIFY(bus_->start());
    media_.start(bus_->connect(QStringLiteral("control-reader")));
}

void ControlCenterTest::init()
{
    warningsSeen.clear();
    state_.setOpen(false);
    QTRY_VERIFY_WITH_TIMEOUT(host_->window() == nullptr, settleMs);
    notifications_.setNotificationDoNotDisturb(false);
}

void ControlCenterTest::cleanup()
{
    QVERIFY2(warningsSeen.isEmpty(), qPrintable(warningsSeen.join(QStringLiteral("; "))));
}

QWindow* ControlCenterTest::openPanel()
{
    state_.setOpen(true);
    if (!QTest::qWaitFor([&] { return host_->window() != nullptr && host_->window()->isVisible(); }, settleMs))
        return nullptr;
    QWindow* window = host_->window();
    // Offscreen there is no compositor configure to size the window's content item, so it is told: a resize by a
    // pixel and back is the event a configure is, and without it the content item is 0×0 and every child anchored
    // to it has a negative size, which a click cannot land on.
    const QSize size = window->size();
    window->resize(size.width() + 1, size.height());
    window->resize(size);
    // The resize is delivered as a window-system event, so it is waited for as the event it is.
    auto* quick = qobject_cast<QQuickWindow*>(window);
    if (quick == nullptr
        || !QTest::qWaitFor([&] { return quick->contentItem()->width() == size.width(); }, settleMs))
        return nullptr;
    return window;
}

namespace {

// An item by object name, searched through the visual tree: a delegate a Repeater made is a child *item* of its
// row, but not a QObject child of anything `findChild` would walk from the window.
QQuickItem* itemNamed(QQuickItem* root, const QString& name)
{
    if (root == nullptr)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = itemNamed(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem* itemNamed(QWindow* window, const char* name)
{
    auto* quick = qobject_cast<QQuickWindow*>(window);
    return quick == nullptr ? nullptr : itemNamed(quick->contentItem(), QLatin1String(name));
}

}  // namespace

bool ControlCenterTest::click(QWindow* window, const char* objectName)
{
    QQuickItem* item = itemNamed(window, objectName);
    if (item == nullptr)
        return false;
    const QPointF centre = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QTest::mouseClick(window, Qt::LeftButton, {}, centre.toPoint());
    return true;
}

QVariant ControlCenterTest::call(QWindow* window, const char* function, const QVariantList& args) const
{
    QVariant result;
    const bool ok = [&] {
        switch (args.size()) {
        case 2:
            return QMetaObject::invokeMethod(window, function, Q_RETURN_ARG(QVariant, result),
                                             Q_ARG(QVariant, args[0]), Q_ARG(QVariant, args[1]));
        case 3:
            return QMetaObject::invokeMethod(window, function, Q_RETURN_ARG(QVariant, result),
                                             Q_ARG(QVariant, args[0]), Q_ARG(QVariant, args[1]),
                                             Q_ARG(QVariant, args[2]));
        default:
            return QMetaObject::invokeMethod(window, function, Q_RETURN_ARG(QVariant, result),
                                             Q_ARG(QVariant, args[0]), Q_ARG(QVariant, args[1]),
                                             Q_ARG(QVariant, args[2]), Q_ARG(QVariant, args[3]));
        }
    }();
    if (!ok)
        qWarning() << "no such function on the panel:" << function;
    return result;
}

void ControlCenterTest::nothingIsUpUntilItIsOpened()
{
    QVERIFY2(host_->window() == nullptr, "a control centre surface existed before anything opened it");
    QVERIFY(!state_.isOpen());
}

void ControlCenterTest::openingCreatesOneOverlaySurfaceInTheCorner()
{
    QWindow* window = openPanel();
    QVERIFY(window != nullptr);
    using LSW = QuantumShell::LayerShellWindow;
    QCOMPARE(window->property("layerNamespace").toString(), QStringLiteral("quantum-shell-control-center"));
    QCOMPARE(window->property("layer").toInt(), static_cast<int>(LSW::Overlay));
    // On demand: it takes keys once clicked and does not take them from the window being worked in.
    QCOMPARE(window->property("keyboardInteractivity").toInt(), static_cast<int>(LSW::OnDemandKeyboard));
    QCOMPARE(window->property("anchors").toInt(), static_cast<int>(LSW::TopEdge | LSW::RightEdge));
    QCOMPARE(window->property("exclusiveZone").toInt(), 0);
    QCOMPARE(window->width(), 360);
    QVERIFY(window->height() > 0);
    QCOMPARE(window->screen(), QGuiApplication::primaryScreen());

    state_.setOpen(true);
    QCOMPARE(host_->window(), window);
    QVERIFY(!state_.toggle());
    QTRY_VERIFY_WITH_TIMEOUT(host_->window() == nullptr, settleMs);
}

void ControlCenterTest::aClickOnDoNotDisturbFlipsTheNotificationServicesOwnMode()
{
    QWindow* window = openPanel();
    QVERIFY(window != nullptr);
    QVERIFY(!notifications_.notificationDoNotDisturb());
    QVERIFY(click(window, "controlDoNotDisturb"));
    QTRY_VERIFY_WITH_TIMEOUT(notifications_.notificationDoNotDisturb(), settleMs);
    // And back: the row is a switch of the service's property, not a one-way write.
    QVERIFY(click(window, "controlDoNotDisturb"));
    QTRY_VERIFY_WITH_TIMEOUT(!notifications_.notificationDoNotDisturb(), settleMs);
}

void ControlCenterTest::withNoPlayerTheTransportButtonsAskNothing()
{
    QWindow* window = openPanel();
    QVERIFY(window != nullptr);
    QVERIFY(!media_.available());
    auto* text = itemNamed(window, "controlMediaText");
    QVERIFY(text != nullptr);
    QCOMPARE(text->property("text").toString(), QStringLiteral("Nothing playing"));
    // Inert rather than dead: the buttons are there and dimmed, which is the panel saying there is nothing to
    // control. The service refuses a request with no player as well (`media-test` asserts that), so the dimming is
    // the panel's own claim and is asserted as such; clicks on the dimmed buttons are delivered and ask nothing.
    auto* transport = itemNamed(window, "controlTransport");
    QVERIFY(transport != nullptr);
    QCOMPARE(transport->opacity(), 0.4);
    for (const char* name : {"controlPrevious", "controlPlayPause", "controlNext"})
        QVERIFY(click(window, name));
    QTest::qWait(150);
}

void ControlCenterTest::aClickOnTransportReachesTheFollowedPlayer()
{
    const QString connection = QStringLiteral("control-player");
    player_ = std::make_unique<mprisfix::FakeMprisPlayer>(bus_->connect(connection));
    player_->properties = {
        {QStringLiteral("Metadata"), QVariantMap{{QStringLiteral("xesam:title"), QStringLiteral("A song")},
                                                 {QStringLiteral("xesam:artist"), QStringList{QStringLiteral("An artist")}}}},
        {QStringLiteral("PlaybackStatus"), QStringLiteral("Playing")}};
    QVERIFY(player_->own(QStringLiteral("org.mpris.MediaPlayer2.controltest")));
    QTRY_VERIFY_WITH_TIMEOUT(media_.available(), settleMs);

    QWindow* window = openPanel();
    QVERIFY(window != nullptr);
    auto* text = itemNamed(window, "controlMediaText");
    QVERIFY(text != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(text->property("text").toString(), QStringLiteral("Playing: A song — An artist"), settleMs);
    QCOMPARE(itemNamed(window, "controlTransport")->opacity(), 1.0);

    QVERIFY(click(window, "controlPlayPause"));
    QTRY_COMPARE_WITH_TIMEOUT(player_->transportCalls, QStringList{QStringLiteral("PlayPause")}, settleMs);
    QVERIFY(click(window, "controlNext"));
    QVERIFY(click(window, "controlPrevious"));
    QTRY_COMPARE_WITH_TIMEOUT(player_->transportCalls,
                              (QStringList{QStringLiteral("PlayPause"), QStringLiteral("Next"),
                                           QStringLiteral("Previous")}),
                              settleMs);

    // The player leaves the bus, and the panel goes back to saying nothing is playing.
    player_.reset();
    bus_->drop(connection);
    QTRY_VERIFY_WITH_TIMEOUT(!media_.available(), settleMs);
    QTRY_COMPARE_WITH_TIMEOUT(text->property("text").toString(), QStringLiteral("Nothing playing"), settleMs);
    QCOMPARE(itemNamed(window, "controlTransport")->opacity(), 0.4);
}

void ControlCenterTest::theVolumeSectionIsInertWithoutASink()
{
    QWindow* window = openPanel();
    QVERIFY(window != nullptr);
    QVERIFY(!audio_.available());
    auto* value = itemNamed(window, "controlVolumeValue");
    QVERIFY(value != nullptr);
    QCOMPARE(value->property("text").toString(), QStringLiteral("—"));
    // The track and the mute are disabled with nothing to control: a click is not a request that would be refused
    // with a record, it is not sent (the slot's warning check is what would catch one).
    QVERIFY(click(window, "controlVolumeTrack"));
    QVERIFY(click(window, "controlMute"));
    QTest::qWait(150);
}

void ControlCenterTest::theTextAndGeometryRulesAnswerForTheReadingsADaemonSends()
{
    QWindow* window = openPanel();
    QVERIFY(window != nullptr);
    QCOMPARE(call(window, "volumeText", {true, false, 42}).toString(), QStringLiteral("42%"));
    QCOMPARE(call(window, "volumeText", {true, true, 42}).toString(), QStringLiteral("MUTE"));
    QCOMPARE(call(window, "volumeText", {false, false, 42}).toString(), QStringLiteral("—"));
    QCOMPARE(call(window, "fillFor", {true, 42}).toDouble(), 0.42);
    QCOMPARE(call(window, "fillFor", {true, 130}).toDouble(), 1.0);
    QCOMPARE(call(window, "fillFor", {false, 42}).toDouble(), 0.0);
    // A press at x in a track `width` wide asks for that share of the range, clamped to it.
    QCOMPARE(call(window, "percentAt", {150, 300}).toInt(), 50);
    QCOMPARE(call(window, "percentAt", {-20, 300}).toInt(), 0);
    QCOMPARE(call(window, "percentAt", {900, 300}).toInt(), 100);
    QCOMPARE(call(window, "percentAt", {10, 0}).toInt(), 0);
    QCOMPARE(call(window, "mediaText", {false, QString(), QString(), QString()}).toString(),
             QStringLiteral("Nothing playing"));
    QCOMPARE(call(window, "mediaText", {true, QStringLiteral("T"), QString(), QStringLiteral("paused")}).toString(),
             QStringLiteral("Paused: T"));
}

void ControlCenterTest::escapeClosesAndASurfaceTheCompositorClosesLeavesItClosed()
{
    QWindow* window = openPanel();
    QVERIFY(window != nullptr);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(!state_.isOpen(), settleMs);
    QTRY_VERIFY_WITH_TIMEOUT(host_->window() == nullptr, settleMs);

    window = openPanel();
    QVERIFY(window != nullptr);
    // The compositor closing a layer surface is the window becoming invisible; the state has to hear it, or the
    // next toggle would try to close what is gone.
    window->hide();
    QTRY_VERIFY_WITH_TIMEOUT(!state_.isOpen(), settleMs);
    QVERIFY(state_.toggle());
    QTRY_VERIFY_WITH_TIMEOUT(host_->window() != nullptr, settleMs);
}

#include "control_center_test.moc"

void ControlCenterTest::aClickOutsideThePanelClosesItAndTheBackdropGoesWithIt()
{
    QVERIFY(host_->backdropWindow() == nullptr);
    QWindow* window = openPanel();
    QVERIFY(window != nullptr);
    QWindow* backdrop = host_->backdropWindow();
    backdrop::verifyIsABackdrop(backdrop);
    QVERIFY(backdrop != window);

    backdrop::click(backdrop);
    QTRY_VERIFY_WITH_TIMEOUT(!state_.isOpen(), settleMs);
    QTRY_VERIFY_WITH_TIMEOUT(host_->window() == nullptr, settleMs);
    QTRY_VERIFY_WITH_TIMEOUT(host_->backdropWindow() == nullptr, settleMs);
}

void ControlCenterTest::openingOnePanelClosesTheOthers()
{
    quantum::apps::LauncherService launcher(QStringList{}, QStringList{}, QString());
    quantum::app::CalendarService calendar;
    quantum::app::PanelGroup panels(launcher, state_, notifications_, calendar);

    // Each panel, opened while another is up, replaces it: exactly one is open afterwards, and it is the one
    // that was asked for.
    const auto open = [&] { return QList<bool>{launcher.isOpen(), state_.isOpen(), notifications_.notificationHistoryOpen()}; };
    launcher.setOpen(true);
    QCOMPARE(open(), (QList<bool>{true, false, false}));
    state_.setOpen(true);
    QCOMPARE(open(), (QList<bool>{false, true, false}));
    notifications_.setNotificationHistoryOpen(true);
    QCOMPARE(open(), (QList<bool>{false, false, true}));
    launcher.setOpen(true);
    QCOMPARE(open(), (QList<bool>{true, false, false}));
    // Every ordered pair, not only the ones the chain above happens to visit: each panel closing each other one.
    state_.setOpen(true);
    launcher.setOpen(true);
    QCOMPARE(open(), (QList<bool>{true, false, false}));
    notifications_.setNotificationHistoryOpen(true);
    launcher.setOpen(true);
    QCOMPARE(open(), (QList<bool>{true, false, false}));
    notifications_.setNotificationHistoryOpen(true);
    state_.setOpen(true);
    QCOMPARE(open(), (QList<bool>{false, true, false}));
    state_.setOpen(false);
    launcher.setOpen(true);
    state_.setOpen(true);
    QCOMPARE(open(), (QList<bool>{false, true, false}));
    launcher.setOpen(true);

    // Closing one closes nothing else, and one closing is not another opening.
    launcher.setOpen(false);
    QCOMPARE(open(), (QList<bool>{false, false, false}));
    state_.setOpen(true);
    state_.setOpen(false);
    QCOMPARE(open(), (QList<bool>{false, false, false}));
    notifications_.setNotificationHistoryOpen(false);
}

void ControlCenterTest::theCalendarDrawsTheMonthItIsOpenedOnAndMovesByMonth()
{
    quantum::app::CalendarService calendar;
    quantum::app::CalendarHost host(calendar, *engine_, QUrl::fromLocalFile(QStringLiteral(QS_CALENDAR_QML)),
                                    QUrl::fromLocalFile(QStringLiteral(QS_BACKDROP_QML)));
    QVERIFY2(host.ready(), qPrintable(host.componentError()));
    QVERIFY(host.window() == nullptr);

    calendar.setOpen(true);
    QVERIFY(QTest::qWaitFor([&] { return host.window() != nullptr && host.window()->isVisible(); }, settleMs));
    QWindow* window = host.window();
    QCOMPARE(window->property("layerNamespace").toString(), QStringLiteral("quantum-shell-calendar"));
    QCOMPARE(window->property("layer").toInt(), static_cast<int>(QuantumShell::LayerShellWindow::Overlay));
    backdrop::verifyIsABackdrop(host.backdropWindow());

    // The month shown is the one containing today, and the grid is six weeks with today marked exactly once.
    const QDate today = QDate::currentDate();
    QVariant title;
    auto* quick = qobject_cast<QQuickWindow*>(window);
    QVERIFY(quick != nullptr);
    quick->resize(quick->width() + 1, quick->height());
    quick->resize(quick->width() - 1, quick->height());
    QVERIFY(QTest::qWaitFor([&] { return quick->contentItem()->width() > 0; }, settleMs));
    // Visual children are found through `childItems`, which `findChild` does not follow.
    std::function<void(QQuickItem*, const QString&, QList<QQuickItem*>&)> collect =
        [&](QQuickItem* item, const QString& name, QList<QQuickItem*>& out) {
            if (item->objectName() == name)
                out.append(item);
            for (QQuickItem* child : item->childItems())
                collect(child, name, out);
        };
    const auto all = [&](const char* name) {
        QList<QQuickItem*> out;
        collect(quick->contentItem(), QString::fromLatin1(name), out);
        return out;
    };
    const auto findText = [&](const char* name) {
        const QList<QQuickItem*> found = all(name);
        return found.isEmpty() ? nullptr : found.first();
    };
    QQuickItem* titleItem = findText("calendarTitle");
    QVERIFY2(titleItem != nullptr, "the panel has no title");
    const QString expected = QLocale().monthName(today.month(), QLocale::LongFormat) + QLatin1Char(' ') + QString::number(today.year());
    QCOMPARE(titleItem->property("text").toString(), expected);
    const QList<QQuickItem*> days = all("calendarDay");
    QCOMPARE(days.size(), 42);
    int marked = 0;
    for (QQuickItem* day : days)
        marked += day->property("today").toBool() ? 1 : 0;
    QCOMPARE(marked, 1);

    // The next button moves one month on, and today is then not in the grid.
    QQuickItem* next = findText("calendarNext");
    QVERIFY(next != nullptr);
    const QPointF point = next->mapToScene(QPointF(next->width() / 2, next->height() / 2));
    QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, point.toPoint());
    const QDate later = today.addMonths(1);
    const QString expectedLater = QLocale().monthName(later.month(), QLocale::LongFormat) + QLatin1Char(' ') + QString::number(later.year());
    QTRY_COMPARE_WITH_TIMEOUT(titleItem->property("text").toString(), expectedLater, settleMs);
    // Today may still appear, as one of the leading days of the next month's first week, but never as a day of
    // the month shown: those cells are the month's own, and today is not in it.
    for (QQuickItem* day : all("calendarDay")) {
        if (day->property("today").toBool())
            QVERIFY2(!day->property("modelData").toMap().value(QStringLiteral("inMonth")).toBool(),
                     "today is marked inside a month it does not belong to");
    }

    // A click outside closes it, and the backdrop goes with it.
    backdrop::click(host.backdropWindow());
    QTRY_VERIFY_WITH_TIMEOUT(!calendar.isOpen(), settleMs);
    QTRY_VERIFY_WITH_TIMEOUT(host.window() == nullptr, settleMs);
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    previousHandler = qInstallMessageHandler(recordWarnings);
    ControlCenterTest test;
    return QTest::qExec(&test, argc, argv);
}
