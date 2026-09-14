// Unit tests for the niri IPC client: wire encoding, reply decoding, line framing, reply ordering and
// the detection pass.
//
// The compositor side of the socket is FakeNiriServer, so none of this needs a running niri. The real
// protocol is covered separately, against the compositor actually running on the machine, by
// tests/integration/niri_live_test.cpp.
#include "niri/NiriIPC.h"

#include "FakeNiriServer.h"

#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <memory>
#include <optional>

using quantum::niri::Availability;
using quantum::niri::NiriCapabilities;
using quantum::niri::NiriFeature;
using quantum::niri::NiriIPC;
using quantum::niri::NiriVersion;
using quantum::niri::Reply;
using quantum::niri::availabilityName;
using quantum::niri::classifyProbeReply;
using quantum::niri::featureName;
using quantum::niri::maximumLineBytes;
using quantum::niri::probedFeatures;
using quantum::niri::requestName;
using quantum::niri::takeCompleteLines;

namespace {

// What a handler saw, so a test can assert after the event loop has delivered the reply.
struct Captured {
    int calls = 0;
    Reply reply;
};

// The replies a running niri 26.04 was measured to give. The three marked "captured" are the exact
// lines recorded from the real compositor; the others carry the response variant this code classifies
// on plus a neutral payload, because what a unit test checks here is the client, not the shape of
// every possible session.
void answerLikeNiri26_04(FakeNiriServer& server) {
    server.setReply(QStringLiteral("Version"), QByteArray(R"json({"Ok":{"Version":"26.04 (8ed0da4)"}})json"));
    server.setReply(QStringLiteral("OverviewState"),
                    QByteArray(R"json({"Ok":{"OverviewState":{"is_open":false}}})json"));  // captured
    server.setReply(QStringLiteral("Casts"), QByteArray(R"json({"Ok":{"Casts":[]}})json"));   // captured
    server.setReply(QStringLiteral("Outputs"), QByteArray(R"json({"Ok":{"Outputs":{}}})json"));
    server.setReply(QStringLiteral("Workspaces"), QByteArray(R"json({"Ok":{"Workspaces":[]}})json"));
    server.setReply(QStringLiteral("Windows"), QByteArray(R"json({"Ok":{"Windows":[]}})json"));
    server.setReply(QStringLiteral("Layers"), QByteArray(R"json({"Ok":{"Layers":[]}})json"));
    server.setReply(QStringLiteral("KeyboardLayouts"),
                    QByteArray(R"json({"Ok":{"KeyboardLayouts":null}})json"));
    server.setReply(QStringLiteral("FocusedOutput"), QByteArray(R"json({"Ok":{"FocusedOutput":null}})json"));
    server.setReply(QStringLiteral("FocusedWindow"), QByteArray(R"json({"Ok":{"FocusedWindow":null}})json"));
}

}  // namespace

class NiriIpcTest : public QObject {
    Q_OBJECT

private slots:
    void sendsRequestsAsTheBareJsonStringNiriExpects();
    void refusesARequestNameThatIsNotAnIdentifier();
    void decodesAnOkReplyAndItsVariant();
    void decodesACompositorErrorAsAnError();
    void keepsWorkingAfterARejectedRequest();
    void classifiesAnswersItCannotTrustAsUnknown();
    void matchesRepliesToRequestsInOrder();
    void handlesTwoRepliesThatArriveInOneRead();
    void sendsARequestWithFieldsAsOneCompactLine();
    void mixesUnitAndFieldRequestsWithoutLosingReplyOrder();
    void framesPartialLinesAndIgnoresBlankOnes();
    void rejectsAReplyThatIsNotAJsonReply();
    void reportsATransportErrorWhenTheSocketDoesNotExist();
    void answersOutstandingRequestsWhenTheConnectionDrops();
    void deliversAReplyAfterTheSendingScopeHasEnded();
    void dropsAnOversizedLineInsteadOfBufferingIt();
    void detectsVersionAndProbesEveryReadOnlyRequest();
    void reportsAProbeAnswerAsUnsupportedForAnUnknownRequest();
    void detectionFailsWhenTheCompositorRefusesTheVersion();
    void detectionFailsOnAVersionItCannotRecognise();
    void detectionFailsWithoutAConnection();
};

void NiriIpcTest::sendsRequestsAsTheBareJsonStringNiriExpects() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    Captured captured;
    client.send(QStringLiteral("Version"), [&captured](const Reply& reply) {
        captured.calls += 1;
        captured.reply = reply;
    });

    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 1, 5000);
    // One JSON value per line, the unit variant as a bare string, no whitespace around it.
    QCOMPARE(server.receivedRequests(), QStringList{QStringLiteral("\"Version\"")});
}

void NiriIpcTest::decodesAnOkReplyAndItsVariant() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("Version"),
                    QByteArray(R"json({"Ok":{"Version":"26.04 (8ed0da4)"}})json"));

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    Captured captured;
    client.send(QStringLiteral("Version"), [&captured](const Reply& reply) {
        captured.calls += 1;
        captured.reply = reply;
    });

    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 1, 5000);
    QVERIFY(captured.reply.isOk());
    QCOMPARE(captured.reply.variant(QStringLiteral("Version")).toString(),
             QStringLiteral("26.04 (8ed0da4)"));
    // A variant that is not in this reply is undefined, so nothing can read a value out of it.
    QVERIFY(captured.reply.variant(QStringLiteral("Casts")).isUndefined());
}

void NiriIpcTest::decodesACompositorErrorAsAnError() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    // Captured from `niri msg request-error`, which asks the compositor for exactly this.
    server.setReply(QStringLiteral("Version"),
                    QByteArray(R"json({"Err":"example compositor error"})json"));

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    Captured captured;
    client.send(QStringLiteral("Version"), [&captured](const Reply& reply) {
        captured.calls += 1;
        captured.reply = reply;
    });

    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 1, 5000);
    QVERIFY(!captured.reply.isOk());
    QCOMPARE(captured.reply.text, QStringLiteral("example compositor error"));
    // A real compositor error is not a missing capability: the request itself was understood.
    QCOMPARE(availabilityName(classifyProbeReply(QStringLiteral("Version"), captured.reply)),
             QStringLiteral("supported"));
}

void NiriIpcTest::keepsWorkingAfterARejectedRequest() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    // Captured: what niri answers a request it does not know.
    server.setReply(QStringLiteral("Version"),
                    QByteArray(R"json({"Err":"error parsing request"})json"));
    server.setReply(QStringLiteral("Workspaces"), QByteArray(R"json({"Ok":{"Workspaces":[]}})json"));

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    Captured rejected;
    Captured accepted;
    client.send(QStringLiteral("Version"), [&rejected](const Reply& reply) {
        rejected.calls += 1;
        rejected.reply = reply;
    });
    client.send(QStringLiteral("Workspaces"), [&accepted](const Reply& reply) {
        accepted.calls += 1;
        accepted.reply = reply;
    });

    QTRY_VERIFY_WITH_TIMEOUT(accepted.calls == 1, 5000);
    QCOMPARE(rejected.calls, 1);
    QCOMPARE(availabilityName(classifyProbeReply(QStringLiteral("Version"), rejected.reply)),
             QStringLiteral("unsupported"));
    // The rejected request does not wedge the stream: the next reply is still matched and decoded.
    QVERIFY(accepted.reply.isOk());
    QVERIFY(accepted.reply.variant(QStringLiteral("Workspaces")).isArray());
}

void NiriIpcTest::classifiesAnswersItCannotTrustAsUnknown() {
    Reply transportFailure;
    transportFailure.text = QStringLiteral("niri closed the IPC connection");
    QCOMPARE(availabilityName(classifyProbeReply(QStringLiteral("Casts"), transportFailure)),
             QStringLiteral("unknown"));

    Reply unexpectedVariant;
    unexpectedVariant.kind = Reply::Kind::ok;
    unexpectedVariant.value = QJsonObject{{QStringLiteral("SomethingElse"), true}};
    QCOMPARE(availabilityName(classifyProbeReply(QStringLiteral("Casts"), unexpectedVariant)),
             QStringLiteral("unknown"));

    // A response whose payload is JSON null is still that response: niri answers FocusedWindow with
    // null when no window is focused, which is a capability that works.
    Reply nullPayload;
    nullPayload.kind = Reply::Kind::ok;
    nullPayload.value = QJsonObject{{QStringLiteral("FocusedWindow"), QJsonValue{}}};
    QCOMPARE(availabilityName(classifyProbeReply(QStringLiteral("FocusedWindow"), nullPayload)),
             QStringLiteral("supported"));
}

void NiriIpcTest::matchesRepliesToRequestsInOrder() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("Version"),
                    QByteArray(R"json({"Ok":{"Version":"26.04 (8ed0da4)"}})json"));
    server.setReply(QStringLiteral("Casts"), QByteArray(R"json({"Ok":{"Casts":[]}})json"));

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    Captured first;
    Captured second;
    client.send(QStringLiteral("Version"), [&first](const Reply& reply) {
        first.calls += 1;
        first.reply = reply;
    });
    client.send(QStringLiteral("Casts"), [&second](const Reply& reply) {
        second.calls += 1;
        second.reply = reply;
    });

    QTRY_VERIFY_WITH_TIMEOUT(second.calls == 1, 5000);
    QCOMPARE(first.calls, 1);
    QCOMPARE(server.receivedRequests(),
             (QStringList{QStringLiteral("\"Version\""), QStringLiteral("\"Casts\"")}));
    QCOMPARE(first.reply.variant(QStringLiteral("Version")).toString(),
             QStringLiteral("26.04 (8ed0da4)"));
    QVERIFY(second.reply.variant(QStringLiteral("Casts")).isArray());
    QVERIFY(second.reply.variant(QStringLiteral("Version")).isUndefined());
}

void NiriIpcTest::handlesTwoRepliesThatArriveInOneRead() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("Version"),
                    QByteArray(R"json({"Ok":{"Version":"26.04 (8ed0da4)"}})json"));
    server.setReply(QStringLiteral("Casts"), QByteArray(R"json({"Ok":{"Casts":[]}})json"));
    server.setDeferReplies(true);

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    Captured first;
    Captured second;
    client.send(QStringLiteral("Version"), [&first](const Reply& reply) {
        first.calls += 1;
        first.reply = reply;
    });
    client.send(QStringLiteral("Casts"), [&second](const Reply& reply) {
        second.calls += 1;
        second.reply = reply;
    });

    QTRY_VERIFY_WITH_TIMEOUT(server.receivedRequests().size() == 2, 5000);
    QCOMPARE(first.calls, 0);
    server.flushReplies();  // both replies in a single write

    QTRY_VERIFY_WITH_TIMEOUT(second.calls == 1, 5000);
    QCOMPARE(first.calls, 1);
    QCOMPARE(first.reply.variant(QStringLiteral("Version")).toString(),
             QStringLiteral("26.04 (8ed0da4)"));
    QVERIFY(second.reply.isOk());
}

void NiriIpcTest::sendsARequestWithFieldsAsOneCompactLine() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    // Captured: what niri answers a request it handled. An action's reply is the bare string.
    server.setReply(QStringLiteral("Action"), QByteArray(R"json({"Ok":"Handled"})json"));

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    Captured captured;
    client.sendObject(QJsonObject{{QStringLiteral("Action"),
                                   QJsonObject{{QStringLiteral("OpenOverview"), QJsonObject{}}}}},
                      [&captured](const Reply& reply) {
                          captured.calls += 1;
                          captured.reply = reply;
                      });

    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 1, 5000);
    // One compact object per line, spelled the way niri-ipc v26.04 serialises Action::OpenOverview.
    QCOMPARE(server.receivedRequests(),
             QStringList{QStringLiteral(R"({"Action":{"OpenOverview":{}}})")});
    QVERIFY(captured.reply.isOk());
    QCOMPARE(captured.reply.value.toString(), QStringLiteral("Handled"));

    // A newline inside a value cannot split the request into two lines: it is escaped. This is what
    // keeps reply matching from drifting when the caller's own data contains one.
    Captured withNewline;
    client.sendObject(
        QJsonObject{{QStringLiteral("Action"),
                     QJsonObject{{QStringLiteral("SpawnSh"),
                                  QJsonObject{{QStringLiteral("command"),
                                               QStringLiteral("printf 'a\nb'")}}}}}},
        [&withNewline](const Reply& reply) {
            withNewline.calls += 1;
            withNewline.reply = reply;
        });

    QTRY_VERIFY_WITH_TIMEOUT(withNewline.calls == 1, 5000);
    // Two requests in total: had the value's newline gone out raw, there would be three lines.
    QCOMPARE(server.receivedRequests().size(), 2);
    QVERIFY(!server.receivedRequests().last().contains(QLatin1Char('\n')));
    QVERIFY(server.receivedRequests().last().contains(QStringLiteral(R"(\n)")));
}

void NiriIpcTest::mixesUnitAndFieldRequestsWithoutLosingReplyOrder() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("Version"),
                    QByteArray(R"json({"Ok":{"Version":"26.04 (8ed0da4)"}})json"));
    server.setReply(QStringLiteral("Action"), QByteArray(R"json({"Ok":"Handled"})json"));
    server.setReply(QStringLiteral("Casts"), QByteArray(R"json({"Ok":{"Casts":[]}})json"));

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    Captured version;
    Captured action;
    Captured casts;
    client.send(QStringLiteral("Version"), [&version](const Reply& reply) {
        version.calls += 1;
        version.reply = reply;
    });
    // A real action, with the nested reference a workspace action carries.
    client.sendObject(
        QJsonObject{{QStringLiteral("Action"),
                     QJsonObject{{QStringLiteral("FocusWorkspace"),
                                  QJsonObject{{QStringLiteral("reference"),
                                               QJsonObject{{QStringLiteral("Index"), 2}}}}}}}},
        [&action](const Reply& reply) {
            action.calls += 1;
            action.reply = reply;
        });
    client.send(QStringLiteral("Casts"), [&casts](const Reply& reply) {
        casts.calls += 1;
        casts.reply = reply;
    });

    QTRY_VERIFY_WITH_TIMEOUT(casts.calls == 1, 5000);
    QCOMPARE(version.calls, 1);
    QCOMPARE(action.calls, 1);

    // All three went out in the order they were queued, whatever shape each one has.
    QCOMPARE(server.receivedRequests(),
             (QStringList{QStringLiteral("\"Version\""),
                          QStringLiteral(R"({"Action":{"FocusWorkspace":{"reference":{"Index":2}}}})"),
                          QStringLiteral("\"Casts\"")}));

    // And each handler got its own reply: a field request does not shift the ones around it.
    QCOMPARE(version.reply.variant(QStringLiteral("Version")).toString(),
             QStringLiteral("26.04 (8ed0da4)"));
    QVERIFY(action.reply.isOk());
    QCOMPARE(action.reply.value.toString(), QStringLiteral("Handled"));
    QVERIFY(casts.reply.variant(QStringLiteral("Casts")).isArray());
    QVERIFY(casts.reply.variant(QStringLiteral("Version")).isUndefined());
}

void NiriIpcTest::framesPartialLinesAndIgnoresBlankOnes() {
    QByteArray buffer = QByteArray(R"json({"Ok":{"Version":"26.04"}})json") + "\n";
    buffer += R"json({"Ok":{"Casts":[])json";

    QList<QByteArray> lines = takeCompleteLines(buffer);
    QCOMPARE(lines.size(), 1);
    QCOMPARE(lines.first(), QByteArray(R"json({"Ok":{"Version":"26.04"}})json"));
    // The incomplete line stays in the buffer: parsing it now would lose the rest of it.
    QCOMPARE(buffer, QByteArray(R"json({"Ok":{"Casts":[])json"));

    buffer += "}}\r\n";
    buffer += "\n";  // an empty line must not shift which request the next reply answers
    lines = takeCompleteLines(buffer);
    QCOMPARE(lines.size(), 1);
    QCOMPARE(lines.first(), QByteArray(R"json({"Ok":{"Casts":[]}})json"));
    QVERIFY(buffer.isEmpty());
    QVERIFY(takeCompleteLines(buffer).isEmpty());
}

void NiriIpcTest::rejectsAReplyThatIsNotAJsonReply() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("Version"), QByteArray("not json at all"));
    server.setReply(QStringLiteral("Casts"), QByteArray(R"json({"Nope":1})json"));

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    Captured garbage;
    Captured wrongShape;
    client.send(QStringLiteral("Version"), [&garbage](const Reply& reply) {
        garbage.calls += 1;
        garbage.reply = reply;
    });
    client.send(QStringLiteral("Casts"), [&wrongShape](const Reply& reply) {
        wrongShape.calls += 1;
        wrongShape.reply = reply;
    });

    QTRY_VERIFY_WITH_TIMEOUT(wrongShape.calls == 1, 5000);
    QCOMPARE(garbage.calls, 1);
    QVERIFY(!garbage.reply.isOk());
    QVERIFY(!garbage.reply.text.isEmpty());
    QVERIFY(!wrongShape.reply.isOk());
    QVERIFY(!wrongShape.reply.text.isEmpty());
}

void NiriIpcTest::reportsATransportErrorWhenTheSocketDoesNotExist() {
    NiriIPC client;
    Captured captured;
    // Queued before any connection exists: the promise is that a handler is never left hanging.
    client.send(QStringLiteral("Version"), [&captured](const Reply& reply) {
        captured.calls += 1;
        captured.reply = reply;
    });

    QSignalSpy errors(&client, &NiriIPC::transportError);
    client.connectToCompositor(QStringLiteral("/nonexistent/niri-test-socket.sock"));

    QTRY_VERIFY_WITH_TIMEOUT(errors.count() >= 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 1, 5000);
    QVERIFY(!captured.reply.isOk());
    QVERIFY(!captured.reply.text.isEmpty());
    QVERIFY(!client.isConnected());
}

void NiriIpcTest::answersOutstandingRequestsWhenTheConnectionDrops() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setDeferReplies(true);  // the reply is never sent; the connection goes instead

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    Captured captured;
    client.send(QStringLiteral("Version"), [&captured](const Reply& reply) {
        captured.calls += 1;
        captured.reply = reply;
    });
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedRequests().size() == 1, 5000);

    QSignalSpy disconnected(&client, &NiriIPC::disconnected);
    server.closeConnections();

    QTRY_VERIFY_WITH_TIMEOUT(disconnected.count() >= 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 1, 5000);
    QVERIFY(!captured.reply.isOk());
    QVERIFY(!captured.reply.text.isEmpty());
}

// The live tests wait for each reply with a deadline and return empty if it does not come. That is only
// safe because a handler belongs to this client, not to the caller: `send` gives no way to withdraw one,
// so a reply arriving after the caller gave up still runs it. Every helper in tests/integration/ depends
// on that, which is why the state they write into is shared rather than a local the handler refers to —
// a handler referring to a caller's local would write a reply through a frame that has gone. This slot
// pins the contract those helpers rest on, so a future change that dropped a handler would be caught
// here rather than as a corrupted stack in a live run.
void NiriIpcTest::deliversAReplyAfterTheSendingScopeHasEnded() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    answerLikeNiri26_04(server);
    server.setDeferReplies(true);  // the reply is withheld until well after the caller has given up

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    // A helper of the shape the live tests use, down to the deadline: the request is sent, the deadline
    // expires because the compositor is holding the answer back, and the caller returns empty-handed.
    auto received = std::make_shared<Captured>();
    const std::optional<Reply> waited = [&]() -> std::optional<Reply> {
        client.send(QStringLiteral("Version"), [received](const Reply& reply) {
            received->calls += 1;
            received->reply = reply;
        });
        if (!QTest::qWaitFor([received] { return received->calls == 1; }, 50)) {
            return std::nullopt;
        }
        return received->reply;
    }();
    QVERIFY2(!waited.has_value(), "the fake compositor was asked to withhold the reply, so the wait must time out");
    QCOMPARE(received->calls, 0);
    QCOMPARE(server.receivedRequests(), QStringList{QStringLiteral("\"Version\"")});

    server.flushReplies();  // the answer arrives now that the caller is gone

    QTRY_VERIFY_WITH_TIMEOUT(received->calls == 1, 5000);
    QCOMPARE(received->reply.variant(QStringLiteral("Version")).toString(),
             QStringLiteral("26.04 (8ed0da4)"));

    // The client is still usable afterwards: a handler that outlived its caller leaves no trace in the
    // pending queue, so the next reply is still matched to the next request.
    server.setDeferReplies(false);
    Captured next;
    client.send(QStringLiteral("Version"), [&next](const Reply& reply) {
        next.calls += 1;
        next.reply = reply;
    });
    QTRY_VERIFY_WITH_TIMEOUT(next.calls == 1, 5000);
    QCOMPARE(next.reply.variant(QStringLiteral("Version")).toString(), QStringLiteral("26.04 (8ed0da4)"));
}

void NiriIpcTest::dropsAnOversizedLineInsteadOfBufferingIt() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setDeferReplies(true);

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    Captured captured;
    client.send(QStringLiteral("Version"), [&captured](const Reply& reply) {
        captured.calls += 1;
        captured.reply = reply;
    });
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedRequests().size() == 1, 5000);

    QSignalSpy errors(&client, &NiriIPC::transportError);
    // No newline anywhere in it, so it can never become a reply line.
    server.writeRaw(QByteArray(maximumLineBytes + 1, 'x'));

    QTRY_VERIFY_WITH_TIMEOUT(errors.count() >= 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 1, 5000);
    QVERIFY(!captured.reply.isOk());
    QVERIFY(captured.reply.text.contains(QStringLiteral("line break")));
}

void NiriIpcTest::detectsVersionAndProbesEveryReadOnlyRequest() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    answerLikeNiri26_04(server);

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    bool versionSeen = false;
    bool capabilitiesSeen = false;
    NiriVersion detected;
    NiriCapabilities capabilities;
    QString failure;
    connect(&client, &NiriIPC::versionDetected, this, [&](const NiriVersion& version) {
        versionSeen = true;
        detected = version;
    });
    connect(&client, &NiriIPC::capabilitiesDetected, this, [&](const NiriCapabilities& known) {
        capabilitiesSeen = true;
        capabilities = known;
    });
    connect(&client, &NiriIPC::detectionFailed, this, [&](const QString& reason) { failure = reason; });

    client.detect();

    QTRY_VERIFY_WITH_TIMEOUT(capabilitiesSeen, 5000);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(versionSeen);
    QVERIFY(detected.isSupported());
    QCOMPARE(detected.toString(), QStringLiteral("26.04 (8ed0da4)"));

    // Version plus one request per probed feature, each spelled the way niri expects.
    QCOMPARE(server.receivedRequests().size(), probedFeatures().size() + 1);
    QVERIFY(server.receivedRequests().contains(QStringLiteral("\"Version\"")));
    for (const NiriFeature feature : probedFeatures()) {
        const QString line = QStringLiteral("\"%1\"").arg(requestName(feature));
        QVERIFY2(server.receivedRequests().contains(line), qPrintable(line));
        QVERIFY2(capabilities.isSupported(feature), qPrintable(featureName(feature)));
    }
    QVERIFY(capabilities.isSupported(NiriFeature::version));
    // 26.04 is the release that exposes ext-background-effect, so the version decides this one.
    QVERIFY(capabilities.isSupported(NiriFeature::compositorBackgroundEffect));
}

void NiriIpcTest::reportsAProbeAnswerAsUnsupportedForAnUnknownRequest() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    answerLikeNiri26_04(server);
    // A compositor built without one of the requests still reports a version and still answers
    // everything else, which is exactly the case the probe exists for.
    server.setReply(QStringLiteral("Casts"), QByteArray(R"json({"Err":"error parsing request"})json"));

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    bool capabilitiesSeen = false;
    NiriCapabilities capabilities;
    connect(&client, &NiriIPC::capabilitiesDetected, this, [&](const NiriCapabilities& known) {
        capabilitiesSeen = true;
        capabilities = known;
    });

    client.detect();

    QTRY_VERIFY_WITH_TIMEOUT(capabilitiesSeen, 5000);
    QCOMPARE(availabilityName(capabilities.availability(NiriFeature::casts)),
             QStringLiteral("unsupported"));
    QVERIFY(capabilities.isSupported(NiriFeature::workspaces));
    QVERIFY(capabilities.isSupported(NiriFeature::compositorBackgroundEffect));
}

void NiriIpcTest::detectionFailsWhenTheCompositorRefusesTheVersion() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("Version"),
                    QByteArray(R"json({"Err":"error parsing request"})json"));

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    bool capabilitiesSeen = false;
    QString failure;
    connect(&client, &NiriIPC::capabilitiesDetected, this, [&capabilitiesSeen](const NiriCapabilities&) {
        capabilitiesSeen = true;
    });
    connect(&client, &NiriIPC::detectionFailed, this, [&failure](const QString& reason) { failure = reason; });

    client.detect();

    QTRY_VERIFY_WITH_TIMEOUT(!failure.isEmpty(), 5000);
    QVERIFY(!capabilitiesSeen);
    QVERIFY(failure.contains(QStringLiteral("error parsing request")));
}

void NiriIpcTest::detectionFailsOnAVersionItCannotRecognise() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("Version"), QByteArray(R"json({"Ok":{"Version":"nightly"}})json"));

    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    QString failure;
    connect(&client, &NiriIPC::detectionFailed, this, [&failure](const QString& reason) { failure = reason; });

    client.detect();

    QTRY_VERIFY_WITH_TIMEOUT(!failure.isEmpty(), 5000);
    // Reported, not silently accepted as some other version.
    QVERIFY(failure.contains(QStringLiteral("nightly")));
    // Nothing was probed: an unrecognised compositor is not asked to prove what it supports.
    QCOMPARE(server.receivedRequests(), QStringList{QStringLiteral("\"Version\"")});
}

void NiriIpcTest::detectionFailsWithoutAConnection() {
    NiriIPC client;
    QString failure;
    connect(&client, &NiriIPC::detectionFailed, this, [&failure](const QString& reason) { failure = reason; });

    client.detect();

    QVERIFY(failure.contains(QStringLiteral("no niri IPC connection is open")));
    QVERIFY(!client.isConnected());
}

void NiriIpcTest::refusesARequestNameThatIsNotAnIdentifier() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    NiriIPC client;
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    // QTRY rather than QSignalSpy::wait: a connection to a local socket can complete before wait()
    // starts listening, and wait() only notices emissions that happen after it does.
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);

    // A quote or a newline inside a name would put a second, invented line on the connection; niri
    // request names are ASCII letters, and anything else is refused instead of quoted loosely.
    const QStringList refused{
        QString(),
        QStringLiteral("Version\nCasts"),
        QStringLiteral("Ver\"sion"),
        QStringLiteral("Version 2"),
        QStringLiteral("Version;"),
    };

    for (const QString& name : refused) {
        Captured captured;
        client.send(name, [&captured](const Reply& reply) {
            captured.calls += 1;
            captured.reply = reply;
        });
        QVERIFY2(captured.calls == 1, qPrintable(QStringLiteral("queued \"%1\"").arg(name)));
        QVERIFY(!captured.reply.isOk());
        QVERIFY(!captured.reply.text.isEmpty());
    }

    // Nothing reached the wire, even after the event loop had a chance to flush it.
    QTest::qWait(50);
    QVERIFY(server.receivedRequests().isEmpty());
}

QTEST_GUILESS_MAIN(NiriIpcTest)
#include "niri_ipc_test.moc"
