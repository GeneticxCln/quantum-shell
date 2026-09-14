// The IPC server over a real socket.
//
// Qt's own `QLocalServer` and `QLocalSocket` on both ends, no niri and no display: what is tested here is
// the framing, the handshake and every refusal, which is all the server itself decides. The values it
// answers with come from a test double, deliberately — the server's job is to carry them, and
// `ipc-capabilities-test` is what checks that in the shell they are the service's real values.
//
// The socket name is this process's own. The abstract namespace is per user rather than per process, so a
// test that bound the shell's name would fail on the machine of anybody who has a shell running — which is
// exactly the person most likely to run this. That the *frozen* name is the one a real shell binds is
// `niri-live-layershell-test`'s business, where the bound name is read back out of the socket list.
#include "app/Logging.h"
#include "ipc/IPCProtocol.h"
#include "ipc/IPCServer.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTest>

#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// A test double for what the IPC can reach, named as one. It records what it was asked so the test can
// check that a verb reached the shell's side rather than being answered inside the server, and its answers
// are mutable so a slot can prove the server carries what it is given instead of a value of its own.
class RecordingCapabilities : public quantum::ipc::Capabilities {
public:
    QJsonObject state() const override { return reportedState; }

    std::optional<QJsonValue> configValue(const QString& path) const override {
        if (path == QLatin1StringView("bar.height")) {
            return QJsonValue(height);
        }
        return std::nullopt;
    }

    bool toggleBar() override {
        visible = !visible;
        ++toggles;
        return visible;
    }

    QJsonObject reportedState;
    int height = 32;
    int toggles = 0;
    bool visible = true;
};

// One connection: send a line, read a line. The real client is `qsctl`, which `niri-live-layershell-test`
// runs against a real shell; this is the same protocol driven from the test process so a refusal can be read
// without spawning anything.
//
// Everything here waits through the event loop rather than with `QLocalSocket`'s blocking `waitFor*` calls,
// and that is load-bearing rather than stylistic. The server is in this same process, and `waitForReadyRead`
// blocks in the socket engine rather than running the event loop, so the accept and the read that have to
// happen on the other end never get a turn — the client waits out its timeout for an answer the shell was
// never given the chance to write. The first version of this test did exactly that and failed every slot.
class ShellClient {
public:
    explicit ShellClient(const QString& socketName) {
        // The option is what adds the abstract namespace's leading NUL, and a client without it addresses a
        // different socket — the trap `IPCProtocol.h` documents, and one an ordinary test would otherwise
        // reproduce by accident.
        socket_.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
        socket_.connectToServer(socketName);
        connected_ = QTest::qWaitFor([this] { return socket_.state() == QLocalSocket::ConnectedState; },
                                     connectTimeoutMs);
    }

    bool connected() const { return connected_; }

    void send(const QByteArray& line) { sendRaw(line + '\n'); }

    // The framing is what this skips, which is the only way to send something that is not a frame: the slot
    // that checks what a line that never ends does has to write bytes with no newline in them at all.
    void sendRaw(const QByteArray& bytes) {
        socket_.write(bytes);
        // Flushed, not waited for: `waitForBytesWritten` is the same trap as above. The frame is small enough
        // that a flush hands it to the kernel, and the wait that matters is the one for the answer.
        socket_.flush();
    }

    QByteArray receive(int timeoutMs = answerTimeoutMs) {
        QByteArray line;
        const bool arrived = QTest::qWaitFor(
            [&] {
                buffer_.append(socket_.readAll());
                const qsizetype newline = buffer_.indexOf('\n');
                if (newline < 0)
                    return false;
                line = buffer_.left(newline);
                buffer_.remove(0, newline + 1);
                return true;
            },
            timeoutMs);
        // An empty line is how a slot learns the answer never came, and the slot says which request it was
        // waiting for: asserting here instead would report a timeout without the test's own description.
        return arrived ? line : QByteArray();
    }

    // One request, one decoded response, with the decode error as the failure message rather than as a
    // silent empty object: a response this test cannot read is a failure of the server, not of the test.
    std::optional<quantum::ipc::Response> request(const QByteArray& line) {
        send(line);
        const QByteArray answer = receive();
        if (answer.isEmpty()) {
            return std::nullopt;
        }
        quantum::ipc::Response response;
        const QString error = quantum::ipc::decodeResponse(answer, &response);
        if (!error.isEmpty()) {
            lastDecodeError_ = error;
            return std::nullopt;
        }
        return response;
    }

    QString lastDecodeError() const { return lastDecodeError_; }
    bool isConnected() const { return socket_.state() == QLocalSocket::ConnectedState; }

private:
    static constexpr int connectTimeoutMs = 2000;
    static constexpr int answerTimeoutMs = 5000;

    QLocalSocket socket_;
    QByteArray buffer_;
    bool connected_ = false;
    QString lastDecodeError_;
};

// One frame, as the bytes it is on the wire.
QByteArray frame(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray requestFor(const char* verb) {
    QJsonObject object;
    object.insert(QStringLiteral("version"), quantum::ipc::ProtocolVersion);
    object.insert(QStringLiteral("verb"), QString::fromLatin1(verb));
    if (QLatin1StringView(verb) == QLatin1StringView(quantum::ipc::verb::ConfigGet)) {
        object.insert(QStringLiteral("path"), QStringLiteral("bar.height"));
    }
    return frame(object);
}

// Distinctive rather than the shell's own values, so the handshake is proved to carry the application's
// identity rather than a string the server happens to contain.
constexpr auto coveredApplicationName = "quantum-shell-under-test";
constexpr auto coveredApplicationVersion = "9.9.9-test";

}  // namespace

class IpcServerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void theSocketIsReallyBoundAndOnlyOnce();
    void theHandshakeNamesTheApplicationAndTheProtocol();
    void everyDeclaredVerbIsAnswered();
    void anUnknownVerbIsRefusedAndTheRefusalNamesTheOnesThatExist();
    void theStateAnsweredIsTheStateItWasGiven();
    void aConfigurationKeyResolvesAndOneThisShellDoesNotReadIsRefused();
    void theBarToggleReportsTheBarAndChangesIt();
    void aMismatchedProtocolVersionIsRefusedAndTheConnectionClosed();
    void aMalformedFrameIsRefusedAndClosesTheConnection();
    void aLineThatNeverEndsIsRefusedRatherThanBuffered();
    void aConnectionAnswersMoreThanOneRequest();
    void aFrameWrittenBeforeTheAcceptIsStillRead();

private:
    QString socketName_;
    RecordingCapabilities capabilities_;
    std::unique_ptr<quantum::ipc::IPCServer> server_;
};

void IpcServerTest::initTestCase() {
    QCoreApplication::setApplicationName(QString::fromLatin1(coveredApplicationName));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(coveredApplicationVersion));

    // Per process, because the abstract namespace is per user: two test processes running at once — the
    // order checks run the same binary in several of them — must not take each other's name.
    socketName_ = QStringLiteral("quantum-shell-test-%1").arg(QCoreApplication::applicationPid());
    server_ = std::make_unique<quantum::ipc::IPCServer>(capabilities_, socketName_);

    QString error;
    QVERIFY2(server_->listen(&error), qPrintable(error));
    QVERIFY(server_->isListening());
}

void IpcServerTest::theSocketIsReallyBoundAndOnlyOnce() {
    QCOMPARE(server_->socketName(), socketName_);

    // A second server on the same name must fail. Without this, `listen()` returning true would be the only
    // evidence that anything was bound at all — and a listen that quietly did nothing would pass every other
    // slot here, because a client in the same process would still find a socket it could not have.
    RecordingCapabilities other;
    quantum::ipc::IPCServer second(other, socketName_);
    QString error;
    QVERIFY2(!second.listen(&error), "a second server took a name that was already bound");
    QVERIFY2(!error.isEmpty(), "a refused listen must say why");
}

void IpcServerTest::theHandshakeNamesTheApplicationAndTheProtocol() {
    ShellClient client(socketName_);
    QVERIFY2(client.connected(), "the test client could not reach the server");

    // The frame the design document writes, with no verb in it.
    const std::optional<quantum::ipc::Response> response = client.request(
        QByteArrayLiteral("{\"version\":1}"));
    QVERIFY2(response.has_value(), qPrintable(client.lastDecodeError()));
    QVERIFY(response->ok);
    QCOMPARE(response->data.value(QStringLiteral("name")).toString(), QString::fromLatin1(coveredApplicationName));
    QCOMPARE(response->data.value(QStringLiteral("shell")).toString(),
             QString::fromLatin1(coveredApplicationVersion));
    QCOMPARE(response->data.value(QStringLiteral("protocol")).toInt(), quantum::ipc::ProtocolVersion);
}

void IpcServerTest::everyDeclaredVerbIsAnswered() {
    // The guard behind "a verb exists only when it is implemented and tested": every name in the frozen list
    // is sent, in the shape that name takes, and must be answered rather than refused as unknown. A verb
    // declared but not handled fails here — which is the failure that matters, because a declared verb that
    // is refused looks exactly like a client's typo.
    ShellClient client(socketName_);
    QVERIFY(client.connected());

    for (const char* verb : quantum::ipc::verb::All) {
        const std::optional<quantum::ipc::Response> response = client.request(requestFor(verb));
        QVERIFY2(response.has_value(), qPrintable(client.lastDecodeError()));
        QVERIFY2(response->ok, qPrintable(QStringLiteral("%1 was refused: %2")
                                              .arg(QString::fromLatin1(verb), response->error)));
    }
}

void IpcServerTest::anUnknownVerbIsRefusedAndTheRefusalNamesTheOnesThatExist() {
    ShellClient client(socketName_);
    QVERIFY(client.connected());

    QJsonObject object;
    object.insert(QStringLiteral("version"), quantum::ipc::ProtocolVersion);
    object.insert(QStringLiteral("verb"), QStringLiteral("volume up"));
    const std::optional<quantum::ipc::Response> response = client.request(frame(object));

    QVERIFY2(response.has_value(), qPrintable(client.lastDecodeError()));
    QCOMPARE(response->ok, false);
    // Both halves matter: the refusal names what was asked for, and what could have been asked for instead.
    QVERIFY2(response->error.contains(QStringLiteral("volume up")), qPrintable(response->error));
    for (const char* verb : quantum::ipc::verb::All) {
        QVERIFY2(response->error.contains(QString::fromLatin1(verb)), qPrintable(response->error));
    }
    // Refused, and still connected: one bad request does not cost a client the connection.
    QVERIFY(client.isConnected());
}

void IpcServerTest::theStateAnsweredIsTheStateItWasGiven() {
    ShellClient client(socketName_);
    QVERIFY(client.connected());

    // Changed between the two calls, so a server answering with something it remembered from before fails.
    capabilities_.reportedState = QJsonObject{{QStringLiteral("connected"), false},
                                              {QStringLiteral("workspaces"), QJsonArray{}}};
    const std::optional<quantum::ipc::Response> first = client.request(requestFor(quantum::ipc::verb::State));
    QVERIFY2(first.has_value(), qPrintable(client.lastDecodeError()));
    QCOMPARE(first->data, capabilities_.reportedState);

    capabilities_.reportedState = QJsonObject{
        {QStringLiteral("connected"), true},
        {QStringLiteral("workspaces"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("6")},
                                {QStringLiteral("idx"), 1}}}}};
    const std::optional<quantum::ipc::Response> second = client.request(requestFor(quantum::ipc::verb::State));
    QVERIFY2(second.has_value(), qPrintable(client.lastDecodeError()));
    QCOMPARE(second->data, capabilities_.reportedState);
}

void IpcServerTest::aConfigurationKeyResolvesAndOneThisShellDoesNotReadIsRefused() {
    ShellClient client(socketName_);
    QVERIFY(client.connected());

    QJsonObject get;
    get.insert(QStringLiteral("version"), quantum::ipc::ProtocolVersion);
    get.insert(QStringLiteral("verb"), QString::fromLatin1(quantum::ipc::verb::ConfigGet));

    get.insert(QStringLiteral("path"), QStringLiteral("bar.height"));
    const std::optional<quantum::ipc::Response> answered = client.request(frame(get));
    QVERIFY2(answered.has_value(), qPrintable(client.lastDecodeError()));
    QVERIFY(answered->ok);
    QCOMPARE(answered->data.value(QStringLiteral("path")).toString(), QStringLiteral("bar.height"));
    QCOMPARE(answered->data.value(QStringLiteral("value")).toInt(), 32);

    // A key the schema does not read is refused by name, not answered with a null a script would have to
    // detect for itself.
    get.insert(QStringLiteral("path"), QStringLiteral("bar.widht"));
    const std::optional<quantum::ipc::Response> refused = client.request(frame(get));
    QVERIFY2(refused.has_value(), qPrintable(client.lastDecodeError()));
    QCOMPARE(refused->ok, false);
    QVERIFY2(refused->error.contains(QStringLiteral("bar.widht")), qPrintable(refused->error));

    // And a `config get` with no key at all says what is missing instead of answering with nothing.
    get.remove(QStringLiteral("path"));
    const std::optional<quantum::ipc::Response> incomplete = client.request(frame(get));
    QVERIFY2(incomplete.has_value(), qPrintable(client.lastDecodeError()));
    QCOMPARE(incomplete->ok, false);
    QVERIFY(!incomplete->error.isEmpty());
}

void IpcServerTest::theBarToggleReportsTheBarAndChangesIt() {
    ShellClient client(socketName_);
    QVERIFY(client.connected());

    // Asserted as a transition rather than against a value: the double is shared by every slot in this
    // binary, so which way the bar is pointing when this slot starts is another slot's business.
    const bool before = capabilities_.visible;
    const int togglesBefore = capabilities_.toggles;

    const std::optional<quantum::ipc::Response> first = client.request(requestFor(quantum::ipc::verb::BarToggle));
    QVERIFY2(first.has_value(), qPrintable(client.lastDecodeError()));
    QCOMPARE(first->ok, true);
    QCOMPARE(first->data.value(QStringLiteral("visible")).toBool(), !before);
    QCOMPARE(capabilities_.toggles, togglesBefore + 1);

    // And back, so the shell's own side is what is reported rather than a value the server flips itself.
    const std::optional<quantum::ipc::Response> second = client.request(requestFor(quantum::ipc::verb::BarToggle));
    QVERIFY2(second.has_value(), qPrintable(client.lastDecodeError()));
    QCOMPARE(second->data.value(QStringLiteral("visible")).toBool(), before);
    QCOMPARE(capabilities_.toggles, togglesBefore + 2);
}

void IpcServerTest::aMismatchedProtocolVersionIsRefusedAndTheConnectionClosed() {
    ShellClient client(socketName_);
    QVERIFY(client.connected());

    QJsonObject object;
    object.insert(QStringLiteral("version"), quantum::ipc::ProtocolVersion + 1);
    object.insert(QStringLiteral("verb"), QString::fromLatin1(quantum::ipc::verb::State));
    const std::optional<quantum::ipc::Response> response = client.request(frame(object));

    QVERIFY2(response.has_value(), qPrintable(client.lastDecodeError()));
    QCOMPARE(response->ok, false);
    // What the handshake exists for: the client is told both versions, so it can report them rather than
    // retry forever or act on an answer given to a request the shell may not have understood.
    QCOMPARE(response->version, quantum::ipc::ProtocolVersion);
    QVERIFY2(response->error.contains(QString::number(quantum::ipc::ProtocolVersion)),
             qPrintable(response->error));
    QVERIFY2(response->error.contains(QString::number(quantum::ipc::ProtocolVersion + 1)),
             qPrintable(response->error));

    // Closed rather than left waiting for a later frame, which could only be another frame it cannot read.
    QTRY_VERIFY(!client.isConnected());
}

void IpcServerTest::aMalformedFrameIsRefusedAndClosesTheConnection() {
    ShellClient client(socketName_);
    QVERIFY(client.connected());

    client.send(QByteArrayLiteral("this is not JSON"));
    const QByteArray answer = client.receive();
    QVERIFY2(!answer.isEmpty(), "a malformed frame was answered with silence");

    quantum::ipc::Response response;
    QCOMPARE(quantum::ipc::decodeResponse(answer, &response), QString());
    QCOMPARE(response.ok, false);
    QVERIFY(!response.error.isEmpty());
    QTRY_VERIFY(!client.isConnected());
}

void IpcServerTest::aLineThatNeverEndsIsRefusedRatherThanBuffered() {
    ShellClient client(socketName_);
    QVERIFY(client.connected());

    // No newline anywhere, which is why this writes the bytes itself rather than going through `send`: the
    // first version of this slot used the framing helper and therefore tested a complete line that was merely
    // long — which is refused as bad JSON once the length limit is gone, so the limit it meant to test could
    // have been deleted without this slot noticing. The check here is on how much has been sent without a frame
    // ending, which is what stops a client growing the shell's memory by never finishing a line.
    const QByteArray endless(quantum::ipc::MaxLineBytes + 4096, 'x');
    client.sendRaw(endless);
    QVERIFY(!endless.contains('\n'));

    const QByteArray answer = client.receive();
    QVERIFY2(!answer.isEmpty(), "an endless line was answered with silence");
    quantum::ipc::Response response;
    QCOMPARE(quantum::ipc::decodeResponse(answer, &response), QString());
    QCOMPARE(response.ok, false);
    QTRY_VERIFY(!client.isConnected());
}

void IpcServerTest::aConnectionAnswersMoreThanOneRequest() {
    ShellClient client(socketName_);
    QVERIFY(client.connected());

    // A client that asks two questions must not have to reconnect between them: the connection is a session,
    // not one exchange.
    const std::optional<quantum::ipc::Response> first = client.request(requestFor(quantum::ipc::verb::Version));
    const std::optional<quantum::ipc::Response> second = client.request(requestFor(quantum::ipc::verb::State));
    QVERIFY2(first.has_value(), qPrintable(client.lastDecodeError()));
    QVERIFY2(second.has_value(), qPrintable(client.lastDecodeError()));
    QVERIFY(first->ok);
    QVERIFY(second->ok);
    QVERIFY(client.isConnected());
}

// A frame that arrived with the connection is still read, and this is the slot that says so out loud.
//
// The client is a raw `AF_UNIX` socket rather than a `QLocalSocket`, and deliberately: it puts the connection
// *and* the frame on the wire with no event processing at all, so by the time the server's event loop next runs
// both are already there — the one ordering in which a server that reads only in response to a *new* arrival
// would hold a connection it never reads and answer nothing, with no error anywhere. Qt's own client cannot be
// posed this question, because posing it means writing before anything is processing events.
//
// It passes, so the server needs no drain of the buffer at accept time; an implementation with such a drain was
// written, could not be shown to matter by any test, and was removed. What this slot protects is the measured
// fact itself: if a Qt that stopped delivering that first frame were ever linked against, this is the slot that
// would fail rather than a shell that silently answers nothing.
void IpcServerTest::aFrameWrittenBeforeTheAcceptIsStillRead() {
    const QString name = socketName_;
    QByteArray address(name.size() + 1, '\0');
    std::memcpy(address.data() + 1, name.toUtf8().constData(), static_cast<size_t>(name.size()));

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(fd >= 0);

    struct ::sockaddr_un remote = {};
    remote.sun_family = AF_UNIX;
    QVERIFY(address.size() <= int(sizeof(remote.sun_path)));
    std::memcpy(remote.sun_path, address.constData(), static_cast<size_t>(address.size()));
    // The length is the address plus the bytes that are actually the name, not the whole `sun_path`: an
    // abstract address carries no terminating NUL in the kernel's copy, and `sizeof` would overstate it and
    // connect to a name whose tail is uninitialised padding.
    const auto length = static_cast<socklen_t>(offsetof(struct ::sockaddr_un, sun_path) + address.size());
    QCOMPARE(::connect(fd, reinterpret_cast<struct ::sockaddr*>(&remote), length), 0);

    const QByteArray frame = requestFor(quantum::ipc::verb::State) + '\n';
    QCOMPARE(::write(fd, frame.constData(), static_cast<size_t>(frame.size())), qint64(frame.size()));

    // Only now does anything process events, so the accept and the frame arrive together.
    QByteArray answer;
    const bool arrived = QTest::qWaitFor(
        [&] {
            // `MSG_DONTWAIT`, and this is load-bearing: the predicate runs before the event loop turns, so a
            // blocking read here would wait forever for an answer the server has not had its turn to write —
            // the same starvation that made an earlier version of this test file fail every slot.
            char buffer[4096];
            const qint64 read = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (read > 0)
                answer.append(buffer, static_cast<int>(read));
            return answer.contains('\n');
        },
        5000);
    ::close(fd);

    QVERIFY2(arrived, "the shell never answered a frame that arrived with the connection");
    quantum::ipc::Response response;
    QCOMPARE(quantum::ipc::decodeResponse(answer.left(answer.indexOf('\n')), &response), QString());
    QVERIFY2(response.ok, qPrintable(response.error));
    QCOMPARE(response.data, capabilities_.reportedState);
}

QTEST_GUILESS_MAIN(IpcServerTest)
#include "ipc_server_test.moc"
