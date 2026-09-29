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
// and namespace come from the configuration file rather than from the QML itself. There is one bar per
// output, and they are created by `BarHost` rather than by a single QML root object, because a layer
// surface is created against an output (`BarHost.h`). The one thing here that is not the compositor's is
// the system status, which reads /proc through `SysMonService`; its sampling is started and stopped by the
// bars' own visibility, and that is the only wiring in this file that exists because a reading has no event
// source (see `SysMonService.h` for why it has none).
#include "app/BarHost.h"
#include "app/Logging.h"
#include "app/ShellCapabilities.h"
#include "app/HistoryHost.h"
#include "app/ControlCenterHost.h"
#include "app/CrashHandler.h"
#include "app/ControlCenterService.h"
#include "app/LauncherHost.h"
#include "app/OsdHost.h"
#include "app/ToastHost.h"
#include "apps/LauncherService.h"
#include "audio/PipeWireService.h"
#include "config/Config.h"
#include "config/ConfigWatcher.h"
#include "dbus/BatteryService.h"
#include "dbus/MediaService.h"
#include "dbus/NetworkService.h"
#include "dbus/NotificationService.h"
#include "ipc/IPCProtocol.h"
#include "ipc/IPCServer.h"
#include "niri/NiriActions.h"
#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"
#include "niri/NiriOutputs.h"
#include "niri/NiriReconnect.h"
#include "niri/NiriService.h"
#include "niri/NiriState.h"
#include "system/SysMonService.h"
#include "wayland/LayerShellWindow.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
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
    // The account of a fatal signal, installed as early as anything can log: a shell started by niri has no
    // terminal and nobody watching, and without this a crash leaves the journal a process that exited with a
    // signal and nothing else. It re-raises, so core dumps and the supervisor's restart still work.
    quantum::app::CrashHandler::install(QS_VERSION);
    qCInfo(quantum::app::shellLog) << "quantum-shell" << QCoreApplication::applicationVersion()
                                   << "starting; ipc protocol" << quantum::ipc::ProtocolVersion << "config"
                                   << quantum::config::ConfigWatcher::defaultPath();
    // The graphics API is chosen here rather than left to Qt's platform default, because the memory
    // budget is a written row (QUANTUM_SHELL.md § Quality gates) and the measurement says the choice
    // matters: with the hardware path the process opens the NVIDIA GL stack — 56 MB of resident
    // mappings, 35 MB of it the driver's shader compiler — and idled at 168.6 MB RSS against the
    // <150 MB budget, while the same executable with every readout live idled at 87.4 MB under the
    // software renderer. The bar is 2D, and the software renderer draws that scene without the
    // driver; the attribution behind the decision is recorded in QUANTUM_SHELL.md § Phase 1.
    //
    // A person who names QSG_RHI_BACKEND explicitly gets what they asked for instead — that is the
    // opt-in the hardware path and the Phase 2 3D plans remain reachable through — so this call only
    // fills the default and never overrides a stated choice. It must run before the first
    // QQuickWindow exists, and the bar window below is the first.
    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND")) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }


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

    // The system's own readings, registered before the engine loads so the widget naming `SysMonService`
    // resolves. It samples nothing yet: nothing is on screen, so there is nothing to keep current, and
    // `setActive` is not called until there is a bar to hide — which is also what keeps a hidden bar free.
    quantum::system::SysMonService sysMon;
    quantum::system::SysMonService::registerQmlSingleton(sysMon);

    // The cadence is the person's rather than a constant here: `bar.system.sample_interval_ms` reaches the
    // service through this one line, before the engine loads, so the first reading is taken at the interval
    // the file asks for rather than corrected after the bar is on screen. The connection is what makes an
    // edit to that key apply to a running shell; the signal fires only when the value actually changed, so a
    // re-read of an unchanged file re-arms nothing. The schema has already refused anything below the floor
    // the service also refuses, so a value arriving here is one both agree on.
    sysMon.setSampleIntervalMs(config.bar()->system()->sampleIntervalMs());
    QObject::connect(config.bar()->system(), &quantum::config::ConfigSystem::sampleIntervalMsChanged,
                     &sysMon, [&sysMon, &config] {
                         sysMon.setSampleIntervalMs(config.bar()->system()->sampleIntervalMs());
                     });

    // The bar's volume readout, registered before the engine loads so a widget naming `PipeWireService`
    // resolves. Three settings reach it here, and all three are the file's rather than constants: the two
    // steps — `bar.audio.step_percent` for the percentage readout and `bar.audio.step_decibels` for the
    // decibel one — and the unit that says which of them a notch applies, which is `bar.audio.volume_scale`,
    // the same key the widget draws in. One question with one answer, which is why a person reading decibels
    // is not moved by a number that depends on where they already are.
    //
    // Every connection is what makes a later edit to that key apply to a running shell, and each is unwired
    // from the service, so it is broken the moment this object dies — which is the process ending, since
    // nothing destroys it. The unit goes through the module's own token door, so an unknown spelling is
    // refused where the enum lives rather than here.
    quantum::audio::PipeWireService audio;
    quantum::audio::PipeWireService::registerQmlSingleton(audio);
    audio.setStepPercent(config.bar()->audio()->stepPercent());
    audio.setStepDecibels(config.bar()->audio()->stepDecibels());
    audio.setWheelStepUnit(config.bar()->audio()->volumeScale());
    QObject::connect(config.bar()->audio(), &quantum::config::ConfigAudio::stepPercentChanged, &audio,
                     [&audio, &config] { audio.setStepPercent(config.bar()->audio()->stepPercent()); });
    QObject::connect(config.bar()->audio(), &quantum::config::ConfigAudio::stepDecibelsChanged, &audio,
                     [&audio, &config] { audio.setStepDecibels(config.bar()->audio()->stepDecibels()); });
    QObject::connect(config.bar()->audio(), &quantum::config::ConfigAudio::volumeScaleChanged, &audio,
                     [&audio, &config] { audio.setWheelStepUnit(config.bar()->audio()->volumeScale()); });

    // The launcher's model, registered before the engine loads so the surface naming `LauncherService` resolves.
    // It reads the machine's own environment for what to scan and which desktop it is on: the XDG data
    // directories, `XDG_CURRENT_DESKTOP` (colon-separated, as the Desktop Entry Specification defines it) and
    // the locale Qt resolved. The first scan is asked for here, off the GUI thread, so the list is ready by the
    // time the launcher is first opened. `[launcher] max_results` is the file's, followed live.
    quantum::apps::LauncherService launcher(quantum::apps::LauncherService::defaultDataDirs(),
                                            qEnvironmentVariable("XDG_CURRENT_DESKTOP")
                                                .split(QLatin1Char(':'), Qt::SkipEmptyParts),
                                            QLocale::system().name());
    quantum::apps::LauncherService::registerQmlSingleton(launcher);
    launcher.setMaxResults(config.launcher()->maxResults());
    QObject::connect(config.launcher(), &quantum::config::ConfigLauncher::maxResultsChanged, &launcher,
                     [&launcher, &config] { launcher.setMaxResults(config.launcher()->maxResults()); });
    launcher.refresh();

    // The control centre's open state, registered before the engine loads so the panel naming
    // `ControlCenterService` resolves. What the panel shows and does belongs to the services it is drawn from.
    quantum::app::ControlCenterService controlCenter;
    quantum::app::ControlCenterService::registerQmlSingleton(controlCenter);

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

    // The network readout's connection to NetworkManager, registered before the engine loads so the widget
    // naming `NetworkService` resolves. It is not started here — see below, where the reason is the same one the
    // audio daemon is started late — and it takes no configuration: what the service follows is the connection
    // the machine is on, and the three `[bar.network]` keys decide only what the widget draws from it.
    quantum::dbus::NetworkService network;
    quantum::dbus::NetworkService::registerQmlSingleton(network);

    // The battery readout's connection to UPower, registered for the same reason and with the same argument: the
    // service follows the machine's power state and the three `[bar.battery]` keys decide only what the widget
    // draws from it. A machine with no battery is not a special case here — the daemon answers that it has none,
    // and the widget draws nothing at all.
    quantum::dbus::BatteryService battery;
    quantum::dbus::BatteryService::registerQmlSingleton(battery);

    // The media player readout's connection to MPRIS players, registered for the same reason: the service
    // follows the active player and the one `[bar.media]` key decides what the widget draws from it. A
    // session with no players is not a special case — the widget draws nothing until a player appears.
    quantum::dbus::MediaService media;
    quantum::dbus::MediaService::registerQmlSingleton(media);

    // The notification daemon. This service is the inverse of the three above it: the shell is the
    // daemon rather than a reader of one, so it takes `org.freedesktop.Notifications` on the session
    // bus rather than subscribing to another process. It takes no configuration — what it publishes is
    // what a sender sent, and the readout decides only how to draw it, which is not this file's
    // business. Registered before the engine loads so a widget naming `NotificationService` resolves,
    // and started here rather than late like the daemons above, because it owns a name on the bus: a
    // desktop with no other notifier has nothing sending notifications until this one is listening, so
    // the name is taken before the bar exists rather than after the first frame.
    quantum::dbus::NotificationService notifications;
    quantum::dbus::NotificationService::registerQmlSingleton(notifications);
    notifications.start();

    QQmlApplicationEngine engine;

    // The bars, one per output. A bar component that fails to load must fail the process rather than leave a
    // half-built shell running with no bar on screen, which is what `ready()` answers.
    quantum::app::BarHost bars(engine, QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (!bars.ready())
        return EXIT_FAILURE;

    // No output at all is a shell drawing nothing, and saying so is the honest outcome: niri has at least one
    // output while it runs, so an empty set is worth a line rather than silence (the bars themselves are
    // logged as each one is created).
    if (bars.bars().isEmpty())
        qCWarning(quantum::app::waylandLog) << "no output to draw a bar on";

    // The sampling follows the bars: it runs while there is something a person can see, and any bar on screen
    // is that. Each bar presents its surface while it is being created, so the bars are already up by the time
    // this line runs — hence the reading of `anyVisible()` as well as the connection: one covers the bars that
    // are up before this line, the other every hide and show after it, including `qsctl bar toggle`, which
    // reaches them through the host below.
    sysMon.setActive(bars.anyVisible());
    QObject::connect(&bars, &quantum::app::BarHost::anyVisibleChanged, &sysMon,
                     [&sysMon, &bars] { sysMon.setActive(bars.anyVisible()); });

    // The toasts, one per output, created when a notification arrives and withdrawn when their expiry runs
    // out. A toast component that fails to load fails the process for the reason the bar's does: a daemon
    // answering notifications that nothing draws is not a notification daemon anyone asked for.
    quantum::app::ToastHost toasts(notifications, config, engine, QUrl(QStringLiteral("qrc:/qml/Toast.qml")));
    if (!toasts.ready())
        return EXIT_FAILURE;

    // The history panel, on the primary output while the service says it is open. It fails the process for the
    // same reason a toast does: a readout whose right click opens a panel that cannot be drawn is a control that
    // lies.
    quantum::app::HistoryHost history(notifications, engine,
                                      QUrl(QStringLiteral("qrc:/qml/NotificationHistory.qml")), QUrl(QStringLiteral("qrc:/qml/Backdrop.qml")));
    if (!history.ready())
        return EXIT_FAILURE;

    // The volume's on-screen display, shown for an adjustment of the sink the audio service follows. It fails the
    // process for the same reason the toast does.
    quantum::app::OsdHost osd(audio, config, engine, QUrl(QStringLiteral("qrc:/qml/VolumeOsd.qml")));
    if (!osd.ready())
        return EXIT_FAILURE;

    // The launcher's surface, on the primary output while the service says it is open. It fails the process for the
    // same reason the toast does: a verb that opens a surface that cannot be drawn is a control that lies.
    quantum::app::LauncherHost launcherHost(launcher, engine, QUrl(QStringLiteral("qrc:/qml/Launcher.qml")), QUrl(QStringLiteral("qrc:/qml/Backdrop.qml")));
    if (!launcherHost.ready())
        return EXIT_FAILURE;

    // The control centre's surface, on the primary output while its state says it is open; fails the process for
    // the same reason the launcher's does.
    quantum::app::ControlCenterHost controlCenterHost(controlCenter, engine,
                                                      QUrl(QStringLiteral("qrc:/qml/ControlCenter.qml")), QUrl(QStringLiteral("qrc:/qml/Backdrop.qml")));
    if (!controlCenterHost.ready())
        return EXIT_FAILURE;

    quantum::app::ShellCapabilities capabilities(service, config, &bars, &launcher, &controlCenter);
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

    // Attaching to the audio daemon is deliberately left until after the bar is on screen and the IPC is
    // listening: it is the shell's only connection to something that may not be running, and a compositor
    // restart or a daemon restart both recover on their own. Nothing on the bar waits for it — the volume
    // readout draws its empty state until a reading arrives, which is the same thing it draws while
    // `available` is false for any other reason.
    audio.start();

    // The same for NetworkManager, and with the same argument: the daemon it follows is on the system bus and
    // may well not be running when the shell starts, and both a daemon that arrives later and one that is
    // restarted under the shell recover without anything here. `start` is handed the system bus because that is
    // where NetworkManager lives — unlike PipeWire, which is on the session's own socket — and it returns
    // immediately whatever the bus says, so the bar is never waiting for it.
    network.start(QDBusConnection::systemBus());

    // And the same for UPower, which is on the same system bus and has the same failure modes: it may not be
    // running when the shell starts, it may be restarted under it, and neither is anything the bar waits for.
    battery.start(QDBusConnection::systemBus());

    // And the same for MPRIS players on the session bus: players come and go, the service follows the active
    // one, and the widget draws nothing until a player appears and has metadata to show.
    media.start(QDBusConnection::sessionBus());

    reconnect.start();
    return app.exec();
}
