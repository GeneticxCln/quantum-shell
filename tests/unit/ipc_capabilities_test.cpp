// What the IPC can reach, answered from the shell's real objects.
//
// `ipc-server-test` proves the framing with a test double, because the server's job is to carry an answer
// and not to know what one means. This is the other half, and it is the half that would otherwise be
// untested glue: a real `NiriService` fed by the fake compositor, a real `Config`, and the adapter between
// them. Two claims are checked here that a live test cannot check as sharply — that the keys the IPC reports
// are the service's own property names rather than a second spelling, and that the values are the service's
// values verbatim rather than a reshape that happens to look right.
//
// The compositor's end of the socket is `FakeNiriServer`, the same test double `niri-service-test` uses, so
// this needs neither a compositor nor a display.
//
// The bar window is deliberately null here. A `QWindow` needs a `QGuiApplication` and therefore a platform
// plugin, which a unit test must not require — and hiding a real surface is checked where it can be checked
// for real: `niri-live-layershell-test` toggles the bar of a running shell and watches it leave and return to
// the compositor's layer list.
#include "app/ShellCapabilities.h"
#include "config/Config.h"
#include "config/ConfigSchema.h"
#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"
#include "niri/NiriOutputs.h"
#include "niri/NiriService.h"
#include "niri/NiriState.h"

#include "FakeNiriServer.h"
#include "NiriProtocolTestData.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QSignalSpy>
#include <QTest>

#include <memory>
#include <optional>
#include <string_view>

using quantum::app::ShellCapabilities;
using quantum::config::Config;
using quantum::config::ConfigValues;
using quantum::niri::NiriEventStream;
using quantum::niri::NiriIPC;
using quantum::niri::NiriOutputs;
using quantum::niri::NiriService;
using quantum::niri::NiriState;

namespace {

// The key paths the shell offers to `qsctl config get`, written out independently of `ConfigSchema.h` and
// compared with it at compile time. The duplication is the point: these paths are what a script types, so a
// rename in the schema must not build until it is acknowledged here and in the document that lists them.
constexpr std::array<const char*, 18> coveredKeyPaths{"bar.height",
                                                       "bar.layerNamespace",
                                                       "bar.system.sample_interval_ms",
                                                       "bar.system.show_cpu",
                                                       "bar.system.show_memory",
                                                       "bar.system.memory_format",
                                                       "bar.audio.show_volume",
                                                       "bar.audio.volume_scale",
                                                       "bar.audio.step_percent",
                                                       "bar.audio.step_decibels",
                                                       "bar.network.show_status",
                                                       "bar.network.show_name",
                                                       "bar.network.show_strength",
                                                       "bar.battery.show_status",
                                                       "bar.battery.show_percentage",
                                                       "bar.battery.show_time",
                                                       "bar.media.show_media",
                                                       "bar.notifications.show_notifications"};

template <std::size_t Declared, std::size_t Covered>
constexpr bool samePaths(const std::array<const char*, Declared>& declared,
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

static_assert(samePaths(quantum::config::KeyPaths, coveredKeyPaths),
              "the configuration key paths changed: update the mirror above, and `qsctl`'s documentation");

}  // namespace

class IpcCapabilitiesTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void everyStateKeyIsAPropertyOfTheService();
    void theStateIsTheServicesOwnValues();
    void everyConfigurationKeyPathResolvesToTheLiveValue();
    void aKeyThisShellDoesNotReadResolvesToNothing();
    void withNoBarThereIsNothingToToggle();

private:
    void pushEvent(const QString& name, const QJsonObject& fields = {});

    FakeNiriServer server_;
    NiriIPC request_;
    NiriEventStream stream_;
    NiriState state_;
    std::unique_ptr<NiriOutputs> outputs_;
    NiriService service_{state_, stream_};
    Config config_;
    std::unique_ptr<ShellCapabilities> capabilities_;
};

void IpcCapabilitiesTest::initTestCase() {
    QVERIFY2(server_.listen(), qPrintable(server_.serverError()));
    server_.setReply(QStringLiteral("EventStream"), QByteArrayLiteral("{\"Ok\":\"Handled\"}"));
    server_.setReply(QStringLiteral("Outputs"), qstest::outputsReplyLine({}));

    state_.observe(stream_);
    outputs_ = std::make_unique<NiriOutputs>(request_);
    state_.observeOutputs(*outputs_);
    outputs_->observe(stream_);

    QSignalSpy streaming(&stream_, &NiriEventStream::streaming);
    request_.connectToCompositor(server_.path());
    stream_.connectToCompositor(server_.path());
    QTRY_VERIFY_WITH_TIMEOUT(streaming.count() == 1, 5000);

    capabilities_ = std::make_unique<ShellCapabilities>(service_, config_, nullptr);
}

void IpcCapabilitiesTest::pushEvent(const QString& name, const QJsonObject& fields) {
    // Connection one is the event stream: the request connection is opened first, so the stream is the second
    // connection the fake compositor accepted, counted from zero.
    server_.writeRawTo(1, qstest::eventLine(name, fields));
}

void IpcCapabilitiesTest::everyStateKeyIsAPropertyOfTheService() {
    // The keys are compared against the *meta-object*, not against a list in this file: `NiriService`'s
    // properties are the QML API, and the IPC exposes the same capabilities under the same names, so the
    // property declarations are the one place the names exist. A property added there and not reported here,
    // or a key reported here that is not a property, fails in both directions.
    const QJsonObject state = capabilities_->state();

    QSet<QString> reported;
    for (const QString& key : state.keys())
        reported.insert(key);

    QSet<QString> properties;
    const QMetaObject* metaObject = service_.metaObject();
    for (int index = QObject::staticMetaObject.propertyCount(); index < metaObject->propertyCount(); ++index)
        properties.insert(QString::fromLatin1(metaObject->property(index).name()));

    QCOMPARE(properties.size(), 6);  // there are six; a count of zero would make this a test of nothing
    QCOMPARE(reported, properties);
}

void IpcCapabilitiesTest::theStateIsTheServicesOwnValues() {
    // The values are compared against the service rather than against literals, which is what catches an
    // adapter that reshapes: it is the same claim as "the IPC and the QML API expose the same capabilities",
    // and comparing it to anything other than the service itself would let a second mapping hide here.
    //
    // Non-empty state first: with nothing reported, every comparison below would hold for an adapter that
    // returned empty containers.
    pushEvent(QStringLiteral("WorkspacesChanged"),
              QJsonObject{{QStringLiteral("workspaces"),
                           qstest::array({qstest::workspaceObject(7, 2, QStringLiteral("DP-3"), true, true),
                                          qstest::workspaceObject(6, 1, QStringLiteral("DP-3"))})}});
    QTRY_VERIFY_WITH_TIMEOUT(service_.workspaces().size() == 2, 5000);

    pushEvent(QStringLiteral("WindowsChanged"),
              QJsonObject{{QStringLiteral("windows"),
                           qstest::array({qstest::windowObject(13, QStringLiteral("google-chrome"), 7, true)})}});
    QTRY_VERIFY_WITH_TIMEOUT(!service_.focusedWindow().isEmpty(), 5000);

    pushEvent(QStringLiteral("OverviewOpenedOrClosed"),
              QJsonObject{{QStringLiteral("is_open"), true}});
    QTRY_VERIFY_WITH_TIMEOUT(service_.overviewOpen(), 5000);

    const QJsonObject state = capabilities_->state();
    QCOMPARE(state.value(QStringLiteral("workspaces")).toArray(),
             QJsonValue::fromVariant(service_.workspaces()).toArray());
    QCOMPARE(state.value(QStringLiteral("focusedWindow")).toObject(),
             QJsonValue::fromVariant(service_.focusedWindow()).toObject());
    QCOMPARE(state.value(QStringLiteral("outputs")).toArray(),
             QJsonValue::fromVariant(service_.outputs()).toArray());
    QCOMPARE(state.value(QStringLiteral("keyboardLayout")).toObject(),
             QJsonValue::fromVariant(service_.keyboardLayout()).toObject());
    QCOMPARE(state.value(QStringLiteral("overviewOpen")).toBool(), service_.overviewOpen());
    QCOMPARE(state.value(QStringLiteral("connected")).toBool(), service_.connected());

    // And with the state established, the two halves are not both empty by accident.
    QCOMPARE(state.value(QStringLiteral("workspaces")).toArray().size(), 2);
    QCOMPARE(state.value(QStringLiteral("overviewOpen")).toBool(), true);
    QCOMPARE(state.value(QStringLiteral("connected")).toBool(), true);
}

void IpcCapabilitiesTest::everyConfigurationKeyPathResolvesToTheLiveValue() {
    // The declared paths first: a path offered to a script that resolves to nothing is a name that lies, so
    // the list is walked rather than the two paths being checked one at a time.
    for (const char* path : quantum::config::KeyPaths) {
        const QString key = QString::fromLatin1(path);
        QVERIFY2(capabilities_->configValue(key).has_value(), qPrintable(key));
    }

    // Then the value is the shell's current one, and stays current: applied through the same `Config` the bar
    // binds to, so an adapter that read the defaults once and kept them fails here.
    QCOMPARE(capabilities_->configValue(QStringLiteral("bar.height"))->toInt(), config_.bar()->height());
    QCOMPARE(capabilities_->configValue(QStringLiteral("bar.layerNamespace"))->toString(),
             config_.bar()->layerNamespace());
    QCOMPARE(capabilities_->configValue(QStringLiteral("bar.system.sample_interval_ms"))->toInt(),
             config_.bar()->system()->sampleIntervalMs());
    QCOMPARE(capabilities_->configValue(QStringLiteral("bar.system.show_cpu"))->toBool(),
             config_.bar()->system()->showCpu());
    QCOMPARE(capabilities_->configValue(QStringLiteral("bar.system.memory_format"))->toString(),
             config_.bar()->system()->memoryFormat());

    ConfigValues changed;
    changed.bar.height = 44;
    changed.bar.layerNamespace = QStringLiteral("quantum-shell-bar-two");
    changed.bar.system.sampleIntervalMs = 750;
    changed.bar.system.showCpu = false;
    changed.bar.system.memoryFormat = QStringLiteral("percent");
    config_.apply(changed);

    QCOMPARE(capabilities_->configValue(QStringLiteral("bar.height"))->toInt(), 44);
    QCOMPARE(capabilities_->configValue(QStringLiteral("bar.height"))->toInt(), config_.bar()->height());
    QCOMPARE(capabilities_->configValue(QStringLiteral("bar.layerNamespace"))->toString(),
             QStringLiteral("quantum-shell-bar-two"));
    // A nested key follows the same path through the tree, and it is a JSON number rather than a string: a
    // script that does arithmetic on `qsctl config get bar.system.sample_interval_ms` gets a number.
    QCOMPARE(capabilities_->configValue(QStringLiteral("bar.system.sample_interval_ms"))->toInt(), 750);
    QCOMPARE(capabilities_->configValue(QStringLiteral("bar.system.sample_interval_ms"))->toInt(),
             config_.bar()->system()->sampleIntervalMs());
    // A flag arrives as a JSON boolean and the form as the token: a script can branch on the first without
    // comparing a string against `true`, which is what this layer's type is for. Read here on the JSON value
    // rather than on a `QVariant`, because that is what this interface carries — the schema's own types are
    // `config-test`'s, where the resolver is asked directly.
    const QJsonValue showCpu = *capabilities_->configValue(QStringLiteral("bar.system.show_cpu"));
    QVERIFY2(showCpu.isBool(), "the IPC reported a flag as something other than a JSON boolean");
    QCOMPARE(showCpu.toBool(), false);
    const QJsonValue format = *capabilities_->configValue(QStringLiteral("bar.system.memory_format"));
    QVERIFY(format.isString());
    QCOMPARE(format.toString(), QStringLiteral("percent"));

    // Put back, so a later slot in the same process is not reading this slot's edit — and because the default
    // namespace is what the rest of the suite refers to.
    config_.apply(ConfigValues{});
}

void IpcCapabilitiesTest::aKeyThisShellDoesNotReadResolvesToNothing() {
    // Every shape of a path that is not a key the schema reads. Each must resolve to nothing rather than to
    // an empty string or a default, because the server turns "nothing" into a refusal that names the path and
    // a null value into an answer a script would have to detect for itself.
    for (const char* path : {"bar", "bar.widht", "bar.height.px", "bar.system", "bar.sample_interval_ms",
                            "bar.system.sample_interval", "bar.system.show", "bar.system.memory_formats",
                            "schema_version", "theme", ""}) {
        QVERIFY2(!capabilities_->configValue(QString::fromLatin1(path)).has_value(), path);
    }
}

void IpcCapabilitiesTest::withNoBarThereIsNothingToToggle() {
    // A shell whose QML failed to load still has state to report, and the honest answer to `bar toggle` is
    // that there is no bar rather than a visibility for a window that does not exist.
    ShellCapabilities withoutBar(service_, config_, nullptr);
    QCOMPARE(withoutBar.toggleBar(), false);
}

QTEST_GUILESS_MAIN(IpcCapabilitiesTest)
#include "ipc_capabilities_test.moc"
