// Proves the rendering ratio a Qt Quick window reports against a real compositor, at the four scales the
// risk register asks for.
//
// Nothing here is a test double: the compositor is real, started nested in the session and without
// `--session`, which niri's own help says to use for a non-main instance; the window is a real Qt Quick
// window mapped on it. What the test pins is the one number that decides how the bar is rendered — the
// window's own `effectiveDevicePixelRatio()` — because the *screen's* `devicePixelRatio()` is the protocol's
// integer `wl_output.scale`, which rounds 1.25 and 1.5 both up to 2. Reading the screen's number looks
// exactly like the fractional scale being rounded up when it is not, and a code change made on that reading
// would add scale code the shell has never needed.
//
// It is visible on screen (the nested compositor is a window) and needs a session to nest in, so it is
// registered only when QS_NIRI_SCALE_TESTS is set alongside $NIRI_SOCKET.
#include "NestedCompositor.h"

#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQuickWindow>
#include <QScreen>
#include <QStandardPaths>
#include <QThread>
#include <QTest>

#include <optional>
#include <utility>

namespace {

// The scales the risk register asks for, measured in the order written: the two that are integers are
// measured too, because a matrix that only covers the fractional values cannot tell a working stack from one
// that rounds.
constexpr double scales[] = {1.0, 1.25, 1.5, 2.0};

// The config for the compositor this test starts: nothing but a comment, the way `NestedCompositor.h` does
// it, so the instance cannot inherit anything from the session it runs in.
constexpr auto scaleCompositorConfig =
    "// The compositor this file configures is started by a Quantum Shell live test.\n";

}  // namespace

class NiriLiveScaleTest : public QObject {
    Q_OBJECT

public:
    // The compositor and the socket to reach it with are set up before the application exists, so they are
    // handed in rather than found here.
    NiriLiveScaleTest(QString nestedSocket, QString outputName, QObject* parent = nullptr)
        : QObject(parent), nestedSocket_(std::move(nestedSocket)), outputName_(std::move(outputName)) {}

    // A compositor that could not be started is the reason the test does not run, and it is answered rather
    // than swallowed: the skip ctest reports is the reason.
    const QString& startupFailure() const { return startupFailure_; }
    void setStartupFailure(QString reason) { startupFailure_ = std::move(reason); }

private slots:
    void initTestCase();
    void theWindowReportsTheOutputScaleExactly();
    void theScreenReportsTheProtocolInteger();

private:
    // The scale the compositor says the output is at, read from its own answer rather than assumed from the
    // request: `niri msg output <name> scale <n>` changes the output and a client that assumed the change
    // had happened would be measuring the request rather than the compositor.
    std::optional<double> compositorScale() const;

    QString nestedSocket_;
    QString outputName_;
    QString startupFailure_;
};

void NiriLiveScaleTest::initTestCase() {
    if (!startupFailure_.isEmpty()) {
        QSKIP(qPrintable(startupFailure_));
    }
}

std::optional<double> NiriLiveScaleTest::compositorScale() const {
    QProcess request;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("NIRI_SOCKET"), nestedSocket_);
    request.setProcessEnvironment(environment);
    request.start(QStringLiteral("niri"), QStringList{QStringLiteral("msg"), QStringLiteral("--json"),
                                                       QStringLiteral("outputs")});
    if (!request.waitForFinished(5000)) {
        return std::nullopt;
    }

    const QJsonObject outputs =
        QJsonDocument::fromJson(request.readAllStandardOutput()).object();
    const QJsonValue value = outputs.value(outputName_);
    if (!value.isObject()) {
        return std::nullopt;
    }
    return value.toObject().value(QStringLiteral("logical")).toObject()
        .value(QStringLiteral("scale")).toDouble(-1.0);
}

void NiriLiveScaleTest::theWindowReportsTheOutputScaleExactly() {
    // One window per scale, and it is closed between them: a surface's preferred scale arrives when the
    // surface binds, so a window kept from the last scale would be one reporting the scale it was created at.
    for (const double scale : scales) {
        QProcess request;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("NIRI_SOCKET"), nestedSocket_);
        request.setProcessEnvironment(environment);
        request.start(QStringLiteral("niri"),
                      QStringList{QStringLiteral("msg"), QStringLiteral("output"), outputName_,
                                  QStringLiteral("scale"), QString::number(scale)});
        QVERIFY2(request.waitForFinished(5000), "the compositor did not answer a scale request");

        // The change is waited for in the compositor's own word, so the ratio below is measured against the
        // output the compositor says it is driving rather than against the request that was sent.
        QTRY_COMPARE_WITH_TIMEOUT(compositorScale().value_or(-1.0), scale, 5000);

        QQuickWindow window;
        window.resize(200, 32);
        window.show();
        QTRY_COMPARE_WITH_TIMEOUT(window.effectiveDevicePixelRatio(), scale, 5000);
        window.close();
    }
}

void NiriLiveScaleTest::theScreenReportsTheProtocolInteger() {
    // The weaker of the two claims, stated for the reason it is weaker rather than because it is all that
    // could be asserted: the *screen's* ratio is the protocol's integer `wl_output.scale`, and niri's own
    // policy for rounding a fractional scale up to it is niri's to change. What is asserted is the
    // relationship the protocol guarantees — the screen is never below the window it scales — and the exact
    // number is the test above's.
    QQuickWindow window;
    window.resize(200, 32);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isVisible(), 5000);
    QVERIFY2(window.screen() != nullptr, "the window mapped on no screen");
    QVERIFY2(window.screen()->devicePixelRatio() >= window.effectiveDevicePixelRatio(),
             "the screen's ratio is below the window's");
    window.close();
}

#include "niri_live_scale_test.moc"

int main(int argc, char* argv[]) {
    // The compositor is started before the application exists, because the display a window is mapped on is
    // chosen when the platform plugin is created and that happens inside QGuiApplication's constructor. So
    // the window is mapped on this test's own compositor rather than on the session's output, which the test
    // never touches, and the reason it could not be started is handed to the test rather than swallowed.
    const QString niriPath = QStandardPaths::findExecutable(QStringLiteral("niri"));
    const QString sessionSocket = quantum::niri::niriSocketPath();

    QString startupFailure;
    QString nestedSocket;
    QString outputName;
    if (niriPath.isEmpty()) {
        startupFailure = QStringLiteral("no niri on PATH: this test starts a compositor of its own to set "
                                         "the scale of");
    } else if (qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()) {
        startupFailure = QStringLiteral("no Wayland display to nest a compositor in; a compositor of one's "
                                         "own needs one it can run in");
    } else if (sessionSocket.isEmpty()) {
        startupFailure = QStringLiteral("$NIRI_SOCKET is not set, so this test cannot tell its own "
                                         "compositor from the session's");
    } else {
        // Started by hand rather than by `NestedNiri` (support/NestedCompositor.h), which waits with
        // `QTest::qWait` and needs an application to spin the event loop of. The job is the same one — a
        // nested niri, found by niri's own socket naming rule — and the config is a comment, so the instance
        // inherits nothing from the session it runs in.
        NestedNiri nested;
        QString reason;
        if (!nested.start(niriPath, QString(scaleCompositorConfig), sessionSocket, &reason)) {
            startupFailure = reason;
        } else {
            nestedSocket = nested.socket();

            // The output the nested instance created, read from its own answer; a nested niri names it after
            // the backend it is running on, and it is the only one.
            QProcess request;
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("NIRI_SOCKET"), nestedSocket);
            request.setProcessEnvironment(environment);
            request.start(QStringLiteral("niri"), QStringList{QStringLiteral("msg"),
                                                               QStringLiteral("--json"),
                                                               QStringLiteral("outputs")});
            if (!request.waitForFinished(5000)) {
                startupFailure = QStringLiteral("the compositor did not answer an Outputs request");
            } else {
                const QJsonObject outputs =
                    QJsonDocument::fromJson(request.readAllStandardOutput()).object();
                outputName = outputs.constBegin().key();
            }

            // The display the window is mapped on, set before the platform plugin is created. The compositor
            // itself is stopped when `main` returns, which is the test's end: it is owned by this function
            // and not by the test object, because the test object's lifetime is QTest's.
            qputenv("WAYLAND_DISPLAY", nested.waylandDisplay().toUtf8());
        }
    }

    QGuiApplication app(argc, argv);
    NiriLiveScaleTest test(nestedSocket, outputName);
    test.setStartupFailure(startupFailure);
    return QTest::qExec(&test, argc, argv);
}
