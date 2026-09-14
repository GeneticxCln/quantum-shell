// Unit tests for the event stream: the subscription, the decoding of each modelled event, and what
// happens to the lines that are not one.
//
// The compositor side is FakeNiriServer, so nothing here needs a running niri. The real stream is
// checked by tests/integration/niri_live_stream_test.cpp.
#include "niri/NiriEventStream.h"
#include "niri/NiriState.h"

#include "NiriProtocolTestData.h"
#include "FakeNiriServer.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <functional>
#include <memory>

using quantum::niri::NiriEventStream;
using quantum::niri::NiriKeyboardLayouts;
using quantum::niri::NiriState;
using quantum::niri::NiriWindow;
using quantum::niri::NiriWorkspace;
using quantum::niri::isKnownEventName;
using quantum::niri::knownEventNames;
using qstest::array;
using qstest::eventLine;
using qstest::keyboardLayoutsObject;
using qstest::windowObject;
using qstest::workspaceObject;

namespace {

// Runs the event loop until `condition` holds. QTest::qWaitFor rather than a QTRY macro, so that a
// helper returning bool can wait; it also checks state instead of spotting an edge, which a
// QSignalSpy::wait cannot do for a signal that already fired.
bool waitFor(const std::function<bool()>& condition) {
    return QTest::qWaitFor(condition, 5000);
}

}  // namespace

class NiriEventStreamTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void subscribesAndReportsStreaming();
    void decodesWorkspacesChanged();
    void decodesWindowsChanged();
    void decodesWindowEvents();
    void decodesWorkspaceEvents();
    void decodesKeyboardLayoutsChanged();
    void decodesKeyboardLayoutSwitched();
    void decodesOverviewOpenedOrClosed();
    void decodesConfigLoaded();
    void reportsKnownEventsItDoesNotModel();
    void reportsNamesThatAreNotEventsOfThisProtocol();
    void reportsAnEventWhosePayloadCannotBeRead();
    void keepsStreamingAfterAProblemLine();
    void refusesAnAckThatIsAnError();
    void refusesAnUnexpectedSuccessfulAck();
    void fatalFramingFailureDisconnectsAndForgetsState();
    void theStateFollowsTheStream();
    void disconnectForgetsTheState();

private:
    std::unique_ptr<FakeNiriServer> server_;
    std::unique_ptr<NiriEventStream> stream_;
};

void NiriEventStreamTest::init() {
    server_ = std::make_unique<FakeNiriServer>();
    QVERIFY2(server_->listen(), qPrintable(server_->serverError()));
    // Captured from a running niri 26.04: the acknowledgement that precedes the event flood.
    server_->setReply(QStringLiteral("EventStream"), QByteArray(R"json({"Ok":"Handled"})json"));

    stream_ = std::make_unique<NiriEventStream>();
    stream_->connectToCompositor(server_->path());
    QVERIFY2(waitFor([this] { return stream_->isStreaming(); }),
             "the compositor never acknowledged the subscription");
}

void NiriEventStreamTest::subscribesAndReportsStreaming() {
    QVERIFY(stream_->isConnected());
    QVERIFY(stream_->isStreaming());
    QCOMPARE(server_->receivedRequests(), QStringList{QStringLiteral("\"EventStream\"")});
}

void NiriEventStreamTest::decodesWorkspacesChanged() {
    QList<NiriWorkspace> received;
    connect(stream_.get(), &NiriEventStream::workspacesChanged, this,
            [&received](const QList<NiriWorkspace>& workspaces) { received = workspaces; });

    const QJsonObject fields{
        {QStringLiteral("workspaces"),
         array({workspaceObject(11, 1, QStringLiteral("OUT-A")),
                workspaceObject(12, 2, QStringLiteral("OUT-A"), true, true)})}};
    server_->writeRaw(eventLine(QStringLiteral("WorkspacesChanged"), fields));

    QVERIFY(waitFor([&received] { return received.size() == 2; }));
    QCOMPARE(received.at(0).id(), quint64{11});
    QCOMPARE(received.at(0).idx(), 1);
    QCOMPARE(received.at(0).output(), QStringLiteral("OUT-A"));
    QVERIFY(!received.at(0).isFocused());
    QCOMPARE(received.at(1).id(), quint64{12});
    QVERIFY(received.at(1).isFocused());
    QVERIFY(received.at(1).isActive());
}

void NiriEventStreamTest::decodesWindowsChanged() {
    QList<NiriWindow> received;
    connect(stream_.get(), &NiriEventStream::windowsChanged, this,
            [&received](const QList<NiriWindow>& windows) { received = windows; });

    const QJsonObject fields{
        {QStringLiteral("windows"),
         array({windowObject(7, QStringLiteral("app.one"), 11),
                windowObject(8, QStringLiteral("app.two"), 11, true)})}};
    server_->writeRaw(eventLine(QStringLiteral("WindowsChanged"), fields));

    QVERIFY(waitFor([&received] { return received.size() == 2; }));
    QCOMPARE(received.at(0).id(), quint64{7});
    QCOMPARE(received.at(0).appId(), QStringLiteral("app.one"));
    QCOMPARE(received.at(0).workspaceId().value_or(0), quint64{11});
    QVERIFY(!received.at(0).isFocused());
    QVERIFY(received.at(1).isFocused());
}

void NiriEventStreamTest::decodesWindowEvents() {
    NiriWindow opened;
    quint64 closed = 0;
    quint64 urgentId = 0;
    bool urgent = false;
    quint64 focusedId = 0;
    bool hasFocused = true;

    connect(stream_.get(), &NiriEventStream::windowOpenedOrChanged, this,
            [&opened](const NiriWindow& window) { opened = window; });
    connect(stream_.get(), &NiriEventStream::windowClosed, this, [&closed](quint64 id) { closed = id; });
    connect(stream_.get(), &NiriEventStream::windowUrgencyChanged, this,
            [&urgentId, &urgent](quint64 id, bool value) {
                urgentId = id;
                urgent = value;
            });
    connect(stream_.get(), &NiriEventStream::focusedWindowChanged, this,
            [&focusedId, &hasFocused](quint64 id, bool present) {
                focusedId = id;
                hasFocused = present;
            });

    server_->writeRaw(eventLine(QStringLiteral("WindowOpenedOrChanged"),
                                QJsonObject{{QStringLiteral("window"),
                                             windowObject(9, QStringLiteral("app.three"), 11, true)}}));
    QVERIFY(waitFor([&opened] { return opened.isValid(); }));
    QCOMPARE(opened.id(), quint64{9});
    QVERIFY(opened.isFocused());
    QCOMPARE(opened.title(), QStringLiteral("title of app.three"));

    server_->writeRaw(eventLine(QStringLiteral("WindowClosed"), QJsonObject{{QStringLiteral("id"), 9}}));
    QVERIFY(waitFor([&closed] { return closed == 9; }));

    server_->writeRaw(eventLine(QStringLiteral("WindowUrgencyChanged"),
                                QJsonObject{{QStringLiteral("id"), 7}, {QStringLiteral("urgent"), true}}));
    QVERIFY(waitFor([&urgentId] { return urgentId == 7; }));
    QVERIFY(urgent);

    server_->writeRaw(eventLine(QStringLiteral("WindowFocusChanged"), QJsonObject{{QStringLiteral("id"), 8}}));
    QVERIFY(waitFor([&focusedId] { return focusedId == 8; }));
    QVERIFY(hasFocused);

    // A null id means no window is focused, which niri documents as a real state rather than an
    // absent field.
    server_->writeRaw(
        eventLine(QStringLiteral("WindowFocusChanged"), QJsonObject{{QStringLiteral("id"), QJsonValue()}}));
    QVERIFY(waitFor([&hasFocused] { return !hasFocused; }));
}

void NiriEventStreamTest::decodesWorkspaceEvents() {
    quint64 activated = 0;
    bool focused = false;
    quint64 urgentId = 0;
    bool urgent = false;
    quint64 activeWorkspace = 0;
    quint64 activeWindow = 0;
    bool hasActiveWindow = false;

    connect(stream_.get(), &NiriEventStream::workspaceActivated, this,
            [&activated, &focused](quint64 id, bool value) {
                activated = id;
                focused = value;
            });
    connect(stream_.get(), &NiriEventStream::workspaceUrgencyChanged, this,
            [&urgentId, &urgent](quint64 id, bool value) {
                urgentId = id;
                urgent = value;
            });
    connect(stream_.get(), &NiriEventStream::workspaceActiveWindowChanged, this,
            [&activeWorkspace, &activeWindow, &hasActiveWindow](quint64 workspace, quint64 window, bool present) {
                activeWorkspace = workspace;
                activeWindow = window;
                hasActiveWindow = present;
            });

    server_->writeRaw(eventLine(QStringLiteral("WorkspaceActivated"),
                                QJsonObject{{QStringLiteral("id"), 12}, {QStringLiteral("focused"), true}}));
    QVERIFY(waitFor([&activated] { return activated == 12; }));
    QVERIFY(focused);

    server_->writeRaw(eventLine(QStringLiteral("WorkspaceUrgencyChanged"),
                                QJsonObject{{QStringLiteral("id"), 11}, {QStringLiteral("urgent"), true}}));
    QVERIFY(waitFor([&urgentId] { return urgentId == 11; }));
    QVERIFY(urgent);

    server_->writeRaw(eventLine(
        QStringLiteral("WorkspaceActiveWindowChanged"),
        QJsonObject{{QStringLiteral("workspace_id"), 11}, {QStringLiteral("active_window_id"), 7}}));
    QVERIFY(waitFor([&activeWorkspace] { return activeWorkspace == 11; }));
    QCOMPARE(activeWindow, quint64{7});
    QVERIFY(hasActiveWindow);

    server_->writeRaw(eventLine(
        QStringLiteral("WorkspaceActiveWindowChanged"),
        QJsonObject{{QStringLiteral("workspace_id"), 11}, {QStringLiteral("active_window_id"), QJsonValue()}}));
    QVERIFY(waitFor([&hasActiveWindow] { return !hasActiveWindow; }));
}

void NiriEventStreamTest::decodesKeyboardLayoutsChanged() {
    NiriKeyboardLayouts received;
    connect(stream_.get(), &NiriEventStream::keyboardLayoutsChanged, this,
            [&received](const NiriKeyboardLayouts& layouts) { received = layouts; });

    server_->writeRaw(eventLine(
        QStringLiteral("KeyboardLayoutsChanged"),
        QJsonObject{{QStringLiteral("keyboard_layouts"),
                     keyboardLayoutsObject({QStringLiteral("English (US)"), QStringLiteral("German")}, 1)}}));

    QVERIFY(waitFor([&received] { return received.isValid(); }));
    QCOMPARE(received.size(), 2);
    QCOMPARE(received.currentName(), QStringLiteral("German"));
    QVERIFY(stream_->unmodelledEvents().isEmpty());

    // Without the layouts themselves there is nothing to point at, and saying so beats an event that
    // silently changed nothing.
    QString malformed;
    connect(stream_.get(), &NiriEventStream::malformedEvent, this,
            [&malformed](const QString& name, const QString& reason) {
                malformed = QStringLiteral("%1: %2").arg(name, reason);
            });
    server_->writeRaw(eventLine(QStringLiteral("KeyboardLayoutsChanged"), QJsonObject{}));
    QVERIFY(waitFor([&malformed] { return !malformed.isEmpty(); }));
    QVERIFY(malformed.startsWith(QStringLiteral("KeyboardLayoutsChanged")));
}

void NiriEventStreamTest::decodesKeyboardLayoutSwitched() {
    int index = -1;
    connect(stream_.get(), &NiriEventStream::keyboardLayoutSwitched, this,
            [&index](int switched) { index = switched; });

    server_->writeRaw(eventLine(QStringLiteral("KeyboardLayoutSwitched"),
                                QJsonObject{{QStringLiteral("idx"), 2}}));

    QVERIFY(waitFor([&index] { return index >= 0; }));
    QCOMPARE(index, 2);
    QVERIFY(stream_->unmodelledEvents().isEmpty());
}

void NiriEventStreamTest::decodesOverviewOpenedOrClosed() {
    bool open = false;
    bool seen = false;
    connect(stream_.get(), &NiriEventStream::overviewOpenedOrClosed, this,
            [&open, &seen](bool isOpen) {
                open = isOpen;
                seen = true;
            });

    server_->writeRaw(eventLine(QStringLiteral("OverviewOpenedOrClosed"),
                                QJsonObject{{QStringLiteral("is_open"), true}}));
    QVERIFY(waitFor([&seen] { return seen; }));
    QVERIFY(open);

    // Both directions are reported, so a closed overview is not just "no event".
    server_->writeRaw(eventLine(QStringLiteral("OverviewOpenedOrClosed"),
                                QJsonObject{{QStringLiteral("is_open"), false}}));
    QVERIFY(waitFor([&open] { return !open; }));
}

void NiriEventStreamTest::decodesConfigLoaded() {
    bool seen = false;
    bool failed = false;
    connect(stream_.get(), &NiriEventStream::configLoaded, this,
            [&seen, &failed](bool value) {
                seen = true;
                failed = value;
            });

    server_->writeRaw(eventLine(QStringLiteral("ConfigLoaded"),
                                QJsonObject{{QStringLiteral("failed"), true}}));
    QVERIFY(waitFor([&seen] { return seen; }));
    QVERIFY(failed);

    // A successful load is reported as a successful load, not as nothing having arrived.
    seen = false;
    server_->writeRaw(eventLine(QStringLiteral("ConfigLoaded"),
                                QJsonObject{{QStringLiteral("failed"), false}}));
    QVERIFY(waitFor([&seen] { return seen; }));
    QVERIFY(!failed);
}

void NiriEventStreamTest::reportsKnownEventsItDoesNotModel() {
    QString reported;
    connect(stream_.get(), &NiriEventStream::unmodelledEvent, this,
            [&reported](const QString& name) { reported = name; });

    server_->writeRaw(eventLine(QStringLiteral("CastsChanged"),
                                QJsonObject{{QStringLiteral("casts"), QJsonArray{}}}));

    QVERIFY(waitFor([&reported] { return !reported.isEmpty(); }));
    // Named, not dropped: a gap this build knows about is not the same as an event that vanished.
    QCOMPARE(reported, QStringLiteral("CastsChanged"));
    QVERIFY(isKnownEventName(reported));
    QCOMPARE(stream_->unmodelledEvents(), QStringList{QStringLiteral("CastsChanged")});
    QVERIFY(stream_->unknownEvents().isEmpty());
}

void NiriEventStreamTest::reportsNamesThatAreNotEventsOfThisProtocol() {
    QString reported;
    connect(stream_.get(), &NiriEventStream::unknownEvent, this,
            [&reported](const QString& name) { reported = name; });

    server_->writeRaw(eventLine(QStringLiteral("QuantumShellProbe"), QJsonObject{}));

    QVERIFY(waitFor([&reported] { return !reported.isEmpty(); }));
    QCOMPARE(reported, QStringLiteral("QuantumShellProbe"));
    // Reported separately from the events this protocol version defines but this build ignores:
    // this is the compositor saying the protocol moved.
    QVERIFY(!isKnownEventName(reported));
    QVERIFY(knownEventNames().size() >= 19);
    QCOMPARE(stream_->unknownEvents(), QStringList{QStringLiteral("QuantumShellProbe")});
    QVERIFY(stream_->unmodelledEvents().isEmpty());
}

void NiriEventStreamTest::reportsAnEventWhosePayloadCannotBeRead() {
    QString name;
    QString reason;
    connect(stream_.get(), &NiriEventStream::malformedEvent, this,
            [&name, &reason](const QString& eventName, const QString& eventReason) {
                name = eventName;
                reason = eventReason;
            });

    // WindowClosed without the window it closed: a modelled event that cannot be acted on. Silence
    // here would look exactly like a window that was never closed.
    server_->writeRaw(eventLine(QStringLiteral("WindowClosed"), QJsonObject{}));

    QVERIFY(waitFor([&name] { return !name.isEmpty(); }));
    QCOMPARE(name, QStringLiteral("WindowClosed"));
    QVERIFY(!reason.isEmpty());

    // The same for a line that is not a single-event object at all.
    name.clear();
    server_->writeRaw(QByteArray(R"json({"WindowClosed":{"id":1},"WindowFocusChanged":{"id":null}})json") + '\n');
    QVERIFY(waitFor([&name] { return !name.isEmpty(); }));
    QCOMPARE(name, QStringLiteral("<line>"));
}

void NiriEventStreamTest::keepsStreamingAfterAProblemLine() {
    QList<NiriWindow> received;
    connect(stream_.get(), &NiriEventStream::windowsChanged, this,
            [&received](const QList<NiriWindow>& windows) { received = windows; });

    server_->writeRaw(QByteArray("this is not json\n"));
    server_->writeRaw(eventLine(QStringLiteral("WindowClosed"), QJsonObject{}));  // malformed too
    server_->writeRaw(eventLine(
        QStringLiteral("WindowsChanged"),
        QJsonObject{{QStringLiteral("windows"), array({windowObject(7, QStringLiteral("app.one"), 11)})}}));

    QVERIFY(waitFor([&received] { return received.size() == 1; }));
    QCOMPARE(received.at(0).id(), quint64{7});
    QVERIFY(stream_->isStreaming());
}

void NiriEventStreamTest::refusesAnAckThatIsAnError() {
    // A compositor without the event stream answers the subscription with an error. That is not a
    // running stream, and saying so is the whole point of the acknowledgement.
    server_->setReply(QStringLiteral("EventStream"),
                      QByteArray(R"json({"Err":"error parsing request"})json"));

    auto stream = std::make_unique<NiriEventStream>();
    QString failure;
    bool streamed = false;
    connect(stream.get(), &NiriEventStream::streamFailed, this,
            [&failure](const QString& reason) { failure = reason; });
    connect(stream.get(), &NiriEventStream::streaming, this, [&streamed] { streamed = true; });

    stream->connectToCompositor(server_->path());

    QVERIFY(waitFor([&failure] { return !failure.isEmpty(); }));
    QVERIFY(!streamed);
    QVERIFY(!stream->isStreaming());
    QVERIFY(failure.contains(QStringLiteral("error parsing request")));
}

void NiriEventStreamTest::refusesAnUnexpectedSuccessfulAck() {
    for (const QByteArray& reply : {QByteArray(R"json({"Ok":"wrong"})json"),
                                   QByteArray(R"json({"Ok":{}})json"),
                                   QByteArray(R"json({"Ok":null})json")}) {
        FakeNiriServer server;
        QVERIFY2(server.listen(), qPrintable(server.serverError()));
        server.setReply(QStringLiteral("EventStream"), reply);
        NiriEventStream stream;
        QSignalSpy failed(&stream, &NiriEventStream::streamFailed);
        QSignalSpy streamed(&stream, &NiriEventStream::streaming);
        stream.connectToCompositor(server.path());
        QVERIFY(waitFor([&failed, &streamed] { return !failed.isEmpty() || !streamed.isEmpty(); }));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(streamed.count(), 0);
        QVERIFY(!stream.isStreaming());
        QVERIFY(!stream.isConnected());
    }
}

void NiriEventStreamTest::fatalFramingFailureDisconnectsAndForgetsState() {
    NiriState state;
    state.observe(*stream_);
    server_->writeRaw(eventLine(
        QStringLiteral("WindowsChanged"),
        QJsonObject{{QStringLiteral("windows"), array({windowObject(7, QStringLiteral("app.one"), 11)})}}));
    QVERIFY(waitFor([&state] { return state.windows().size() == 1; }));

    QSignalSpy failed(stream_.get(), &NiriEventStream::streamFailed);
    QSignalSpy disconnected(stream_.get(), &NiriEventStream::disconnected);
    server_->writeRaw(QByteArray(quantum::niri::maximumLineBytes + 1, 'x'));
    QVERIFY(waitFor([&failed] { return !failed.isEmpty(); }));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(disconnected.count(), 1);
    QVERIFY(!stream_->isStreaming());
    QVERIFY(!stream_->isConnected());
    QVERIFY(state.windows().isEmpty());
}

void NiriEventStreamTest::theStateFollowsTheStream() {
    NiriState state;
    state.observe(*stream_);

    server_->writeRaw(eventLine(
        QStringLiteral("WorkspacesChanged"),
        QJsonObject{{QStringLiteral("workspaces"),
                     array({workspaceObject(11, 1, QStringLiteral("OUT-A"), true, true),
                            workspaceObject(12, 2, QStringLiteral("OUT-A"))})}}));
    server_->writeRaw(eventLine(
        QStringLiteral("WindowsChanged"),
        QJsonObject{{QStringLiteral("windows"),
                     array({windowObject(7, QStringLiteral("app.one"), 11),
                            windowObject(8, QStringLiteral("app.two"), 11)})}}));

    QVERIFY(waitFor([&state] { return state.workspaces().size() == 2 && state.windows().size() == 2; }));
    QCOMPARE(state.focusedWorkspace().id(), quint64{11});

    // No window is focused yet; the focus event is what says which is.
    QVERIFY(!state.focusedWindow().isValid());
    server_->writeRaw(eventLine(QStringLiteral("WindowFocusChanged"), QJsonObject{{QStringLiteral("id"), 8}}));
    QVERIFY(waitFor([&state] { return state.focusedWindow().isValid(); }));
    QCOMPARE(state.focusedWindow().id(), quint64{8});

    server_->writeRaw(eventLine(QStringLiteral("WindowClosed"), QJsonObject{{QStringLiteral("id"), 8}}));
    QVERIFY(waitFor([&state] { return state.windows().size() == 1; }));
    QVERIFY(!state.focusedWindow().isValid());

    // The rest of the modelled events reach the model through the same single observe() call, so a
    // consumer cannot half-wire the stream and end up with a model that is quietly stale.
    server_->writeRaw(eventLine(
        QStringLiteral("KeyboardLayoutsChanged"),
        QJsonObject{{QStringLiteral("keyboard_layouts"),
                     keyboardLayoutsObject({QStringLiteral("us"), QStringLiteral("de")}, 0)}}));
    QVERIFY(waitFor([&state] { return state.keyboardLayouts().isValid(); }));
    server_->writeRaw(eventLine(QStringLiteral("KeyboardLayoutSwitched"),
                                QJsonObject{{QStringLiteral("idx"), 1}}));
    QVERIFY(waitFor([&state] { return state.keyboardLayouts().currentName() == QStringLiteral("de"); }));

    server_->writeRaw(eventLine(QStringLiteral("OverviewOpenedOrClosed"),
                                QJsonObject{{QStringLiteral("is_open"), true}}));
    QVERIFY(waitFor([&state] { return state.isOverviewOpen(); }));

    server_->writeRaw(eventLine(QStringLiteral("ConfigLoaded"),
                                QJsonObject{{QStringLiteral("failed"), true}}));
    QVERIFY(waitFor([&state] { return state.configLoadFailed(); }));
}

void NiriEventStreamTest::disconnectForgetsTheState() {
    NiriState state;
    state.observe(*stream_);

    server_->writeRaw(eventLine(
        QStringLiteral("WindowsChanged"),
        QJsonObject{{QStringLiteral("windows"), array({windowObject(7, QStringLiteral("app.one"), 11)})}}));
    QVERIFY(waitFor([&state] { return state.windows().size() == 1; }));

    QSignalSpy disconnected(stream_.get(), &NiriEventStream::disconnected);
    server_->closeConnections();

    QVERIFY(waitFor([&disconnected] { return disconnected.count() > 0; }));
    // Nothing about the compositor's state is known once the compositor is gone.
    QVERIFY(state.windows().isEmpty());
    QVERIFY(state.workspaces().isEmpty());
    QVERIFY(!stream_->isStreaming());
}

QTEST_GUILESS_MAIN(NiriEventStreamTest)
#include "niri_event_stream_test.moc"
