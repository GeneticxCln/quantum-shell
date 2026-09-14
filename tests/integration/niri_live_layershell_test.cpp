// Integration test: the bar is a real layer surface on niri, and it asked for what it says it asked for.
//
// Two sources, deliberately kept apart, because they answer different questions and each one alone can be
// satisfied while the other is false.
//
//   * `niri`'s own layer list — a `Layers` request on the IPC socket — reports a surface's namespace, its
//     output, its layer and its keyboard interactivity. This is the compositor's answer, and it is what
//     proves the surface was accepted: a shell that sent a layer surface the compositor refused would
//     appear on the wire and in no list.
//   * The `niri` layer list reports **nothing about anchors or the exclusive zone**, because the protocol
//     has no request for a client to read back what it set. Those two therefore come from the client's own
//     protocol traffic, which the test captures by starting the shell with WAYLAND_DEBUG=1 and reading the
//     requests libwayland prints. That is the same wire the compositor sees, and it is the only place the
//     anchors and the exclusive zone exist at all.
//
// A third property is checked on that wire because it is the one this test was written after getting
// wrong: the compositor's initial configure has to be acknowledged before any buffer is attached to the
// surface, and niri answers a client that gets that order wrong by disconnecting it.
//
// Not registered with ctest by default: this test maps a bar on screen, which reserves the top of the
// output and moves the tiling area while it runs, so it changes the desktop of whoever started it. It
// needs QS_NIRI_SESSION_TESTS as well as $NIRI_SOCKET — the same opt-in the other acting test takes — and
// it takes the same resource lock, so a parallel run never has two tests rearranging the desktop at once.
//
// Every expectation below is written out as a literal rather than read from the QML, on purpose. These are
// the values `qml/Main.qml` declares, and pinning them here is what makes a change there a failing test
// here rather than a silent change of the shell's frozen namespace or of the strip it reserves.
#include "ipc/IPCProtocol.h"
#include "niri/NiriIPC.h"
#include "niri/NiriProtocol.h"

#include "SnapshotReconcile.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <optional>

using quantum::niri::NiriIPC;
using quantum::niri::Reply;
using quantum::niri::niriSocketPath;

namespace {

// What the bar asks for when nothing is configured: the schema's own defaults
// (`src/config/ConfigSchema.h`), which `qml/Main.qml` binds to and which this test therefore mirrors —
// layerNamespace, layer, anchors (TopEdge|LeftEdge|RightEdge = 1|4|8 = 13), keyboardInteractivity
// (NoKeyboard = 0), height and exclusiveZone (32). The width is 0 because the bar is anchored to both the
// left and right edges, and a stretched axis must ask the compositor for zero rather than name a size.
//
// These literals only hold because the shell is started with an XDG_CONFIG_HOME of its own that has no
// configuration in it (see `startShell`). Reading the values from a configuration the person running this
// test happens to have would make every assertion here depend on their desktop rather than on the build.
constexpr auto expectedNamespace = "quantum-shell-bar";
constexpr auto expectedLayer = "Top";
constexpr auto expectedKeyboardInteractivity = "None";
constexpr int expectedAnchor = 13;
constexpr int expectedExclusiveZone = 32;
constexpr int expectedHeight = 32;
constexpr int expectedLayerArgument = 2;        // zwlr_layer_shell_v1.layer.top
constexpr int expectedKeyboardArgument = 0;     // keyboard_interactivity.none

// How long to wait for the shell to put its surface on screen. The measured start-to-mapped time on this
// machine is well under a second; the margin is for a compositor busy placing a new layer surface.
constexpr int appearanceTimeoutMs = 20000;

// How long a fact the shell produces at start-up is waited for before it is judged absent: the abstract
// socket being bound, and the first buffer being attached. Both are things the shell does rather than things
// it might not do, so this is a budget for a slow machine rather than a grace period for a failing one — and
// the wait ends as soon as the fact is there, which is what keeps a busy machine from being read as a shell
// that never painted.
constexpr int startUpFactTimeoutMs = 10000;

// The environment the shell needs to load the shell-integration plugin Qt looks up by key. Both paths
// come from the build — tests/CMakeLists.txt defines them — so the test runs against the binary and
// plugin it was built beside rather than against whatever is installed. A missing definition is a
// compile error where they are used, which is a better guard than an #error here: this file has Q_OBJECT
// in it, and an #error that moc's own preprocessor decides is active makes moc emit nothing at all, so
// the symptom is an undefined vtable rather than the message.

// The reply is caught into storage the handler shares ownership of, rather than into a local the handler
// refers to. A request that times out returns while its handler is still registered with the client —
// `NiriIPC` stores it until the reply arrives, and a compositor that is slow or busy can answer after the
// wait has given up — so a reply arriving late used to be written through a reference to a frame that no
// longer existed. That is not a hypothetical: it crashed this test twice, once as a corrupted stack canary
// reported as `__stack_chk_fail` inside glib's main-context iteration and once as a segmentation fault in
// `QString::operator=` called from the handler. Sharing the storage means a late reply writes into something
// that is still alive and is discarded unread, because the caller has already stopped waiting.
std::optional<Reply> request(NiriIPC& client, QStringView name, int timeoutMs = 5000) {
    auto received = std::make_shared<std::optional<Reply>>();
    client.send(name, [received](const Reply& reply) { *received = reply; });
    if (!QTest::qWaitFor([received] { return received->has_value(); }, timeoutMs)) {
        return std::nullopt;
    }
    return *received;
}

// The layer surfaces the compositor reports, keyed by namespace: every entry whose namespace is the one
// under test. Returned as a list so that a second bar appearing with the same namespace is visible rather
// than collapsed into the first match.
QList<QJsonObject> layersNamed(const QJsonArray& layers, const QString& name) {
    QList<QJsonObject> found;
    for (const QJsonValue& value : layers) {
        const QJsonObject layer = value.toObject();
        if (layer.value(QStringLiteral("namespace")).toString() == name) {
            found.append(layer);
        }
    }
    return found;
}

// The layer surface a request line refers to, and the wl_surface it was created on. Parsed from the
// get_layer_surface call rather than assumed, so the checks below follow the objects this run actually
// used instead of an id from a previous one.
struct LayerObjects {
    QString layerSurface;  // e.g. zwlr_layer_surface_v1#36
    QString wlSurface;     // e.g. wl_surface#34
    QString output;        // "nil" when the client let the compositor choose
    int layer = -1;
    QString nameSpace;
};

// The requests one shell run sent for its layer surface. Every field is -1 or empty when the run never
// sent that request, which is what makes a missing request a failed assertion rather than a default that
// happens to match.
struct LayerRequests {
    LayerObjects objects;
    int requestedWidth = -1;
    int requestedHeight = -1;
    int anchor = -1;
    int exclusiveZone = -1;
    int keyboardInteractivity = -1;
    bool acked = false;
    qsizetype ackIndex = -1;
    qsizetype firstAttachIndex = -1;

    bool sawGetLayerSurface() const { return objects.layer != -1; }
};

// Reads libwayland's request lines out of a WAYLAND_DEBUG transcript. Lines look like
//
//   [18:36:07.880619] {Default Queue}  -> zwlr_layer_surface_v1#36.set_size(0, 32)
//
// and only the `-> ` half is a request; the rest are events from the compositor.
LayerRequests parseRequests(const QString& transcript) {
    static const QRegularExpression requestLine(QStringLiteral(R"(-> \s*(\S.*)$)"));
    // An ordinary literal rather than a raw string, and not by preference: `moc` reads this file too, and
    // it does not honour a raw string's custom delimiter. It sees `R"` and ends the string at the first
    // `)"`, which this pattern contains — the rest of the file is then lexed as garbage and moc finds no
    // Q_OBJECT in it at all, so the symptom is an undefined vtable rather than a bad pattern.
    static const QRegularExpression getLayerSurface(QStringLiteral(
        "zwlr_layer_shell_v1#\\d+\\.get_layer_surface\\(new id (zwlr_layer_surface_v1#\\d+), "
        "(wl_surface#\\d+), (\\S+), (\\d+), \"([^\"]*)\"\\)"));
    static const QRegularExpression setSize(
        QStringLiteral(R"(zwlr_layer_surface_v1#\d+\.set_size\((\d+), (\d+)\))"));
    static const QRegularExpression setAnchor(
        QStringLiteral(R"(zwlr_layer_surface_v1#\d+\.set_anchor\((\d+)\))"));
    static const QRegularExpression setExclusiveZone(
        QStringLiteral(R"(zwlr_layer_surface_v1#\d+\.set_exclusive_zone\((-?\d+)\))"));
    static const QRegularExpression setKeyboard(
        QStringLiteral(R"(zwlr_layer_surface_v1#\d+\.set_keyboard_interactivity\((\d+)\))"));
    static const QRegularExpression ackConfigure(
        QStringLiteral(R"(zwlr_layer_surface_v1#\d+\.ack_configure\((\d+)\))"));
    static const QRegularExpression attach(QStringLiteral(R"(wl_surface#\d+\.attach\()"));

    LayerRequests parsed;
    const QStringList lines = transcript.split(QLatin1Char('\n'));

    for (qsizetype index = 0; index < lines.size(); ++index) {
        const QRegularExpressionMatch request = requestLine.match(lines.at(index));
        if (!request.hasMatch()) {
            continue;
        }
        const QString text = request.captured(1);

        if (const auto match = getLayerSurface.match(text); match.hasMatch()) {
            parsed.objects.layerSurface = match.captured(1);
            parsed.objects.wlSurface = match.captured(2);
            parsed.objects.output = match.captured(3);
            parsed.objects.layer = match.captured(4).toInt();
            parsed.objects.nameSpace = match.captured(5);
            continue;
        }
        if (const auto match = setSize.match(text); match.hasMatch()) {
            parsed.requestedWidth = match.captured(1).toInt();
            parsed.requestedHeight = match.captured(2).toInt();
            continue;
        }
        if (const auto match = setAnchor.match(text); match.hasMatch()) {
            parsed.anchor = match.captured(1).toInt();
            continue;
        }
        if (const auto match = setExclusiveZone.match(text); match.hasMatch()) {
            parsed.exclusiveZone = match.captured(1).toInt();
            continue;
        }
        if (const auto match = setKeyboard.match(text); match.hasMatch()) {
            parsed.keyboardInteractivity = match.captured(1).toInt();
            continue;
        }
        // The initial configure and the first buffer are ordered against each other, so the first of each
        // is what matters and neither is overwritten by a later one.
        if (!parsed.acked) {
            if (const auto match = ackConfigure.match(text); match.hasMatch()) {
                parsed.acked = true;
                parsed.ackIndex = index;
                continue;
            }
        }
        if (parsed.firstAttachIndex == -1 && attach.match(text).hasMatch()) {
            parsed.firstAttachIndex = index;
        }
    }

    return parsed;
}

// The frozen abstract socket name as an address. The leading NUL is what makes it abstract rather than a
// filesystem entry, and /proc prints that NUL as '@' — one of them, exactly as `ss -x` does.
QString abstractSocketRow()
{
    return QStringLiteral("@%1").arg(QString::fromLatin1(quantum::ipc::SocketName));
}

// The socket inode the kernel has bound to that name, or nothing when no process holds it.
std::optional<qint64> boundSocketInode()
{
    QFile table(QStringLiteral("/proc/net/unix"));
    if (!table.open(QIODevice::ReadOnly))
        return std::nullopt;

    const QList<QByteArray> lines = table.readAll().split('\n');
    for (const QByteArray& line : lines) {
        const QList<QByteArray> fields = line.simplified().split(' ');
        // Eight columns: the counters, the flags, the type and state, the inode and the path (`Num RefCount
        // Protocol Flags Type St Inode Path`). A row with no path is a connected socket rather than a listening
        // one, which is why the last field is compared rather than trusted to exist.
        if (fields.size() < 8)
            continue;
        if (fields.constLast() == abstractSocketRow().toUtf8())
            return fields.at(6).toLongLong();
    }
    return std::nullopt;
}

// One workspace as a line of text, from either source, so that the two can be compared: niri writes
// `is_focused` where the shell's state writes `isFocused`, and niri states an id as a number where the shell
// passes it as text — an id is passed as text because a random 64-bit id does not survive QML's double.
QString describeWorkspace(const QJsonObject& workspace, bool fromCompositor)
{
    const QString id = fromCompositor
                           ? QString::number(workspace.value(QStringLiteral("id")).toInteger())
                           : workspace.value(QStringLiteral("id")).toString();
    return QStringLiteral("%1 idx=%2 focused=%3 active=%4")
        .arg(id)
        .arg(workspace.value(QStringLiteral("idx")).toInt())
        .arg(workspace.value(fromCompositor ? QStringLiteral("is_focused") : QStringLiteral("isFocused"))
                 .toBool())
        .arg(workspace.value(fromCompositor ? QStringLiteral("is_active") : QStringLiteral("isActive"))
                 .toBool());
}

// Adds what the two lists of workspaces disagree about, to a `Disagreement` the caller owns so that the checks
// which are not a comparison — whether the shell reports itself connected at all — land in the same
// first-disagreement rather than in a second one nobody reads.
void noteWorkspaceDisagreements(qstest::Disagreement& disagreement, const QJsonArray& fromShell,
                                const QJsonArray& fromCompositor)
{
    QStringList shellLines;
    QStringList compositorLines;
    for (const QJsonValue& value : fromShell) {
        shellLines.append(describeWorkspace(value.toObject(), false));
    }
    for (const QJsonValue& value : fromCompositor) {
        compositorLines.append(describeWorkspace(value.toObject(), true));
    }
    shellLines.sort();
    compositorLines.sort();

    // Two empty lists agree and prove nothing, so the shell reporting none is a disagreement in its own right
    // rather than an agreement to be reported as one.
    disagreement.check(QStringLiteral("the shell reported no workspaces, so agreeing with a compositor that "
                                      "has none proves nothing"),
                       !shellLines.isEmpty());
    disagreement.expectEqual(QStringLiteral("the workspaces the shell reports"), shellLines, compositorLines);
}

QString describe(const LayerRequests& requests) {
    return QStringLiteral("get_layer_surface(layer=%1, namespace=%2), set_size(%3, %4), set_anchor(%5), "
                          "set_exclusive_zone(%6), set_keyboard_interactivity(%7), acked=%8, "
                          "first attach at line %9")
        .arg(requests.objects.layer)
        .arg(requests.objects.nameSpace)
        .arg(requests.requestedWidth)
        .arg(requests.requestedHeight)
        .arg(requests.anchor)
        .arg(requests.exclusiveZone)
        .arg(requests.keyboardInteractivity)
        .arg(requests.acked ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(requests.firstAttachIndex);
}

}  // namespace

class NiriLiveLayerShellTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    // Stops the shell between cases, so an assertion that fails cannot leave a bar on the desktop of
    // whoever is using it.
    void cleanup();

    void theBarAppearsInTheCompositorsLayerListAndDisappearsWithTheShell();
    void theBarAsksForItsAnchorsExclusiveZoneAndKeyboardInteractivity();
    void theConfiguredHeightAndNamespaceAreWhatTheCompositorIsGiven();
    void theInitialConfigureIsAcknowledgedBeforeAnyBufferIsAttached();
    void theFrozenSocketNameIsTheOneTheShellBound();
    void theShellAnswersQsctlOverItsOwnSocket();
    void qsctlTogglesTheBarOutOfTheCompositorsLayerList();

private:
    // Starts the shell with WAYLAND_DEBUG on and waits for its surface to reach the compositor, reading its
    // configuration from `configHome` — empty means the empty one, so every value is a schema default — and
    // looking for the surface under `nameSpace`, likewise empty for the default. Records why when it
    // cannot, so the failure names the step rather than the symptom.
    bool startShell(const QString& configHome = QString(), const QString& nameSpace = QString());
    void stopShell();
    // The compositor's layer list, or an empty optional when it did not answer.
    std::optional<QJsonArray> compositorLayers(int timeoutMs = 5000);
    // Waits for this shell's surface to be listed, or to be gone again.
    bool waitForBarListed(bool listed);
    // The transcript libwayland printed for the current run.
    const QString& transcript() const { return transcript_; }

    // Runs the built `qsctl` and returns its standard output, with its exit code in `exitCode` and its
    // standard error in `qsctlError()`. A process that never finished is reported through `startFailure_`,
    // so a failure names the step rather than the symptom.
    QString runQsctl(const QStringList& arguments, int* exitCode);
    const QString& qsctlError() const { return qsctlError_; }

    // One reading of both sides of the IPC comparison: what the shell reports about its own state over its
    // socket, and what the compositor says about the same workspaces. A client that failed, or a compositor
    // that did not answer, comes back as a reading that was not taken rather than as a disagreement.
    qstest::Reading readStateAgainstCompositor();

    NiriIPC client_;
    QProcess shell_;
    // Handed to the shell as its XDG_CONFIG_HOME, so the run is configured by nothing at all.
    QTemporaryDir configHome_;
    // The namespace the current run is expected to appear under. A member rather than a constant because
    // the case below runs the shell against a configuration that names a different one, and the wait for
    // the surface has to know which name it is waiting for.
    QString barNamespace_;
    QString transcript_;
    QString transportFailure_;
    QString startFailure_;
    QString qsctlError_;
    // How many workspaces the shell reported on the last reading, so a successful run can say what it
    // agreed about rather than only that it agreed.
    int shellWorkspaces_ = 0;
};

void NiriLiveLayerShellTest::initTestCase() {
    if (niriSocketPath().isEmpty()) {
        QSKIP("$NIRI_SOCKET is not set: this test needs a running niri session");
    }
    if (!QFile::exists(QStringLiteral(QS_SHELL_BINARY))) {
        QSKIP(qPrintable(QStringLiteral("%1 does not exist: this test needs the shell built")
                             .arg(QStringLiteral(QS_SHELL_BINARY))));
    }
    // One of the cases starts the shell against a configuration of its own under a scratch directory; the
    // rest start it against this empty one. Either way it has to be a directory that exists, or the shell
    // would fail to read a configuration it was told to look for.
    if (!configHome_.isValid()) {
        QSKIP("a scratch directory for the shell's XDG_CONFIG_HOME could not be created");
    }

    connect(&shell_, &QProcess::readyReadStandardError, this,
            [this] { transcript_ += QString::fromUtf8(shell_.readAllStandardError()); });
    connect(&shell_, &QProcess::readyReadStandardOutput, this,
            [this] { shell_.readAllStandardOutput(); });
    // Started before the first request so the failure text says what happened rather than only that
    // something did.
    connect(&shell_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (startFailure_.isEmpty()) {
            startFailure_ = QStringLiteral("QProcess error %1: %2")
                                .arg(static_cast<int>(error))
                                .arg(shell_.errorString());
        }
    });

    QSignalSpy requestConnected(&client_, &NiriIPC::connected);
    connect(&client_, &NiriIPC::transportError, this,
            [this](const QString& reason) { transportFailure_ = reason; });
    client_.connectToCompositor();
    QTRY_VERIFY_WITH_TIMEOUT(requestConnected.count() == 1 || !transportFailure_.isEmpty(), 5000);
    QVERIFY2(requestConnected.count() == 1,
             qPrintable(QStringLiteral("no request connection to %1: %2")
                            .arg(niriSocketPath(), transportFailure_)));
}

void NiriLiveLayerShellTest::cleanup() {
    stopShell();
}

bool NiriLiveLayerShellTest::startShell(const QString& configHome, const QString& nameSpace) {
    transcript_.clear();
    startFailure_.clear();
    barNamespace_ = nameSpace.isEmpty() ? QString::fromLatin1(expectedNamespace) : nameSpace;
    const QString configPath = configHome.isEmpty() ? configHome_.path() : configHome;

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // An empty XDG_CONFIG_HOME, so the shell reads no configuration file and every value is the schema's
    // default — the same one this file's expectations mirror. Without it this test would read whatever
    // config.toml the person running it has, and a bar height set there would fail an assertion about the
    // build rather than about their configuration.
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), configPath);
    environment.insert(QStringLiteral("QT_WAYLAND_SHELL_INTEGRATION"), QStringLiteral("quantum-shell"));
    // Any value in QT_PLUGIN_PATH is additional to Qt's own plugin path, so this points Qt at the shell
    // integration built in this tree without hiding Qt's platform plugins.
    environment.insert(QStringLiteral("QT_PLUGIN_PATH"), QStringLiteral(QS_SHELL_PLUGIN_PATH));
    // libwayland prints every request to stderr, which is how the anchors and the exclusive zone are read
    // back: niri's layer list does not report them and no request reads them.
    environment.insert(QStringLiteral("WAYLAND_DEBUG"), QStringLiteral("1"));
    // The abstract socket name is per user rather than per process, so a process already holding it would take
    // this shell's IPC away from it: the shell would log a refusal and carry on — a bar is more useful than an
    // exit — and every `qsctl` below would then be answered by *that* shell while this test asserted about this
    // one. It is checked here rather than left to show up as a puzzling assertion failure.
    if (boundSocketInode().has_value()) {
        startFailure_ = QStringLiteral(
            "%1 is already bound by another process, which is usually another Quantum Shell; this test "
            "starts its own shell and cannot tell the two apart")
                            .arg(abstractSocketRow());
        return false;
    }
    shell_.setProcessEnvironment(environment);
    shell_.setProcessChannelMode(QProcess::SeparateChannels);

    shell_.start(QStringLiteral(QS_SHELL_BINARY), {});
    if (!shell_.waitForStarted(5000)) {
        startFailure_ = QStringLiteral("the shell did not start: %1").arg(shell_.errorString());
        return false;
    }

    if (!waitForBarListed(true)) {
        stopShell();
        startFailure_ = QStringLiteral(
            "the shell ran but its surface never appeared in the compositor's layer list as \"%1\" "
            "(shell state: %2; %3)")
                            .arg(barNamespace_,
                                 shell_.state() == QProcess::Running ? QStringLiteral("running")
                                                                     : QStringLiteral("exited"))
                            .arg(transcript_.right(2000));
        return false;
    }

    // And the socket, because "the shell is up" is not one thing: its surface reaches the compositor and its
    // IPC server binds the abstract name independently of each other, and nothing orders them. Waiting only
    // for the surface made every `qsctl` assertion below a race the test could lose — `qsctl` exits 3, having
    // been unable to reach a socket that was a moment away from existing, and the failure then reads as a
    // wrong answer from the shell rather than as a shell that had not finished starting. Measured on this
    // machine the bind follows the surface by a few milliseconds; the budget is for a machine where it does
    // not.
    QElapsedTimer bindClock;
    bindClock.start();
    while (!boundSocketInode().has_value() && bindClock.elapsed() < startUpFactTimeoutMs) {
        QTest::qWait(50);
    }
    if (!boundSocketInode().has_value()) {
        const qint64 waited = bindClock.elapsed();
        stopShell();
        startFailure_ = QStringLiteral(
            "the shell's surface appeared but after %1 ms it had nothing bound at %2, so nothing can be "
            "asked of it over IPC")
                            .arg(waited)
                            .arg(abstractSocketRow());
        return false;
    }
    // Said when the wait actually waited, which is the evidence that the two start-up steps are unordered
    // rather than the surface merely happening to come second: a shell whose socket is always bound before its
    // bar is listed would never print this.
    if (bindClock.elapsed() > 0) {
        qInfo("the shell's surface was listed before its IPC socket existed; the socket followed by %lld ms",
              static_cast<long long>(bindClock.elapsed()));
    }
    return true;
}

void NiriLiveLayerShellTest::stopShell() {
    if (shell_.state() == QProcess::NotRunning) {
        return;
    }
    shell_.terminate();
    if (!shell_.waitForFinished(5000)) {
        shell_.kill();
        shell_.waitForFinished(5000);
    }
    // Whatever arrived between the last readyRead and the exit.
    transcript_ += QString::fromUtf8(shell_.readAllStandardError());
}

std::optional<QJsonArray> NiriLiveLayerShellTest::compositorLayers(int timeoutMs) {
    const std::optional<Reply> reply = request(client_, QStringLiteral("Layers"), timeoutMs);
    if (!reply.has_value()) {
        return std::nullopt;
    }
    if (!reply->isOk()) {
        return std::nullopt;
    }
    return reply->variant(QStringLiteral("Layers")).toArray();
}

bool NiriLiveLayerShellTest::waitForBarListed(bool listed) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < appearanceTimeoutMs) {
        if (const std::optional<QJsonArray> layers = compositorLayers(2000)) {
            const bool present = !layersNamed(*layers, barNamespace_).isEmpty();
            if (present == listed) {
                return true;
            }
        }
        QTest::qWait(100);
    }
    return false;
}

// The compositor's half of the claim: niri accepted the surface, and everything it publishes about it is
// what qml/Main.qml declared. There is no assertion here about anchors or the exclusive zone because niri's
// layer list does not carry them — the case below reads those from the wire instead.
void NiriLiveLayerShellTest::theBarAppearsInTheCompositorsLayerListAndDisappearsWithTheShell() {
    QVERIFY2(startShell(), qPrintable(startFailure_));

    const std::optional<QJsonArray> layers = compositorLayers();
    QVERIFY2(layers.has_value(), "the compositor did not answer a Layers request");
    const QList<QJsonObject> mine = layersNamed(*layers, QString::fromLatin1(expectedNamespace));

    // Exactly one: a second surface with the same namespace would mean the shell is placing two bars, and
    // taking the first match would hide it.
    QCOMPARE(mine.size(), 1);
    const QJsonObject bar = mine.first();

    QCOMPARE(bar.value(QStringLiteral("layer")).toString(), QString::fromLatin1(expectedLayer));
    QCOMPARE(bar.value(QStringLiteral("keyboard_interactivity")).toString(),
             QString::fromLatin1(expectedKeyboardInteractivity));
    // The bar is on an output, but which one is the compositor's choice: qml/Main.qml passes no output and
    // lets niri place it on the focused one. So the assertion is that niri named one, not that it named a
    // particular connector.
    QVERIFY2(!bar.value(QStringLiteral("output")).toString().isEmpty(),
             "the compositor listed the bar without naming an output");

    qInfo("the compositor lists \"%s\" on %s in the %s layer, keyboard interactivity %s",
          expectedNamespace, qPrintable(bar.value(QStringLiteral("output")).toString()),
          qPrintable(bar.value(QStringLiteral("layer")).toString()),
          qPrintable(bar.value(QStringLiteral("keyboard_interactivity")).toString()));

    // Ownership, and the reason this case is worth more than a listing: the surface the compositor is
    // showing is the one this process created, so it goes when this process goes. A surface left behind by
    // something else that happened to use the namespace would still be there.
    stopShell();
    QVERIFY2(waitForBarListed(false),
             "the bar was still in the compositor's layer list after the shell exited, so the surface is "
             "not the one this process created");
}

// The client's half: what was actually asked for, read off the protocol the compositor saw. niri's layer
// list has no field for either of these, so a test that only asked the compositor could not tell a bar with
// the wrong exclusive zone from a correct one.
void NiriLiveLayerShellTest::theBarAsksForItsAnchorsExclusiveZoneAndKeyboardInteractivity() {
    QVERIFY2(startShell(), qPrintable(startFailure_));
    stopShell();

    const LayerRequests requests = parseRequests(transcript());
    const QString saw = describe(requests);

    QVERIFY2(requests.sawGetLayerSurface(),
             qPrintable(QStringLiteral("no get_layer_surface request reached niri; parsed: %1").arg(saw)));
    QCOMPARE(requests.objects.nameSpace, QString::fromLatin1(expectedNamespace));
    QCOMPARE(requests.objects.layer, expectedLayerArgument);

    // Zero width because the bar is anchored to both the left and right edges; the compositor chooses that
    // axis, and a client-chosen size on a stretched axis is a protocol error niri enforces.
    QCOMPARE(requests.requestedWidth, 0);
    QCOMPARE(requests.requestedHeight, expectedHeight);

    // 13 is top|left|right. This is the assertion the compositor cannot make for us.
    QCOMPARE(requests.anchor, expectedAnchor);
    QCOMPARE(requests.exclusiveZone, expectedExclusiveZone);
    QCOMPARE(requests.keyboardInteractivity, expectedKeyboardArgument);

    qInfo("the shell asked for anchor %d, exclusive zone %d, size %dx%d and keyboard interactivity %d",
          requests.anchor, requests.exclusiveZone, requests.requestedWidth, requests.requestedHeight,
          requests.keyboardInteractivity);
}

// The configuration reaching the compositor, which is the only place it can reach beyond the shell: the
// bar's height and namespace come from `config.toml` rather than from a literal in QML, so a run against a
// file that says 44 and a namespace of its own has to be indistinguishable on the wire from a build that
// had those values baked in. This is also the case that would fail if the shell read its configuration
// after creating its surface, because the values are what the surface is created with.
void NiriLiveLayerShellTest::theConfiguredHeightAndNamespaceAreWhatTheCompositorIsGiven() {
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QVERIFY(QDir().mkpath(home.filePath(QStringLiteral("quantum-shell"))));
    QFile file(home.filePath(QStringLiteral("quantum-shell/config.toml")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("schema_version = 1\n\n[bar]\nheight = 44\nnamespace = \"quantum-shell-bar-configured\"\n");
    file.close();

    const QString configuredNamespace = QStringLiteral("quantum-shell-bar-configured");
    QVERIFY2(startShell(home.path(), configuredNamespace), qPrintable(startFailure_));

    // The compositor's half: it accepted the surface, and it knows it by the configured name.
    const std::optional<QJsonArray> layers = compositorLayers();
    QVERIFY2(layers.has_value(), "the compositor did not answer a Layers request");
    const QList<QJsonObject> mine = layersNamed(*layers, configuredNamespace);
    QCOMPARE(mine.size(), 1);
    stopShell();

    // The client's half: the height is the file's, on both requests that carry it.
    const LayerRequests requests = parseRequests(transcript());
    const QString saw = describe(requests);
    QVERIFY2(requests.sawGetLayerSurface(),
             qPrintable(QStringLiteral("no get_layer_surface request reached niri; parsed: %1").arg(saw)));
    QCOMPARE(requests.objects.nameSpace, configuredNamespace);
    QCOMPARE(requests.requestedHeight, 44);
    QCOMPARE(requests.exclusiveZone, 44);
    // Still stretched: the height is configured, the anchors are not.
    QCOMPARE(requests.requestedWidth, 0);
    QCOMPARE(requests.anchor, expectedAnchor);

    qInfo("the configured bar went up as \"%s\" with an exclusive zone of %d",
          qPrintable(configuredNamespace), requests.exclusiveZone);
}

// The ordering niri disconnects a client for getting wrong, and the one this test exists because of: the
// compositor's initial configure has to be acknowledged before a buffer is attached to the surface.
void NiriLiveLayerShellTest::theInitialConfigureIsAcknowledgedBeforeAnyBufferIsAttached() {
    QVERIFY2(startShell(), qPrintable(startFailure_));

    // Waited for rather than slept through. The claim is about the order of two requests, not about how quickly
    // the shell paints, so a fixed pause is a machine-speed assertion: on a busy machine the attach would not
    // have appeared yet and the failure would read as a surface that never painted, which is a different claim
    // and a wrong one. The wait ends the moment the attach is in the transcript.
    QElapsedTimer paintClock;
    paintClock.start();
    while (parseRequests(transcript()).firstAttachIndex == -1
           && paintClock.elapsed() < startUpFactTimeoutMs) {
        QTest::qWait(100);
    }
    const qint64 paintMs = paintClock.elapsed();
    stopShell();

    const LayerRequests requests = parseRequests(transcript());
    QVERIFY2(requests.acked, "the shell never acknowledged the compositor's initial configure");
    QVERIFY2(requests.firstAttachIndex != -1,
             qPrintable(QStringLiteral("no buffer was attached in the %1 ms it was given, so the surface never "
                                       "painted and this claim is not being tested (%2 characters of wayland "
                                       "traffic were read)")
                            .arg(paintMs)
                            .arg(transcript().size())));

    QVERIFY2(requests.ackIndex < requests.firstAttachIndex,
             qPrintable(QStringLiteral("a buffer was attached at line %1, before the initial configure was "
                                       "acknowledged at line %2")
                            .arg(requests.firstAttachIndex)
                            .arg(requests.ackIndex)));

    qInfo("acknowledged the initial configure at line %lld, first attached a buffer at line %lld (painted "
          "after %lld ms)",
          static_cast<long long>(requests.ackIndex), static_cast<long long>(requests.firstAttachIndex),
          static_cast<long long>(paintMs));
}

QString NiriLiveLayerShellTest::runQsctl(const QStringList& arguments, int* exitCode)
{
    *exitCode = -1;
    qsctlError_.clear();

    QProcess qsctl;
    qsctl.setProcessChannelMode(QProcess::SeparateChannels);
    qsctl.start(QStringLiteral(QS_QSCTL_BINARY), arguments);
    if (!qsctl.waitForStarted(5000)) {
        startFailure_ = QStringLiteral("qsctl did not start: %1").arg(qsctl.errorString());
        return QString();
    }
    if (!qsctl.waitForFinished(10000)) {
        qsctl.kill();
        qsctl.waitForFinished(5000);
        startFailure_ = QStringLiteral("qsctl did not finish: %1").arg(qsctl.errorString());
        return QString();
    }

    *exitCode = qsctl.exitCode();
    qsctlError_ = QString::fromUtf8(qsctl.readAllStandardError()).trimmed();
    return QString::fromUtf8(qsctl.readAllStandardOutput()).trimmed();
}

// The shell's own report, read back through the socket it bound, against the compositor's own list of the
// same workspaces. Both are read fresh on every attempt, which is what makes a disagreement tellable apart:
// the stamp is the *compositor's* answer, so a run that ends with the compositor answering the same thing
// while the shell disagrees is a shell that has not applied an event, where one that ends with the
// compositor still answering something new is a desktop that never held still.
qstest::Reading NiriLiveLayerShellTest::readStateAgainstCompositor() {
    qstest::Reading reading;

    int exitCode = -1;
    const QString state = runQsctl({QStringLiteral("state")}, &exitCode);
    if (exitCode != 0) {
        // Not a disagreement: a client that failed, or a shell that refused, has said nothing about the
        // workspaces, and asking again would be waiting on nothing rather than on an event.
        reading.noReading = QStringLiteral("`qsctl state` exited %1").arg(exitCode);
        if (!qsctlError().isEmpty()) {
            reading.noReading += QStringLiteral(": %1").arg(qsctlError());
        }
        return reading;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(state.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        reading.noReading = QStringLiteral("`qsctl state` did not answer with JSON: %1").arg(state);
        return reading;
    }
    const QJsonObject stateObject = document.object();

    const std::optional<Reply> reply = request(client_, QStringLiteral("Workspaces"));
    if (!reply.has_value()) {
        reading.noReading = QStringLiteral("the compositor did not answer a Workspaces request");
        return reading;
    }
    if (!reply->isOk()) {
        reading.noReading = QStringLiteral("the compositor refused a Workspaces request: %1")
                                .arg(reply->describe());
        return reading;
    }

    const QJsonArray fromShell = stateObject.value(QStringLiteral("workspaces")).toArray();
    const QJsonArray fromCompositor = reply->variant(QStringLiteral("Workspaces")).toArray();
    shellWorkspaces_ = fromShell.size();
    reading.stamp = QString::fromUtf8(QJsonDocument(fromCompositor).toJson(QJsonDocument::Compact));

    qstest::Disagreement disagreement(QStringLiteral("the shell"), QStringLiteral("the compositor"));
    // The shell's own view of its connection is part of what it is asked to report, so it is compared here
    // rather than asserted once: a shell that has not connected yet is a reading to take again, and one that
    // never does still fails, with its own message.
    disagreement.check(QStringLiteral("the shell does not report itself connected to the compositor"),
                       stateObject.value(QStringLiteral("connected")).toBool());
    noteWorkspaceDisagreements(disagreement, fromShell, fromCompositor);
    reading.mismatch = disagreement.text();
    return reading;
}

// The public name, verified where it exists: the kernel's own table of unix sockets. What `IPCProtocol.h`
// spells and what the documents freeze is an address with one leading NUL, and the trap this guards against is
// a spelling that binds `@@quantum-shell` instead — a different socket that works perfectly and is connected to
// by nothing, which is why the name is read back rather than asserted from the constant alone.
void NiriLiveLayerShellTest::theFrozenSocketNameIsTheOneTheShellBound() {
    // The mirror of the frozen name, written out here so that renaming the constant fails to build rather than
    // silently renaming a public interface.
    QCOMPARE(abstractSocketRow(), QStringLiteral("@quantum-shell"));

    // Nothing is bound before the shell starts, and something is afterwards: both halves matter, because a
    // stale socket left by an earlier run would otherwise make the second half true for a shell that never
    // listened at all.
    QVERIFY2(!boundSocketInode().has_value(),
             qPrintable(QStringLiteral("%1 was bound before this test started anything")
                            .arg(abstractSocketRow())));

    QVERIFY2(startShell(), qPrintable(startFailure_));

    // `startShell` has already waited for this — the surface reaching the compositor and the socket being bound
    // are two different things the shell does and nothing orders them — so this is a single read. The assertion
    // stays here and names the name this slot is about rather than being folded into the start-up step's
    // failure, which would report a different claim.
    const std::optional<qint64> inode = boundSocketInode();
    QVERIFY2(inode.has_value(),
             qPrintable(QStringLiteral("the shell is running and its surface is on screen, but it has nothing "
                                       "bound at %1")
                            .arg(abstractSocketRow())));
    qInfo("the shell bound the abstract socket %s (inode %lld)", qPrintable(abstractSocketRow()),
          static_cast<long long>(*inode));

    stopShell();

    // And it is gone with the process: an abstract socket has no filesystem entry to be left behind, which is
    // the reason the design chose the abstract namespace over a path in the runtime directory.
    QTRY_VERIFY_WITH_TIMEOUT(!boundSocketInode().has_value(), 5000);
}

// `qsctl` answering through the shell's own socket, which is what "responds to qsctl" means as a claim about
// the shell rather than about a client.
//
// The shell for this case is started against a configuration of its own that names a height no default has, and
// that value is the first thing asserted: `config get bar.height` answering 44 is evidence that the process
// answering is *this* shell, reading *this* file, and not another shell that happens to hold the name. Every
// later assertion in this slot rests on that, so it comes first.
void NiriLiveLayerShellTest::theShellAnswersQsctlOverItsOwnSocket() {
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QVERIFY(QDir().mkpath(home.filePath(QStringLiteral("quantum-shell"))));
    QFile file(home.filePath(QStringLiteral("quantum-shell/config.toml")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    // Both keys, because `startShell` waits for the surface under the namespace it is given: the file has to
    // name it, or the shell would come up under the default name and the wait would be for a surface that is
    // never going to exist.
    file.write("schema_version = 1\n\n[bar]\nheight = 44\nnamespace = \"quantum-shell-bar-ipc\"\n");
    file.close();

    const QString configuredNamespace = QStringLiteral("quantum-shell-bar-ipc");
    QVERIFY2(startShell(home.path(), configuredNamespace), qPrintable(startFailure_));

    int exitCode = -1;
    const QString version = runQsctl({QStringLiteral("version")}, &exitCode);
    QCOMPARE(exitCode, 0);
    QVERIFY2(version.contains(QStringLiteral("protocol %1").arg(quantum::ipc::ProtocolVersion)),
             qPrintable(version));

    // `config get` prints the bare value: unquoted, so that `x=$(qsctl config get bar.height)` needs no
    // unwrapping. 44 rather than the schema's 32, which is this slot's proof of which shell answered.
    QCOMPARE(runQsctl({QStringLiteral("config"), QStringLiteral("get"), QStringLiteral("bar.height")},
                      &exitCode),
             QStringLiteral("44"));
    QCOMPARE(exitCode, 0);

    // A key this shell does not read is refused by name, and the exit code says refused rather than "no shell".
    runQsctl({QStringLiteral("config"), QStringLiteral("get"), QStringLiteral("bar.colour")}, &exitCode);
    QCOMPARE(exitCode, 1);
    QVERIFY2(qsctlError().contains(QStringLiteral("bar.colour")), qPrintable(qsctlError()));

    // Read as close together as two local calls can be, and until the two agree rather than once: the person
    // running this may switch workspace between the two reads, and a change between them is not the claim being
    // false. What is being checked is that the two agree, that they never do is a failure, and a run that
    // needed more than one reading says so instead of passing quietly.
    const qstest::ReconcileOutcome outcome = qstest::reconcile(
        [this] { return readStateAgainstCompositor(); }, 3000, 150,
        qstest::Sides{QStringLiteral("the shell"), QStringLiteral("the compositor")});

    const QString reconciled = qstest::describeReconciliation(QStringLiteral("the workspaces"), outcome);
    if (!reconciled.isEmpty()) {
        qInfo("%s", qPrintable(reconciled));
    }
    QVERIFY2(outcome.agreed,
             qPrintable(qstest::describeFailure(QStringLiteral("the workspaces"), outcome)));
    qInfo("qsctl state agreed with the compositor on %lld workspaces",
          static_cast<long long>(shellWorkspaces_));
}

// The verb that acts. Hiding a layer surface is observable in exactly one place — the compositor's layer list —
// so the answer printed by `qsctl` is checked against that rather than trusted: a `bar toggle` that reported a
// visibility it did not have would pass every client-side assertion and leave the bar on screen.
void NiriLiveLayerShellTest::qsctlTogglesTheBarOutOfTheCompositorsLayerList() {
    QVERIFY2(startShell(), qPrintable(startFailure_));

    int exitCode = -1;
    const QString hidden = runQsctl({QStringLiteral("bar"), QStringLiteral("toggle")}, &exitCode);
    QCOMPARE(exitCode, 0);
    QVERIFY2(hidden.contains(QStringLiteral("false")), qPrintable(hidden));
    QVERIFY2(waitForBarListed(false),
             "the bar is still in the compositor's layer list after `qsctl bar toggle` reported it hidden");

    const QString shown = runQsctl({QStringLiteral("bar"), QStringLiteral("toggle")}, &exitCode);
    QCOMPARE(exitCode, 0);
    QVERIFY2(shown.contains(QStringLiteral("true")), qPrintable(shown));
    QVERIFY2(waitForBarListed(true),
             "the bar did not come back into the compositor's layer list after the second toggle");

    qInfo("qsctl toggled the bar out of the layer list (%s) and back (%s)", qPrintable(hidden),
          qPrintable(shown));
}

QTEST_GUILESS_MAIN(NiriLiveLayerShellTest)
#include "niri_live_layershell_test.moc"
