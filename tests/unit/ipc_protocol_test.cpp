// The IPC wire format, driven without a socket.
//
// Everything here is a pure function of one line, which is the point of the design: the format can be
// tested — including every way it refuses a frame — before anything binds a socket. The socket itself is
// `ipc-server-test`'s subject.
//
// Two guards are in this file rather than in the server. The first is the mirror of the frozen verb list: it
// is written out again here, independently, and compared at compile time, so renaming a verb in
// `IPCProtocol.h` does not build until this file agrees — the same pattern `NiriServiceKeys.h` uses, and for
// the same reason, because these names are what a script and an external widget are written against.
//
// The second is the protocol version. It is pinned to a literal here on purpose: a version is the promise
// that a frame means the same thing, so raising it must be a deliberate edit that says so, not a constant
// that follows the code.
#include "ipc/IPCProtocol.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <array>
#include <string_view>

namespace {

// The verbs, written out as the documents spell them. `AGENTS.md` freezes them and `QUANTUM_SHELL.md` § IPC
// is where they are designed, so a change here is a change to a document as well as to the code.
constexpr std::array<const char*, 4> coveredVerbs{"version", "state", "config get", "bar toggle"};

template <std::size_t Declared, std::size_t Covered>
constexpr bool sameVerbs(const std::array<const char*, Declared>& declared,
                         const std::array<const char*, Covered>& covered) {
    if (Declared != Covered) {
        return false;
    }
    for (std::size_t index = 0; index < Declared; ++index) {
        if (std::string_view(declared[index]) != std::string_view(covered[index])) {
            return false;
        }
    }
    return true;
}

static_assert(sameVerbs(quantum::ipc::verb::All, coveredVerbs),
              "the frozen verb set changed: update the mirror above, and every document that names it");

// The protocol revision, pinned. See the note at the top of this file.
constexpr int coveredProtocolVersion = 1;
static_assert(quantum::ipc::ProtocolVersion == coveredProtocolVersion,
              "the IPC protocol version changed: say what a frame of the old version now means instead");

}  // namespace

class IpcProtocolTest : public QObject {
    Q_OBJECT

private slots:
    void theFrozenVerbSetIsTheOneThisTestDeclares();
    void everyVerbRoundTripsThroughTheWire();
    void theHandshakeIsTheVersionRequestWrittenWithoutAVerb();
    void aRequestWithoutAVersionIsRefusedByName();
    void aLineThatIsNotAJsonObjectIsRefused();
    void aFieldOfTheWrongTypeIsRefusedByName();
    void anUnknownVerbDecodesSoTheServerCanRefuseIt();
    void aResponseRoundTripsThroughTheWire();
    void aRefusalNeverArrivesWithoutAMessage();
    void aResponseWithoutItsVersionOrItsAnswerIsRefused();
    void takeLinesSplitsCompleteLinesAndKeepsAnIncompleteOne();
    void takeLinesRefusesALineThatArrivesLongerThanTheLimit();
};

void IpcProtocolTest::theFrozenVerbSetIsTheOneThisTestDeclares() {
    // The compile-time assertion above is the guard; this slot is what makes a failure readable, because a
    // `static_assert` says that something changed and this says which name.
    QCOMPARE(quantum::ipc::verb::All.size(), coveredVerbs.size());
    for (std::size_t index = 0; index < coveredVerbs.size(); ++index) {
        QCOMPARE(QString::fromLatin1(quantum::ipc::verb::All[index]), QString::fromLatin1(coveredVerbs[index]));
    }
}

void IpcProtocolTest::everyVerbRoundTripsThroughTheWire() {
    using namespace quantum::ipc;

    for (const char* name : verb::All) {
        Request sent;
        sent.version = ProtocolVersion;
        sent.verb = QString::fromLatin1(name);
        // `config get` is the one verb that carries an argument; the others must not.
        if (sent.verb == QLatin1StringView(verb::ConfigGet))
            sent.path = QStringLiteral("bar.height");

        const QByteArray encoded = encodeRequest(sent);
        QVERIFY2(!encoded.contains('\n'), "a frame must be one line: the newline is the framing");

        Request received;
        QCOMPARE(decodeRequest(encoded, &received), QString());
        QCOMPARE(received.version, sent.version);
        QCOMPARE(received.verb, sent.verb);
        QCOMPARE(received.path, sent.path);
    }
}

void IpcProtocolTest::theHandshakeIsTheVersionRequestWrittenWithoutAVerb() {
    using namespace quantum::ipc;

    // Exactly the frame QUANTUM_SHELL.md § IPC writes: a version and nothing else.
    Request request;
    QCOMPARE(decodeRequest(QByteArrayLiteral("{\"version\":1}"), &request), QString());
    QCOMPARE(request.version, ProtocolVersion);
    // Decoded as the `version` verb rather than as a special case, so the handshake and the verb cannot
    // drift apart into two answers to the same question.
    QCOMPARE(request.verb, QString::fromLatin1(verb::Version));
}

void IpcProtocolTest::aRequestWithoutAVersionIsRefusedByName() {
    using namespace quantum::ipc;

    Request request;
    const QString error = decodeRequest(QByteArrayLiteral("{\"verb\":\"state\"}"), &request);
    QVERIFY2(!error.isEmpty(), "a request without a version must not be decoded");
    QVERIFY2(error.contains(QLatin1StringView("version")), qPrintable(error));
}

void IpcProtocolTest::aLineThatIsNotAJsonObjectIsRefused() {
    using namespace quantum::ipc;

    Request request;
    QVERIFY(!decodeRequest(QByteArrayLiteral("{\"version\":1"), &request).isEmpty());  // truncated JSON
    QVERIFY(!decodeRequest(QByteArrayLiteral("[1,2,3]"), &request).isEmpty());         // not an object
    QVERIFY(!decodeRequest(QByteArray(), &request).isEmpty());                         // an empty line
}

void IpcProtocolTest::aFieldOfTheWrongTypeIsRefusedByName() {
    using namespace quantum::ipc;

    Request request;
    const QString verbError = decodeRequest(QByteArrayLiteral("{\"version\":1,\"verb\":7}"), &request);
    QVERIFY2(verbError.contains(QLatin1StringView("verb")), qPrintable(verbError));

    const QString pathError =
        decodeRequest(QByteArrayLiteral("{\"version\":1,\"verb\":\"state\",\"path\":[]}"), &request);
    QVERIFY2(pathError.contains(QLatin1StringView("path")), qPrintable(pathError));
}

void IpcProtocolTest::anUnknownVerbDecodesSoTheServerCanRefuseIt() {
    using namespace quantum::ipc;

    // The design document names `volume up` as a verb a shell could have; this shell has no audio service,
    // so what matters is that it decodes far enough to be refused *by name* rather than as a bad frame. A
    // client that typed something wrong gets "that is not a verb I implement", not "your JSON is broken".
    Request request;
    QCOMPARE(decodeRequest(QByteArrayLiteral("{\"version\":1,\"verb\":\"volume up\"}"), &request), QString());
    QCOMPARE(request.verb, QStringLiteral("volume up"));
}

void IpcProtocolTest::aResponseRoundTripsThroughTheWire() {
    using namespace quantum::ipc;

    Response sent;
    sent.ok = true;
    sent.data.insert(QStringLiteral("height"), 32);

    Response received;
    QCOMPARE(decodeResponse(encodeResponse(sent), &received), QString());
    QCOMPARE(received.version, ProtocolVersion);
    QCOMPARE(received.ok, true);
    QCOMPARE(received.data.value(QStringLiteral("height")).toInt(), 32);
    QVERIFY(received.error.isEmpty());
}

void IpcProtocolTest::aRefusalNeverArrivesWithoutAMessage() {
    using namespace quantum::ipc;

    // Even a refusal whose message was never written carries one, because a refusal that says nothing is
    // indistinguishable from a bug at the other end of a socket.
    Response sent;
    sent.ok = false;

    Response received;
    QCOMPARE(decodeResponse(encodeResponse(sent), &received), QString());
    QCOMPARE(received.ok, false);
    QVERIFY2(!received.error.isEmpty(), "a refusal must always carry a message");
}

void IpcProtocolTest::aResponseWithoutItsVersionOrItsAnswerIsRefused() {
    using namespace quantum::ipc;

    Response response;
    QVERIFY(!decodeResponse(QByteArrayLiteral("{\"ok\":true}"), &response).isEmpty());
    QVERIFY(!decodeResponse(QByteArrayLiteral("{\"version\":1}"), &response).isEmpty());
    QVERIFY(!decodeResponse(QByteArrayLiteral("{\"version\":1,\"ok\":false}"), &response).isEmpty());
}

void IpcProtocolTest::takeLinesSplitsCompleteLinesAndKeepsAnIncompleteOne() {
    using namespace quantum::ipc;

    QByteArray buffer = QByteArrayLiteral("{\"a\":1}\r\n{\"b\":2}\n{\"c\":");
    QString error;
    const QList<QByteArray> lines = takeLines(buffer, &error);

    QCOMPARE(lines.size(), 2);
    // A carriage return before the newline is framing, not part of the frame: a client on any platform may
    // send it, and leaving it in would make the JSON fail to parse for a reason that looks like bad JSON.
    QCOMPARE(lines.at(0), QByteArrayLiteral("{\"a\":1}"));
    QCOMPARE(lines.at(1), QByteArrayLiteral("{\"b\":2}"));
    // The tail stays for the next read rather than being answered as a frame.
    QCOMPARE(buffer, QByteArrayLiteral("{\"c\":"));
    QVERIFY(error.isEmpty());
}

void IpcProtocolTest::takeLinesRefusesALineThatArrivesLongerThanTheLimit() {
    using namespace quantum::ipc;

    QByteArray buffer(MaxLineBytes + 1, 'x');
    QString error;
    QVERIFY(takeLines(buffer, &error).isEmpty());
    QVERIFY2(!error.isEmpty(), "a line longer than the limit must be refused rather than kept");
    // Cleared, not kept: the alternative is holding a client's bytes until it decides to stop sending them.
    QVERIFY(buffer.isEmpty());

    // The same in one write, with the newline: the limit applies to the line, not to how it arrived.
    QByteArray complete(MaxLineBytes + 1, 'y');
    complete.append('\n');
    QVERIFY(takeLines(complete, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(IpcProtocolTest)
#include "ipc_protocol_test.moc"
