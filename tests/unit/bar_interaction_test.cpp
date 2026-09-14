// The bar's interaction, driven through the component the shell ships rather than through a copy of it.
//
// What a click and a wheel *mean* is a property of `qml/Workspaces.qml`, and nothing in C++ can be asked
// about it: a capsule that draws the right workspace and does nothing when it is clicked passes every
// assertion a C++ test can make. So the file the application packs into its resources is loaded into a
// real engine here, over the fake compositor, and the gestures are delivered as real window events —
// `QTest::mouseClick` and `QTest::wheelEvent`, which go through the platform layer the way a person's
// pointer does. The assertions are about what reached niri's end of the socket, so they are about the
// request the person's click produced and not about which QML function was called.
//
// No display and no session: the platform plugin is the offscreen one, and the fake compositor owns the
// socket. The three names a QML file is written against — the import, `NiriService` and `NiriActions` —
// are mirrored below and compared at compile time, on the same rule the service test mirrors the first
// two for.
#include "niri/NiriActions.h"
#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"
#include "niri/NiriQmlModule.h"
#include "niri/NiriService.h"
#include "niri/NiriState.h"

#include "FakeNiriServer.h"
#include "NiriProtocolTestData.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include <memory>
#include <string_view>

using quantum::niri::NiriActions;
using quantum::niri::NiriEventStream;
using quantum::niri::NiriIPC;
using quantum::niri::NiriService;
using quantum::niri::NiriState;

namespace qml = quantum::niri::qml;

namespace {

// The names this test's QML and the shipped component are both written against, mirrored here rather than
// read out of the header. A rename in `src/niri/` does not build until this file agrees, which is where
// the question "who else says `NiriActions`" gets asked. The two below the module's own names are the
// singletons: the component this test loads imports the module and calls both by name, so a rename that
// got past these assertions would still fail — in the engine, at load time — and be reported as an
// unloadable component rather than as a broken rule.
constexpr auto coveredModuleUri = "QuantumShell";
constexpr int coveredModuleMajorVersion = 1;
constexpr int coveredModuleMinorVersion = 0;
constexpr auto coveredServiceTypeName = "NiriService";
constexpr auto coveredActionTypeName = "NiriActions";

static_assert(std::string_view(qml::ModuleUri) == std::string_view(coveredModuleUri),
              "the QML module URI changed: update the mirror above, and every QML file that imports it");
static_assert(qml::ModuleMajorVersion == coveredModuleMajorVersion, "the QML module major version changed");
static_assert(qml::ModuleMinorVersion == coveredModuleMinorVersion, "the QML module minor version changed");
static_assert(std::string_view(qml::ServiceTypeName) == std::string_view(coveredServiceTypeName),
              "the QML service type name changed: update the mirror above, and every binding that reads it");
static_assert(std::string_view(qml::ActionTypeName) == std::string_view(coveredActionTypeName),
              "the QML action type name changed: update the mirror above, and every gesture that calls it");

// The workspaces the compositor reports to this test, as ids: 6 is the focused one, so a click on the
// second capsule (workspace 7) is a click on a workspace the compositor has not focused, and a click on
// the first is the case the bar deliberately does nothing for.
constexpr quint64 firstWorkspaceId = 6;
constexpr quint64 secondWorkspaceId = 7;
constexpr quint64 thirdWorkspaceId = 8;

}  // namespace

class BarInteractionTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void clickingACapsuleFocusesTheWorkspaceItNames();
    void clickingTheFocusedCapsuleAsksTheCompositorForNothing();
    void theWheelMovesThroughNirisOwnWorkspaces();

private:
    // The delegate the strip built for `index`, in the order the model reports: the item a pointer would
    // land on, found by walking what the real component produced rather than by counting pixels.
    QQuickItem* capsuleAt(int index);
    // The point at the middle of `capsule`, in the window's own coordinates — what `QTest` wants.
    QPointF centreOf(QQuickItem* capsule) const;

    int requestsSent() const;
    QString lastRequest() const;

    FakeNiriServer server_;
    NiriIPC requests_;
    NiriEventStream stream_;
    NiriState state_;
    std::unique_ptr<NiriService> service_;
    std::unique_ptr<NiriActions> actions_;
    std::unique_ptr<QQmlEngine> engine_;
    std::unique_ptr<QObject> bar_;
    std::unique_ptr<QQuickWindow> window_;
};

void BarInteractionTest::initTestCase() {
    QVERIFY2(server_.listen(), qPrintable(server_.serverError()));
    server_.setReply(QStringLiteral("EventStream"), QByteArrayLiteral("{\"Ok\":\"Handled\"}"));
    server_.setReply(QStringLiteral("Action"), QByteArrayLiteral("{\"Ok\":\"Handled\"}"));

    // The state the strip draws, fed the way the shell feeds it: an event from the compositor, not a value
    // pushed into the service.
    state_.observe(stream_);

    service_ = std::make_unique<NiriService>(state_, stream_);
    NiriService::registerQmlSingleton(*service_);
    actions_ = std::make_unique<NiriActions>(requests_);
    NiriActions::registerQmlSingleton(*actions_);

    QSignalSpy streaming(&stream_, &NiriEventStream::streaming);
    requests_.connectToCompositor(server_.path());
    stream_.connectToCompositor(server_.path());
    QTRY_VERIFY_WITH_TIMEOUT(streaming.count() == 1, 5000);
    QVERIFY(requests_.isConnected());

    server_.writeRawTo(1, qstest::eventLine(
                              QStringLiteral("WorkspacesChanged"),
                              QJsonObject{{QStringLiteral("workspaces"),
                                           qstest::array({qstest::workspaceObject(
                                                              firstWorkspaceId, 1,
                                                              QStringLiteral("DP-3"), true, true),
                                                          qstest::workspaceObject(
                                                              secondWorkspaceId, 2,
                                                              QStringLiteral("DP-3")),
                                                          qstest::workspaceObject(
                                                              thirdWorkspaceId, 3,
                                                              QStringLiteral("DP-3"))})}}));
    QTRY_COMPARE_WITH_TIMEOUT(service_->workspaces().size(), 3, 5000);

    // The component itself. No copy of the strip exists in this file: `QS_BAR_QML` is the `Workspaces.qml`
    // entry of the list the application's resources are built from, read here from the source tree because
    // that resource set is the application's, and the loading of it is the first assertion — a component
    // that does not resolve fails here rather than silently testing nothing.
    engine_ = std::make_unique<QQmlEngine>();
    QQmlComponent component(engine_.get(), QUrl::fromLocalFile(QStringLiteral(QS_BAR_QML)));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    bar_.reset(component.create());
    QVERIFY2(bar_ != nullptr, qPrintable(component.errorString()));

    // The bar gives the strip its height (`Main.qml` anchors it to fill the window) and its width comes
    // from the strip itself; both are set here for the same reason, so that what the gestures are
    // delivered against is a laid-out component rather than a default one.
    auto* root = qobject_cast<QQuickItem*>(bar_.get());
    QVERIFY(root != nullptr);
    root->setHeight(32);

    window_ = std::make_unique<QQuickWindow>();
    window_->resize(qRound(root->width()), 32);
    root->setParentItem(window_->contentItem());
    window_->show();
    QVERIFY2(QTest::qWaitForWindowExposed(window_.get()), "the window never became exposed");
}

QQuickItem* BarInteractionTest::capsuleAt(int index) {
    auto* root = qobject_cast<QQuickItem*>(bar_.get());
    if (root == nullptr) {
        return nullptr;
    }
    // The id the component gives the row, which is how a pointer's target is found without depending on
    // the order children happen to be declared in.
    QQuickItem* strip = root->findChild<QQuickItem*>(QStringLiteral("strip"));
    if (strip == nullptr) {
        return nullptr;
    }
    const QList<QQuickItem*> capsules = strip->childItems();
    if (index < 0 || index >= capsules.size()) {
        return nullptr;
    }
    return capsules.at(index);
}

QPointF BarInteractionTest::centreOf(QQuickItem* capsule) const {
    return capsule->mapToScene(QPointF(capsule->width() / 2.0, capsule->height() / 2.0));
}

int BarInteractionTest::requestsSent() const {
    return server_.receivedRequests().size();
}

QString BarInteractionTest::lastRequest() const {
    if (server_.receivedRequests().isEmpty()) {
        return QString();
    }
    return server_.receivedRequests().last();
}

void BarInteractionTest::clickingACapsuleFocusesTheWorkspaceItNames() {
    const int before = requestsSent();

    QQuickItem* capsule = capsuleAt(1);
    QVERIFY2(capsule != nullptr, "the strip built no second capsule, so there is nothing to click");

    // The second workspace, by the id the compositor gave it — which is the whole claim: the click reaches
    // the compositor as `FocusWorkspace` on *that* workspace's id and not on its index in the strip.
    QTest::mouseClick(window_.get(), Qt::LeftButton, Qt::NoModifier, centreOf(capsule).toPoint());

    QTRY_VERIFY_WITH_TIMEOUT(requestsSent() > before, 5000);
    QCOMPARE(lastRequest(),
             QStringLiteral(R"({"Action":{"FocusWorkspace":{"reference":{"Id":7}}}})"));
}

void BarInteractionTest::clickingTheFocusedCapsuleAsksTheCompositorForNothing() {
    const int before = requestsSent();

    QQuickItem* capsule = capsuleAt(0);
    QVERIFY2(capsule != nullptr, "the strip built no first capsule, so there is nothing to click");

    // niri resolves the reference and then switches to it, and with `workspace-auto-back-and-forth` — which
    // this project's own session sets — switching to the workspace already focused lands on the previously
    // focused one. So the click is dropped here rather than sent: a bar that moved a person off the
    // workspace they are on when they click the workspace they are on would be worse than one that does
    // nothing. The wait is what makes "nothing" mean anything: a request that was going to be sent would
    // have been answered by now.
    QTest::mouseClick(window_.get(), Qt::LeftButton, Qt::NoModifier, centreOf(capsule).toPoint());
    QTest::qWait(150);

    QCOMPARE(requestsSent(), before);
}

void BarInteractionTest::theWheelMovesThroughNirisOwnWorkspaces() {
    QQuickItem* capsule = capsuleAt(1);
    QVERIFY2(capsule != nullptr, "the strip built no capsule to turn the wheel over");
    const QPointF overStrip = centreOf(capsule);

    // Down is `focus-workspace-down` and up is `focus-workspace-up`: the same pair, in the same direction,
    // that niri's own default config binds to the wheel (`Mod+WheelScrollDown { focus-workspace-down; }`).
    // Which workspace is below is the compositor's answer, so what is asserted here is the request and not
    // a workspace id computed from the strip — the bar does not walk its own model.
    const int beforeDown = requestsSent();
    QTest::wheelEvent(window_.get(), overStrip, QPoint(0, -120));
    QTRY_VERIFY_WITH_TIMEOUT(requestsSent() > beforeDown, 5000);
    QCOMPARE(lastRequest(), QStringLiteral(R"({"Action":{"FocusWorkspaceDown":{}}})"));

    const int beforeUp = requestsSent();
    QTest::wheelEvent(window_.get(), overStrip, QPoint(0, 120));
    QTRY_VERIFY_WITH_TIMEOUT(requestsSent() > beforeUp, 5000);
    QCOMPARE(lastRequest(), QStringLiteral(R"({"Action":{"FocusWorkspaceUp":{}}})"));
}

// LeakSanitizer's suppressions for this binary, and this binary only.
//
// This is the one test that loads Qt Quick: creating a window and loading the bar's QML pulls in font
// configuration (fontconfig, pango, cairo), a GL driver and Qt's own scene-graph render context, each of
// which holds process-lifetime allocations that LeakSanitizer reports as leaks at exit. They are not this
// project's to free, and without these rules the sanitizer run is red for a reason that says nothing
// about this repository — a check that is always red is a check nobody reads. No rule names a symbol in
// `src/`, so a leak in the shell's own code still fails this run, which is why the list is libraries
// rather than `detect_leaks=0`.
//
// The hook rather than `LSAN_OPTIONS=suppressions=<path>`, which is parsed by splitting on spaces: this
// checkout lives under a path with one in it, so an environment variable cannot name the file. The
// function only exists in a sanitized build, which is the only build whose runtime looks for it.
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
extern "C" const char* __lsan_default_suppressions();
extern "C" const char* __lsan_default_suppressions() {
    return "leak:libfontconfig\n"
           "leak:libpango\n"
           "leak:libpangocairo\n"
           "leak:libcairo\n"
           "leak:libnvidia\n"
           "leak:libEGL\n"
           "leak:libGLX\n"
           "leak:libgbm\n"
           "leak:libdbus-1\n"
           "leak:QSGDefaultRenderContext\n"
           "leak:QSGRenderContext\n";
}
#endif

// The platform plugin and the scene graph backend, set here rather than in the test's environment: the
// slot-order checks run this binary directly, once per slot and in shuffled order, and inherit no ctest
// environment properties. A value the environment already carries wins, so a person debugging this on a
// session keeps their choice.
//
// The backend is `software` for two measured reasons. Nothing asserted here is a pixel — the test reads
// what the gestures asked the compositor for — so the renderer is not part of the claim. And initialising
// GL under the offscreen plugin loads a driver (NVIDIA's here) whose process-lifetime allocations
// LeakSanitizer reports with no module name on the stack, which no `leak:` rule can match; the software
// backend does not load it, and the same run under AddressSanitizer goes from red to green and from
// 1178 ms to 306 ms by asking for it. The GL path's remaining third-party leaks are suppressed in the
// list below, which stays because fontconfig's are still there either way.
int main(int argc, char* argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")) {
        qputenv("QT_QUICK_BACKEND", QByteArrayLiteral("software"));
    }
    QGuiApplication application(argc, argv);
    BarInteractionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "bar_interaction_test.moc"
