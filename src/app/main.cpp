// The composition root: the one place the shell's parts are joined to each other.
//
// Everything assembled here is tested on its own elsewhere — the request connection, the typed event
// stream, the state model it feeds, the output refresher that niri's missing output event forces, the
// supervisor that recovers from a compositor restart, and the QObject QML binds to. This file adds no
// behaviour of its own beyond the order those objects are created in and the fact that they are wired
// at all, which is why it holds no logic to test: it is the wiring, stated in one place.
//
// What it puts on screen is `qml/Main.qml`, a layer-shell bar whose workspace strip and clock read
// `NiriService` — so the bar is a view of the live compositor, not a picture of one — and whose height
// and namespace come from the configuration file rather than from the QML itself.
#include "app/Logging.h"
#include "app/ShellCapabilities.h"
#include "config/Config.h"
#include "config/ConfigWatcher.h"
#include "ipc/IPCProtocol.h"
#include "ipc/IPCServer.h"
#include "niri/NiriActions.h"
#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"
#include "niri/NiriOutputs.h"
#include "niri/NiriReconnect.h"
#include "niri/NiriService.h"
#include "niri/NiriState.h"
#include "wayland/LayerShellWindow.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QUrl>
#include <QWindow>

namespace {

// The abstract socket as a person reads it: the public name, written the way `ss -x` prints it and the way
// the design document writes it. Built from the constant rather than typed out, so the name in a record is
// the name the server bound.
QString socketAddress()
{
    return QStringLiteral("\\0%1").arg(QString::fromLatin1(quantum::ipc::SocketName));
}

}  // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("quantum-shell"));
    // The build's own version, which is what `qsctl version` reports and what a bug report should quote. It
    // comes from CMake's project version rather than from a second copy in this file.
    QGuiApplication::setApplicationVersion(QStringLiteral(QS_VERSION));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Quantum Shell"));
    // Sets the Wayland app_id, which is how the compositor and any window rules name this client.
    QGuiApplication::setDesktopFileName(QStringLiteral("quantum-shell"));

    // Before anything that can log, so no record is emitted with Qt's bare format. Qt sends records to the
    // terminal when there is one and to the systemd journal when there is not — a shell started by niri has
    // no terminal, so its records are read with `journalctl --user` (QUANTUM_SHELL.md § Logging), and the
    // first one says which version is running.
    quantum::app::Logging::install();
    qCInfo(quantum::app::shellLog) << "quantum-shell" << QCoreApplication::applicationVersion()
                                   << "starting; ipc protocol" << quantum::ipc::ProtocolVersion << "config"
                                   << quantum::config::ConfigWatcher::defaultPath();

    // The window the bar is built on. Registered before the engine loads anything, because a QML file
    // naming a type has to resolve it at load time.
    qmlRegisterType<QuantumShell::LayerShellWindow>("QuantumShell", 1, 0, "LayerShellWindow");

    // The configuration, read before anything is built from it. `start()` is the one synchronous read:
    // it happens here, while there is no engine and no surface, so the bar is created at the height and
    // with the namespace the file asks for rather than corrected once it is already on screen. Every
    // later read — an edit while the shell is drawing — goes to the watcher's worker thread.
    quantum::config::Config config;
    quantum::config::Config::registerQmlSingleton(config);
    quantum::config::ConfigWatcher configWatcher(config);
    configWatcher.start();

    quantum::niri::NiriIPC requests;
    quantum::niri::NiriEventStream stream;
    quantum::niri::NiriState state;

    // Outputs are the one part of the model niri sends no event for, so they come from a request
    // connection of their own rather than from the stream.
    quantum::niri::NiriOutputs outputs(requests);

    // Both of these wire themselves to the stream, whose object survives a compositor restart, so each
    // is called exactly once. `observe` is what turns the stream's typed events into model state, and
    // it includes forgetting that state when the compositor goes away.
    state.observe(stream);
    state.observeOutputs(outputs);

    quantum::niri::NiriService service(state, stream);
    quantum::niri::NiriService::registerQmlSingleton(service);

    // The actions the bar performs, on the request connection: a capsule click focuses its workspace and
    // the wheel moves to the one below or above. Registered beside the state service so the two names the
    // bar binds to — `NiriService` and `NiriActions` — come from the same module and the same place.
    quantum::niri::NiriActions actions(requests);
    quantum::niri::NiriActions::registerQmlSingleton(actions);

    quantum::niri::NiriReconnect reconnect;
    reconnect.keepAttached(requests);
    reconnect.keepAttached(stream);

    // NiriOutputs::observe also performs the first Outputs request, which only makes sense once there is
    // a connection to ask on — so it is wired on the first attach rather than before `start()`. Later
    // attaches need no second wiring: the same stream emits the same burst, and the refresher already
    // listens to it.
    bool outputsWired = false;
    QObject::connect(&reconnect, &quantum::niri::NiriReconnect::attached, &outputs,
                     [&outputs, &stream, &outputsWired] {
                         if (outputsWired)
                             return;
                         outputsWired = true;
                         outputs.observe(stream);
                     });

    QQmlApplicationEngine engine;
    // A QML file that fails to load must fail the process rather than leave a half-built shell running
    // with no bar on screen.
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return EXIT_FAILURE;

    // The local IPC server, and what it is allowed to reach: the same three objects the QML is built from.
    // The bar is the engine's root object, held weakly, because hiding a surface is the one thing a verb can
    // do rather than read.
    auto* barWindow = qobject_cast<QWindow*>(engine.rootObjects().constFirst());
    quantum::app::ShellCapabilities capabilities(service, config, barWindow);
    quantum::ipc::IPCServer ipc(capabilities, QString::fromLatin1(quantum::ipc::SocketName));
    QString ipcError;
    if (ipc.listen(&ipcError)) {
        qCInfo(quantum::app::ipcLog) << "listening on the abstract socket" << socketAddress();
    } else {
        // Not fatal, and deliberately so: the abstract name is already taken when a second shell starts,
        // and a bar that draws is more useful than a shell that exits because someone started it twice.
        qCWarning(quantum::app::ipcLog)
            << "cannot listen on the abstract socket" << socketAddress() << ":" << ipcError
            << "- qsctl will be answered by whichever process holds the name";
    }

    reconnect.start();
    return app.exec();
}
