// Unit tests for the output refresh service: what it asks for, when it asks, and what it does with
// each kind of answer.
//
// The compositor's end of the socket is FakeNiriServer, so this needs no running niri. The real
// replies — a real output with a real fractional scale and a real mode list — are read from the
// running compositor by tests/integration/niri_live_stream_test.cpp.
#include "niri/NiriOutputs.h"

#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"

#include "FakeNiriServer.h"
#include "NiriProtocolTestData.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>

#include <functional>
#include <memory>

using quantum::niri::NiriEventStream;
using quantum::niri::NiriIPC;
using quantum::niri::NiriOutput;
using quantum::niri::NiriOutputs;
using qstest::array;
using qstest::eventLine;
using qstest::logicalObject;
using qstest::modeObject;
using qstest::outputObject;
using qstest::outputsReplyLine;
using qstest::workspaceObject;

namespace {

// The event stream is the second connection this test opens, so raw events are addressed to it
// explicitly rather than to whichever socket happened to be accepted first.
constexpr int streamConnection = 1;

bool waitFor(const std::function<bool()>& condition) {
    return QTest::qWaitFor(condition, 5000);
}

int outputsRequests(const FakeNiriServer& server) {
    int count = 0;
    for (const QString& line : server.receivedRequests()) {
        if (line == QStringLiteral("\"Outputs\"")) {
            ++count;
        }
    }
    return count;
}

QJsonObject enabledOutput(const QString& name) {
    return outputObject(name, {modeObject(3840, 2160, 59997, true)}, QJsonValue(0),
                        QJsonValue(logicalObject(0, 0, 3072, 1728, 1.25)));
}

QByteArray workspacesEvent(const QStringList& outputNames) {
    QList<QJsonObject> workspaces;
    quint64 id = 1;
    for (const QString& name : outputNames) {
        workspaces.append(workspaceObject(id, static_cast<int>(id), name));
        ++id;
    }
    return eventLine(QStringLiteral("WorkspacesChanged"),
                     QJsonObject{{QStringLiteral("workspaces"), array(workspaces)}});
}

}  // namespace

class NiriOutputsTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void asksOnceAndOrdersTheOutputs();
    void coalescesRefreshesWhileOneIsInFlight();
    void asksAgainWhenTheWorkspacesNameAnOutputItHasNotSeen();
    void doesNotAskAgainForTheSameWorkspaceOutputs();
    void asksAgainAfterAConfigLoad();
    void reportsARefusedRequestAndKeepsTheLastCompleteList();
    void treatsAPartialReplyAsAFailure();
    void forgetsTheOutputsWhenTheCompositorGoesAway();

private:
    std::unique_ptr<FakeNiriServer> server_;
    std::unique_ptr<NiriIPC> requests_;
    std::unique_ptr<NiriEventStream> stream_;
    std::unique_ptr<NiriOutputs> outputs_;
    QList<NiriOutput> lastChange_;
    QStringList failures_;
};

void NiriOutputsTest::init() {
    // QTest runs one test function per init(). The previous test's objects are torn down first —
    // their sockets are gone, so anything still queued on them would otherwise report into the
    // records this test is about to trust — and only then are the records cleared.
    outputs_.reset();
    stream_.reset();
    requests_.reset();
    server_.reset();
    lastChange_.clear();
    failures_.clear();

    server_ = std::make_unique<FakeNiriServer>();
    QVERIFY2(server_->listen(), qPrintable(server_->serverError()));
    server_->setReply(QStringLiteral("EventStream"), QByteArray(R"json({"Ok":"Handled"})json"));
    // Deliberately in the opposite order to the one the shell wants them in, so the ordering the
    // service promises is not just the order the reply happened to arrive in.
    server_->setReply(QStringLiteral("Outputs"),
                      outputsReplyLine({enabledOutput(QStringLiteral("HDMI-A-1")),
                                        enabledOutput(QStringLiteral("DP-3"))}));

    requests_ = std::make_unique<NiriIPC>();
    QSignalSpy requestConnected(requests_.get(), &NiriIPC::connected);
    requests_->connectToCompositor(server_->path());
    QTRY_VERIFY_WITH_TIMEOUT(requestConnected.count() == 1, 5000);

    stream_ = std::make_unique<NiriEventStream>();
    stream_->connectToCompositor(server_->path());
    QVERIFY2(waitFor([this] { return stream_->isStreaming(); }),
             "the compositor never acknowledged the subscription");

    outputs_ = std::make_unique<NiriOutputs>(*requests_);
    connect(outputs_.get(), &NiriOutputs::outputsChanged, this,
            [this](const QList<NiriOutput>& outputs) { lastChange_ = outputs; });
    connect(outputs_.get(), &NiriOutputs::refreshFailed, this,
            [this](const QString& reason) { failures_.append(reason); });
    outputs_->observe(*stream_);

    QVERIFY2(waitFor([this] { return !outputs_->outputs().isEmpty(); }), "no outputs arrived");
}

void NiriOutputsTest::asksOnceAndOrdersTheOutputs() {
    // One request: asked for on start, not on a timer and not repeatedly.
    QCOMPARE(outputsRequests(*server_), 1);
    QCOMPARE(outputs_->outputs().size(), 2);
    QCOMPARE(outputs_->outputs().at(0).name(), QStringLiteral("DP-3"));
    QCOMPARE(outputs_->outputs().at(1).name(), QStringLiteral("HDMI-A-1"));
    QCOMPARE(lastChange_.size(), 2);
    // The wire form of the request: a bare JSON string, like every other unit request.
    QVERIFY(server_->receivedRequests().contains(QStringLiteral("\"Outputs\"")));
    QVERIFY(failures_.isEmpty());
}

void NiriOutputsTest::coalescesRefreshesWhileOneIsInFlight() {
    const int before = outputsRequests(*server_);

    // Three asks in a row, with no event loop between them: the first goes out, the others are one
    // pending refresh between them.
    outputs_->refresh();
    outputs_->refresh();
    outputs_->refresh();

    QVERIFY(waitFor([&] { return outputsRequests(*server_) >= before + 2; }));
    QTest::qWait(100);
    // Exactly one extra request: not three, and not none.
    QCOMPARE(outputsRequests(*server_), before + 2);
    QVERIFY(!outputs_->isRefreshing());
}

void NiriOutputsTest::asksAgainWhenTheWorkspacesNameAnOutputItHasNotSeen() {
    const int before = outputsRequests(*server_);

    // niri sends no output event, so a workspace naming an output the last reply did not mention is
    // the only thing that can point at a monitor having appeared.
    server_->writeRawTo(streamConnection,
                        workspacesEvent({QStringLiteral("DP-3"), QStringLiteral("DP-9")}));

    QVERIFY(waitFor([&] { return outputsRequests(*server_) > before; }));
}

void NiriOutputsTest::doesNotAskAgainForTheSameWorkspaceOutputs() {
    server_->writeRawTo(streamConnection, workspacesEvent({QStringLiteral("DP-3")}));
    QVERIFY(waitFor([this] { return !outputs_->isRefreshing(); }));
    QTest::qWait(50);
    const int settled = outputsRequests(*server_);

    // The same set of output names again is not news about the outputs.
    server_->writeRawTo(streamConnection, workspacesEvent({QStringLiteral("DP-3")}));
    QTest::qWait(100);
    QCOMPARE(outputsRequests(*server_), settled);
}

void NiriOutputsTest::asksAgainAfterAConfigLoad() {
    const int before = outputsRequests(*server_);

    // Output configuration lives in niri's config file, so a reload is exactly when scale, mode and
    // position may have changed.
    server_->writeRawTo(streamConnection,
                        eventLine(QStringLiteral("ConfigLoaded"),
                                  QJsonObject{{QStringLiteral("failed"), QJsonValue(false)}}));

    QVERIFY(waitFor([&] { return outputsRequests(*server_) > before; }));
}

void NiriOutputsTest::reportsARefusedRequestAndKeepsTheLastCompleteList() {
    server_->setReply(QStringLiteral("Outputs"), QByteArray(R"json({"Err":"error parsing request"})json"));

    outputs_->refresh();
    QVERIFY(waitFor([this] { return !failures_.isEmpty(); }));
    QVERIFY(failures_.last().contains(QStringLiteral("refused")));

    // The last thing the compositor actually said is still the best answer available.
    QCOMPARE(outputs_->outputs().size(), 2);

    // A failure does not wedge the service: the next answer is used, and it is the source of truth
    // again rather than being ignored because something failed earlier.
    QVERIFY(!outputs_->isRefreshing());
    server_->setReply(QStringLiteral("Outputs"),
                      outputsReplyLine({enabledOutput(QStringLiteral("DP-3")),
                                        enabledOutput(QStringLiteral("HDMI-A-1")),
                                        enabledOutput(QStringLiteral("DP-4"))}));
    outputs_->refresh();
    QVERIFY(waitFor([this] { return outputs_->outputs().size() == 3; }));
}

void NiriOutputsTest::treatsAPartialReplyAsAFailure() {
    QJsonObject nameless = enabledOutput(QStringLiteral("DP-4"));
    nameless.remove(QStringLiteral("name"));
    server_->setReply(QStringLiteral("Outputs"),
                      outputsReplyLine({enabledOutput(QStringLiteral("DP-3")), nameless}));

    outputs_->refresh();
    QVERIFY(waitFor([this] { return !failures_.isEmpty(); }));
    QVERIFY(failures_.last().contains(QStringLiteral("no usable name")));

    // The previous complete list is kept. Applying what could be read would drop an output, and a
    // missing output in this list is indistinguishable from an unplugged monitor.
    QCOMPARE(outputs_->outputs().size(), 2);
    QCOMPARE(outputs_->outputs().at(0).name(), QStringLiteral("DP-3"));
}

void NiriOutputsTest::forgetsTheOutputsWhenTheCompositorGoesAway() {
    QVERIFY(!outputs_->outputs().isEmpty());

    server_->closeConnections();

    QVERIFY(waitFor([this] { return outputs_->outputs().isEmpty(); }));
    QVERIFY(lastChange_.isEmpty());
    // Nothing is left believing a request is still in flight, which would stop every later trigger.
    QVERIFY(!outputs_->isRefreshing());
}

QTEST_GUILESS_MAIN(NiriOutputsTest)
#include "niri_outputs_test.moc"
