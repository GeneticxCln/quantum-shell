// Unit tests for the action layer: the JSON each action puts on the wire, and what each kind of answer
// becomes.
//
// The compositor's end of the socket is FakeNiriServer, so no action here is performed. The actions
// that can be performed without changing anything are exercised against the running compositor by
// tests/integration/niri_live_action_test.cpp; the rest have effects (running a process, moving a
// window, taking a screenshot) and are pinned here by shape instead.
#include "niri/NiriActions.h"

#include "niri/NiriIPC.h"

#include "FakeNiriServer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSignalSpy>
#include <QTest>

#include <optional>

using quantum::niri::NiriActions;
using quantum::niri::NiriIPC;
using quantum::niri::Reply;

namespace {

// What a handler saw, so a test can assert after the event loop delivered the reply.
struct Captured {
    int calls = 0;
    NiriActions::Result result;
};

bool connectTo(NiriIPC& client, FakeNiriServer& server) {
    QSignalSpy connected(&client, &NiriIPC::connected);
    client.connectToCompositor(server.path());
    return QTest::qWaitFor([&connected] { return connected.count() == 1; }, 5000);
}

// The last request line as JSON. An unparseable line gives an empty object, which fails the comparison
// in the test that asked for it rather than being hidden.
QJsonObject lastRequest(const FakeNiriServer& server) {
    if (server.receivedRequests().isEmpty()) {
        return QJsonObject{};
    }
    return QJsonDocument::fromJson(server.receivedRequests().last().toUtf8()).object();
}

// The action's own fields, unwrapped from the `{"Action":{...}}` request shape.
QJsonObject actionFields(const FakeNiriServer& server) {
    return lastRequest(server).value(QStringLiteral("Action")).toObject();
}

}  // namespace

class NiriActionsTest : public QObject {
    Q_OBJECT

private slots:
    void sendsEachActionInTheShapeNiriParses();
    void sendsAWorkspaceReferenceInEachOfItsThreeForms();
    void reportsHandledWhenTheCompositorPerformsTheAction();
    void reportsRefusedWhenTheCompositorCannotParseTheAction();
    void reportsNotDeliveredWhenTheConnectionGoesAway();
    void reportsAnOkReplyThatIsNotHandledAsUnexpected();
    void refusesWhatItCannotEncodeWithoutSendingAnything();
    void emitsEveryFailureEvenWhenNoHandlerWasGiven();
    void keepsActionsInOrderWithTheRequestsAroundThem();
};

void NiriActionsTest::sendsEachActionInTheShapeNiriParses() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    // Captured: what niri answers an action it performed.
    server.setReply(QStringLiteral("Action"), QByteArray(R"json({"Ok":"Handled"})json"));

    NiriIPC client;
    NiriActions actions(client);
    QVERIFY2(connectTo(client, server), "the request connection never came up");

    Captured captured;

    actions.focusWorkspace(NiriActions::WorkspaceReference::atIndex(2),
                           [&captured](const NiriActions::Result& result) {
                               captured.calls += 1;
                               captured.result = result;
                           });
    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 1, 5000);
    // This exact shape is printed in niri's own IPC documentation, for `focus-workspace 2`.
    QCOMPARE(server.receivedRequests().last(),
             QStringLiteral(R"({"Action":{"FocusWorkspace":{"reference":{"Index":2}}}})"));
    const QJsonObject reference{ {QStringLiteral("Index"), 2} };
    const QJsonObject focusWorkspace{ {QStringLiteral("FocusWorkspace"),
                                      QJsonObject{{QStringLiteral("reference"), reference}}} };
    QCOMPARE(actionFields(server), focusWorkspace);
    QVERIFY(captured.result.isHandled());

    actions.spawn({QStringLiteral("foot"), QStringLiteral("-e"), QStringLiteral("htop")},
                  [&captured](const NiriActions::Result&) { captured.calls += 1; });
    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 2, 5000);
    const QJsonObject spawn{
        {QStringLiteral("Spawn"),
         QJsonObject{{QStringLiteral("command"), QJsonArray{QStringLiteral("foot"),
                                                            QStringLiteral("-e"),
                                                            QStringLiteral("htop")}}}} };
    QCOMPARE(actionFields(server), spawn);

    // The shell form, where the string is handed to a shell rather than exec'd directly.
    actions.spawnSh(QStringLiteral("swaybg -i wall.png &"), [](const NiriActions::Result&) {});
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedRequests().size() == 3, 5000);
    const QJsonObject spawnSh{
        {QStringLiteral("SpawnSh"),
         QJsonObject{{QStringLiteral("command"), QStringLiteral("swaybg -i wall.png &")}}} };
    QCOMPARE(actionFields(server), spawnSh);

    actions.moveWindowToWorkspace(NiriActions::WorkspaceReference::atIndex(3), true, 12,
                                  [](const NiriActions::Result&) {});
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedRequests().size() == 4, 5000);
    const QJsonObject moveByIndex{
        {QStringLiteral("MoveWindowToWorkspace"),
         QJsonObject{{QStringLiteral("reference"), QJsonObject{{QStringLiteral("Index"), 3}}},
                     {QStringLiteral("focus"), true},
                     {QStringLiteral("window_id"), 12}}} };
    QCOMPARE(actionFields(server), moveByIndex);

    // No id means the focused window; the field is sent as null rather than left out, because null is
    // what serde reads as None.
    actions.moveWindowToWorkspace(NiriActions::WorkspaceReference::named(QStringLiteral("main")), false,
                                  std::nullopt, [](const NiriActions::Result&) {});
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedRequests().size() == 5, 5000);
    const QJsonObject moveByName{
        {QStringLiteral("MoveWindowToWorkspace"),
         QJsonObject{{QStringLiteral("reference"),
                      QJsonObject{{QStringLiteral("Name"), QStringLiteral("main")}}},
                     {QStringLiteral("focus"), false},
                     {QStringLiteral("window_id"), QJsonValue()}}} };
    QCOMPARE(actionFields(server), moveByName);

    actions.screenshotScreen(true, false, [](const NiriActions::Result&) {});
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedRequests().size() == 6, 5000);
    const QJsonObject screenshot{
        {QStringLiteral("ScreenshotScreen"),
         QJsonObject{{QStringLiteral("write_to_disk"), true},
                     {QStringLiteral("show_pointer"), false},
                     {QStringLiteral("path"), QJsonValue()}}} };
    QCOMPARE(actionFields(server), screenshot);

    actions.openOverview([](const NiriActions::Result&) {});
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedRequests().size() == 7, 5000);
    const QJsonObject openOverview{
        {QStringLiteral("OpenOverview"), QJsonObject{}} };
    QCOMPARE(actionFields(server), openOverview);

    actions.closeOverview([](const NiriActions::Result&) {});
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedRequests().size() == 8, 5000);
    const QJsonObject closeOverview{
        {QStringLiteral("CloseOverview"), QJsonObject{}} };
    QCOMPARE(actionFields(server), closeOverview);

    actions.switchLayout(NiriActions::LayoutTarget::next(), [](const NiriActions::Result&) {});
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedRequests().size() == 9, 5000);
    const QJsonObject switchNext{
        {QStringLiteral("SwitchLayout"),
         QJsonObject{{QStringLiteral("layout"), QStringLiteral("Next")}}} };
    QCOMPARE(actionFields(server), switchNext);

    actions.switchLayout(NiriActions::LayoutTarget::atIndex(1), [](const NiriActions::Result&) {});
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedRequests().size() == 10, 5000);
    const QJsonObject switchByIndex{
        {QStringLiteral("SwitchLayout"),
         QJsonObject{{QStringLiteral("layout"), QJsonObject{{QStringLiteral("Index"), 1}}}}} };
    QCOMPARE(actionFields(server), switchByIndex);

    // Every one of those went out as a single line: niri reads one request per line, and a request
    // split across two would have shifted every reply after it.
    QCOMPARE(server.receivedRequests().size(), 10);
    for (const QString& line : server.receivedRequests()) {
        QVERIFY2(!line.contains(QLatin1Char('\n')), qPrintable(line));
    }
}

void NiriActionsTest::sendsAWorkspaceReferenceInEachOfItsThreeForms() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("Action"), QByteArray(R"json({"Ok":"Handled"})json"));

    NiriIPC client;
    NiriActions actions(client);
    QVERIFY2(connectTo(client, server), "the request connection never came up");

    // niri-ipc v26.04's `WorkspaceReferenceArg` has exactly three variants, and each is a different
    // JSON shape: Index and Id by number, Name by string.
    const QList<QPair<NiriActions::WorkspaceReference, QJsonObject>> references{
        {NiriActions::WorkspaceReference::atIndex(0), QJsonObject{{QStringLiteral("Index"), 0}}},
        {NiriActions::WorkspaceReference::withId(8), QJsonObject{{QStringLiteral("Id"), 8}}},
        {NiriActions::WorkspaceReference::named(QStringLiteral("browser")),
         QJsonObject{{QStringLiteral("Name"), QStringLiteral("browser")}}},
    };

    int sent = 0;
    for (const auto& [reference, expected] : references) {
        QVERIFY(reference.isValid());
        QCOMPARE(reference.toJson(), expected);
        actions.focusWorkspace(reference, [](const NiriActions::Result&) {});
        ++sent;
        QTRY_VERIFY_WITH_TIMEOUT(server.receivedRequests().size() == sent, 5000);
        QCOMPARE(actionFields(server).value(QStringLiteral("FocusWorkspace")).toObject()
                     .value(QStringLiteral("reference")).toObject(),
                 expected);
    }
    QCOMPARE(server.receivedRequests().size(), 3);
}

void NiriActionsTest::reportsHandledWhenTheCompositorPerformsTheAction() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("Action"), QByteArray(R"json({"Ok":"Handled"})json"));

    NiriIPC client;
    NiriActions actions(client);
    QVERIFY2(connectTo(client, server), "the request connection never came up");

    Captured captured;
    QStringList failures;
    connect(&actions, &NiriActions::actionFailed, this,
            [&failures](const QString& action, const NiriActions::Result&) { failures.append(action); });

    actions.openOverview([&captured](const NiriActions::Result& result) {
        captured.calls += 1;
        captured.result = result;
    });

    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 1, 5000);
    QVERIFY(captured.result.isHandled());
    QCOMPARE(captured.result.outcome, NiriActions::Result::Outcome::handled);
    QCOMPARE(captured.result.describe(), QStringLiteral("handled"));
    // A performed action is not a failure, so nothing is reported as one.
    QVERIFY(failures.isEmpty());
}

void NiriActionsTest::reportsRefusedWhenTheCompositorCannotParseTheAction() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    // Captured: what niri answers a request it cannot parse — which is what an action name outside its
    // enum, or a field of the wrong type, produces.
    server.setReply(QStringLiteral("Action"), QByteArray(R"json({"Err":"error parsing request"})json"));

    NiriIPC client;
    NiriActions actions(client);
    QVERIFY2(connectTo(client, server), "the request connection never came up");

    Captured captured;
    QStringList failedActions;
    NiriActions::Result reported;
    connect(&actions, &NiriActions::actionFailed, this,
            [&failedActions, &reported](const QString& action, const NiriActions::Result& result) {
                failedActions.append(action);
                reported = result;
            });

    actions.spawn({QStringLiteral("foot")}, [&captured](const NiriActions::Result& result) {
        captured.calls += 1;
        captured.result = result;
    });

    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 1, 5000);
    QVERIFY(!captured.result.isHandled());
    QCOMPARE(captured.result.outcome, NiriActions::Result::Outcome::refused);
    QCOMPARE(captured.result.detail, QStringLiteral("error parsing request"));
    QCOMPARE(captured.result.describe(), QStringLiteral("refused: error parsing request"));

    // The failure is reported by name as well as to the handler.
    QCOMPARE(failedActions, QStringList{QStringLiteral("Spawn")});
    QCOMPARE(reported.outcome, NiriActions::Result::Outcome::refused);
}

void NiriActionsTest::reportsNotDeliveredWhenTheConnectionGoesAway() {
    NiriIPC client;
    NiriActions actions(client);

    Captured captured;
    actions.focusWorkspace(NiriActions::WorkspaceReference::atIndex(1),
                           [&captured](const NiriActions::Result& result) {
                               captured.calls += 1;
                               captured.result = result;
                           });

    QStringList failures;
    connect(&actions, &NiriActions::actionFailed, this,
            [&failures](const QString& action, const NiriActions::Result&) { failures.append(action); });

    // Queued before there was a connection, and the connection cannot be made.
    client.connectToCompositor(QStringLiteral("/nonexistent/niri-actions-test.sock"));

    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 1, 5000);
    QVERIFY(!captured.result.isHandled());
    QCOMPARE(captured.result.outcome, NiriActions::Result::Outcome::notDelivered);
    QVERIFY(!captured.result.detail.isEmpty());
    QVERIFY(captured.result.describe().startsWith(QStringLiteral("not delivered")));
    QCOMPARE(failures, QStringList{QStringLiteral("FocusWorkspace")});
}

void NiriActionsTest::reportsAnOkReplyThatIsNotHandledAsUnexpected() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    // An ok reply that is not the `Handled` response this protocol defines: a compositor that answered
    // something else has not told us the action was performed, and that must not read as success.
    server.setReply(QStringLiteral("Action"),
                    QByteArray(R"json({"Ok":{"SomethingElse":true}})json"));

    NiriIPC client;
    NiriActions actions(client);
    QVERIFY2(connectTo(client, server), "the request connection never came up");

    Captured captured;
    actions.closeOverview([&captured](const NiriActions::Result& result) {
        captured.calls += 1;
        captured.result = result;
    });

    QTRY_VERIFY_WITH_TIMEOUT(captured.calls == 1, 5000);
    QVERIFY(!captured.result.isHandled());
    QCOMPARE(captured.result.outcome, NiriActions::Result::Outcome::unexpected);
    QVERIFY(!captured.result.detail.isEmpty());
}

void NiriActionsTest::refusesWhatItCannotEncodeWithoutSendingAnything() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("Action"), QByteArray(R"json({"Ok":"Handled"})json"));

    NiriIPC client;
    NiriActions actions(client);
    QVERIFY2(connectTo(client, server), "the request connection never came up");

    QStringList failures;
    connect(&actions, &NiriActions::actionFailed, this,
            [&failures](const QString& action, const NiriActions::Result&) { failures.append(action); });

    // A workspace index is a `u8` on niri's side, so an index above 255 is not a workspace this build
    // can name; truncating it would silently target a different one. An empty name names nothing, and
    // an empty command runs nothing.
    Captured captured;
    actions.focusWorkspace(NiriActions::WorkspaceReference::atIndex(300),
                           [&captured](const NiriActions::Result& result) {
                               captured.calls += 1;
                               captured.result = result;
                           });
    QCOMPARE(captured.calls, 1);
    QCOMPARE(captured.result.outcome, NiriActions::Result::Outcome::notDelivered);
    QVERIFY(captured.result.detail.contains(QStringLiteral("between 0 and 255")));

    actions.moveWindowToWorkspace(NiriActions::WorkspaceReference::named(QString()), true,
                                  std::nullopt, [&captured](const NiriActions::Result& result) {
                                      captured.calls += 1;
                                      captured.result = result;
                                  });
    QCOMPARE(captured.calls, 2);
    QCOMPARE(captured.result.outcome, NiriActions::Result::Outcome::notDelivered);

    actions.spawn({}, [](const NiriActions::Result&) {});
    actions.spawnSh(QString(), [](const NiriActions::Result&) {});
    actions.switchLayout(NiriActions::LayoutTarget::atIndex(999), [](const NiriActions::Result&) {});

    QCOMPARE(failures,
             (QStringList{QStringLiteral("FocusWorkspace"),
                          QStringLiteral("MoveWindowToWorkspace"), QStringLiteral("Spawn"),
                          QStringLiteral("SpawnSh"), QStringLiteral("SwitchLayout")}));

    // Nothing at all reached the wire: these were refused here, not by the compositor.
    QTest::qWait(50);
    QVERIFY(server.receivedRequests().isEmpty());
}

void NiriActionsTest::emitsEveryFailureEvenWhenNoHandlerWasGiven() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("Action"), QByteArray(R"json({"Err":"action failed"})json"));

    NiriIPC client;
    NiriActions actions(client);
    QVERIFY2(connectTo(client, server), "the request connection never came up");

    QStringList failures;
    QString reason;
    connect(&actions, &NiriActions::actionFailed, this,
            [&failures, &reason](const QString& action, const NiriActions::Result& result) {
                failures.append(action);
                reason = result.describe();
            });

    // No handler: a caller with nothing to do with the answer still hears about a failure.
    actions.openOverview();

    QTRY_VERIFY_WITH_TIMEOUT(failures.size() == 1, 5000);
    QCOMPARE(failures.first(), QStringLiteral("OpenOverview"));
    QCOMPARE(reason, QStringLiteral("refused: action failed"));
}

void NiriActionsTest::keepsActionsInOrderWithTheRequestsAroundThem() {
    FakeNiriServer server;
    QVERIFY2(server.listen(), qPrintable(server.serverError()));
    server.setReply(QStringLiteral("Version"),
                    QByteArray(R"json({"Ok":{"Version":"26.04 (8ed0da4)"}})json"));
    server.setReply(QStringLiteral("Action"), QByteArray(R"json({"Ok":"Handled"})json"));
    server.setReply(QStringLiteral("Casts"), QByteArray(R"json({"Ok":{"Casts":[]}})json"));

    NiriIPC client;
    NiriActions actions(client);
    QVERIFY2(connectTo(client, server), "the request connection never came up");

    // One connection carries both: a read, an action, and another read, each answered on its own.
    Reply version;
    Captured moved;
    Reply casts;
    client.send(QStringLiteral("Version"),
                [&version](const Reply& reply) { version = reply; });
    actions.focusWorkspace(NiriActions::WorkspaceReference::atIndex(1),
                           [&moved](const NiriActions::Result& result) {
                               moved.calls += 1;
                               moved.result = result;
                           });
    client.send(QStringLiteral("Casts"), [&casts](const Reply& reply) { casts = reply; });

    QTRY_VERIFY_WITH_TIMEOUT(moved.calls == 1, 5000);
    QVERIFY(moved.result.isHandled());
    QVERIFY(casts.isOk());
    QVERIFY(casts.variant(QStringLiteral("Casts")).isArray());
    QCOMPARE(version.variant(QStringLiteral("Version")).toString(),
             QStringLiteral("26.04 (8ed0da4)"));
    // The action did not shift what the reads around it were answered with.
    QVERIFY(version.variant(QStringLiteral("Casts")).isUndefined());
    QVERIFY(casts.variant(QStringLiteral("Version")).isUndefined());
    QCOMPARE(server.receivedRequests().size(), 3);
}

QTEST_GUILESS_MAIN(NiriActionsTest)
#include "niri_actions_test.moc"
