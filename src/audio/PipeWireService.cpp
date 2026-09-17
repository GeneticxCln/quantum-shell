#include "audio/PipeWireService.h"

#include "QmlModule.h"
#include "app/Logging.h"
#include "audio/AudioVolume.h"

#include <pipewire/context.h>
#include <pipewire/core.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/keys.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>
#include <pipewire/properties.h>
#include <pipewire/proxy.h>
#include <pipewire/thread-loop.h>
#include <pipewire/version.h>

#include <spa/param/param.h>
#include <spa/pod/pod.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <spa/utils/type.h>

#include <QByteArray>
#include <QHash>
#include <QMetaObject>
#include <QTimer>
#include <QQmlEngine>
#include <QtGlobal>

#include <cerrno>
#include <cstring>
#include <optional>

namespace quantum::audio {

namespace {

// The metadata object, and the one key in it this service reads. Both read back from a running session
// rather than recalled: `pw-metadata -n default` on this machine prints
// `update: id:0 key:'default.audio.sink' value:'{"name": "alsa_output..."}' type:'Spa:String:JSON'`,
// and `/usr/share/pipewire/pipewire.conf`'s own commented metadata factory creates the object under this
// name. WirePlumber is what sets the key on a real desktop, which is why QUANTUM_SHELL.md lists it as a
// dependency of the audio module rather than as an option.
constexpr auto DefaultMetadataName = "default";
constexpr auto DefaultSinkKey = "default.audio.sink";

// The class a sink advertises, and the property the metadata names it by. Both are WirePlumber's own
// convention: on this session the default sink's `node.name` is exactly the string in
// `default.audio.sink`'s JSON, and its node carries `media.class = "Audio/Sink"`.
constexpr auto SinkMediaClass = "Audio/Sink";

// The backoff between attach attempts. It starts short, because the case it is most often for is a daemon
// that is about to exist — the shell starting before WirePlumber, or a session restarted under it — and
// doubling from a quarter second reaches the cap in five attempts, which is roughly eight seconds of
// trying before the shell settles into a slow retry. A backoff timer is one of the three uses
// SYSTEM_PROMPT.md § Event-driven allows a timer for, which is why this file may have one and
// `src/system/SysMonService.cpp` needed a waiver for its own.
constexpr int FirstRetryMs = 250;
constexpr int MaxRetryMs = 8000;

void initialiseLibrary()
{
    // `pw_init` loads the library's configuration and its logging once per process; calling it twice is
    // harmless but pointless, and a function-local static is the way to say "once" without a global to
    // keep in step. Nothing is passed in: the shell does not override PipeWire's own configuration,
    // because the daemon it talks to is the desktop's and its settings are the desktop's.
    static const bool initialised = [] {
        pw_init(nullptr, nullptr);
        return true;
    }();
    Q_UNUSED(initialised);
}

QString fromPipeWire(const char* text)
{
    return QString::fromUtf8(text == nullptr ? "" : text);
}

}  // namespace

// Everything PipeWire-shaped, and the only place a `pw_*` name appears. Defined here rather than in the
// header so that registering a QML type does not drag `libpipewire` into the configuration schema, the
// IPC library or the bar's own test.
//
// **Threading, from the header's own words.** `thread-loop.h` says "The lock is recursive" and "All events
// and callbacks are called with the thread lock held", so every callback below runs on the loop thread
// with the lock already taken, and every entry point from the GUI thread takes it before touching a proxy.
// Nothing is shared without it, and the published properties are only ever written by a queued invocation
// on the GUI thread, which is why there is no lock over `PipeWireService`'s own fields.
struct PipeWireService::Impl {
    explicit Impl(PipeWireService* owner) : service(owner) {}

    // --- lifecycle -------------------------------------------------------------------------------

    void attach();
    void detach();
    void retryLater();

    // --- what the daemon says --------------------------------------------------------------------

    void onGlobal(uint32_t id, const char* type, const spa_dict* props);
    void onGlobalRemoved(uint32_t id);
    void onMetadataProperty(const char* key, const char* value);
    void onNodeInfo(const pw_node_info* info);
    void onNodeParam(uint32_t id, const spa_pod* param);
    void onCoreError(int result, const char* message);

    // --- what the shell asks ---------------------------------------------------------------------

    void stepVolume(int direction);
    void setVolumePercent(int percent);
    void toggleMute();

    // --- the two helpers every one of the above goes through --------------------------------------

    // Binds `name`'s node and starts following its Props. The lock must already be held: every caller is
    // either a callback (which has it) or has taken it.
    void bindSink(uint32_t id, const QString& name);
    // Drops the binding to the current sink, if there is one. Lock held, as above.
    void releaseSink();
    // Publishes the reading that is currently held, through a queued invocation so that it lands on the
    // GUI thread and QML sees a whole reading rather than half of one.
    void publish();
    // True the moment there is a reading to show: a sink is bound and its Props have arrived.
    bool hasReading() const { return sink != nullptr && haveReading; }

    PipeWireService* service = nullptr;

    // The remote to attach to. Empty means PipeWire's own default (`pipewire-0`), which is what a real
    // session has; a test names its own so that it talks to a daemon it started and never to the
    // desktop's. This is the `remote.name` property the library's `keys.h` documents and `pw-cli -r`
    // sets, not a second mechanism invented here.
    QString remoteName;

    // Whether a connection is wanted. Set by `start`, cleared by `stop`, and checked by every retry: the
    // backoff between attempts must not outlive the intent, or a service that was stopped while a retry was
    // pending would attach again behind the caller's back.
    bool wanted = false;

    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    spa_hook coreHook{};
    pw_registry* registry = nullptr;
    spa_hook registryHook{};
    pw_metadata* metadata = nullptr;
    spa_hook metadataHook{};
    pw_node* sink = nullptr;
    spa_hook sinkHook{};

    // The global id the bound sink came from, so a removal can be told from a different sink's.
    uint32_t sinkGlobalId = SPA_ID_INVALID;
    // Every sink the registry has announced, by `node.name`. Kept rather than bound eagerly: only the one
    // the metadata names is followed, and the metadata can arrive before or after the node.
    QHash<QString, uint32_t> sinkIds;
    // What the metadata last said the default sink is, and which sink is actually bound.
    QString wantedSinkName;
    QString sinkName;

    SinkState state;
    bool haveReading = false;

    int retryMs = FirstRetryMs;

    // The event tables, one per interface this service listens to, each built once and returned by
    // reference. They are functions rather than file-scope constants because every callback in them has to
    // name `Impl`, and `Impl` is private to `PipeWireService` — a constant at namespace scope has no access
    // to it. The callbacks themselves are captureless lambdas, which convert to plain function pointers,
    // written inside a member function of `Impl` and so carrying its access. Only the events this service
    // uses are set; the rest stay null, which is how the library is told it may skip them.
    static const pw_core_events& coreEvents();
    static const pw_registry_events& registryEvents();
    static const pw_metadata_events& metadataEvents();
    static const pw_node_events& nodeEvents();
};

// --- lifecycle -----------------------------------------------------------------------------------

void PipeWireService::Impl::attach()
{
    initialiseLibrary();

    // The design's floor for this module is PipeWire 1.6, and the library is a link-time dependency, so a
    // build against an older one is a link error rather than something to detect. It is still checked,
    // because the library a process ends up with at run time is the dynamic linker's decision and the
    // difference between 1.6 and 1.4 here is whether the daemon emits the events this file relies on.
    if (!pw_check_library_version(1, 6, 0)) {
        qCWarning(quantum::app::audioLog)
            << "PipeWire" << pw_get_library_version() << "is older than the 1.6 this shell is built "
            << "against; the volume readout will stay empty";
        return;
    }

    loop = pw_thread_loop_new("quantum-shell-audio", nullptr);
    if (loop == nullptr) {
        qCWarning(quantum::app::audioLog) << "cannot create a PipeWire thread loop; the volume readout "
                                             "will stay empty and another attempt follows";
        retryLater();
        return;
    }

    // The remote is named here rather than in the environment because the environment belongs to whoever
    // started the shell. A shell with no name configured attaches to whatever `pipewire-0` is, which on a
    // session is the desktop's daemon.
    pw_properties* properties = nullptr;
    const QByteArray remote = remoteName.toUtf8();
    if (!remote.isEmpty())
        properties = pw_properties_new(PW_KEY_REMOTE_NAME, remote.constData(), nullptr);

    // Ownership of `properties` passes to the context even when this fails, which the header says in as
    // many words, so it must not be freed here.
    context = pw_context_new(pw_thread_loop_get_loop(loop), properties, 0);
    if (context == nullptr) {
        qCWarning(quantum::app::audioLog) << "cannot create a PipeWire context";
        pw_thread_loop_destroy(loop);
        loop = nullptr;
        retryLater();
        return;
    }

    pw_thread_loop_start(loop);

    pw_thread_loop_lock(loop);
    core = pw_context_connect(context, nullptr, 0);
    if (core == nullptr) {
        pw_thread_loop_unlock(loop);
        qCInfo(quantum::app::audioLog)
            << "no PipeWire daemon to attach to" << (remoteName.isEmpty() ? QStringLiteral("(the default)")
                                                                          : remoteName);
        detach();
        retryLater();
        return;
    }

    pw_core_add_listener(core, &coreHook, &coreEvents(), this);
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(registry, &registryHook, &registryEvents(), this);
    pw_thread_loop_unlock(loop);

    qCInfo(quantum::app::audioLog)
        << "attached to PipeWire" << pw_get_library_version()
        << "on" << (remoteName.isEmpty() ? QStringLiteral("the default remote") : remoteName);
    retryMs = FirstRetryMs;
}

void PipeWireService::Impl::detach()
{
    if (loop == nullptr)
        return;

    // The proxies first, under the lock, because the callbacks that use them run with it held and would
    // otherwise be handed a freed pointer by a removal racing this.
    pw_thread_loop_lock(loop);
    releaseSink();
    if (metadata != nullptr) {
        spa_hook_remove(&metadataHook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata));
        metadata = nullptr;
    }
    if (registry != nullptr) {
        spa_hook_remove(&registryHook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
        registry = nullptr;
    }
    if (core != nullptr) {
        spa_hook_remove(&coreHook);
        pw_core_disconnect(core);
        core = nullptr;
    }
    sinkIds.clear();
    wantedSinkName.clear();
    sinkName.clear();
    haveReading = false;
    state = SinkState{};
    pw_thread_loop_unlock(loop);

    // `pw_thread_loop_stop` must be called *without* the lock — the header says so, and it is why the
    // unlock above is explicit rather than left to the scope. It waits for the loop thread to finish, so
    // no callback is running when the context goes.
    pw_thread_loop_stop(loop);
    pw_context_destroy(context);
    context = nullptr;
    pw_thread_loop_destroy(loop);
    loop = nullptr;
}

void PipeWireService::Impl::retryLater()
{
    if (!wanted)
        return;
    const int delay = retryMs;
    retryMs = qMin(retryMs * 2, MaxRetryMs);
    qCInfo(quantum::app::audioLog) << "trying again in" << delay << "ms";
    // A single-shot backoff, re-armed per attempt. Not a loop that polls state: nothing is asked of the
    // daemon between attempts, and an attempt is the thing that establishes the connection.
    QTimer::singleShot(delay, service, [this] {
        if (wanted && loop == nullptr)
            attach();
    });
}

// --- the registry --------------------------------------------------------------------------------

void PipeWireService::Impl::onGlobal(uint32_t id, const char* type, const spa_dict* props)
{
    if (type == nullptr)
        return;

    if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        // Only the `default` metadata: it is the one WirePlumber publishes the default sink in, and a
        // second metadata object's `default.audio.sink` would be a different desktop's answer.
        const char* name = props == nullptr ? nullptr : spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (metadata != nullptr || name == nullptr || std::strcmp(name, DefaultMetadataName) != 0)
            return;
        metadata = static_cast<pw_metadata*>(pw_registry_bind(
            registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
        if (metadata == nullptr) {
            qCWarning(quantum::app::audioLog) << "cannot bind the" << DefaultMetadataName
                                              << "metadata object, so no sink can be resolved";
            return;
        }
        pw_metadata_add_listener(metadata, &metadataHook, &metadataEvents(), this);
        // The bound object replays its properties on connect, which is how the current default arrives;
        // if it does not, the first change to it does.
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    const char* mediaClass = props == nullptr ? nullptr : spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (mediaClass == nullptr || std::strcmp(mediaClass, SinkMediaClass) != 0)
        return;

    const char* nodeName = props == nullptr ? nullptr : spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (nodeName == nullptr)
        return;

    // Every sink is remembered, and the one the metadata names is followed. Which of the two arrives
    // first is the daemon's decision: a sink can be created after the default was set (a USB device
    // plugged in) or before it (the daemon start-up), and both orders have to end in the same place.
    const QString name = fromPipeWire(nodeName);
    sinkIds.insert(name, id);
    if (name == wantedSinkName && sink == nullptr)
        bindSink(id, name);
}

void PipeWireService::Impl::onGlobalRemoved(uint32_t id)
{
    for (auto it = sinkIds.begin(); it != sinkIds.end();) {
        if (it.value() == id)
            it = sinkIds.erase(it);
        else
            ++it;
    }
    if (id == sinkGlobalId) {
        qCInfo(quantum::app::audioLog) << "the sink this shell was following is gone:" << sinkName;
        releaseSink();
        publish();
    }
}

void PipeWireService::Impl::onMetadataProperty(const char* key, const char* value)
{
    if (key == nullptr || std::strcmp(key, DefaultSinkKey) != 0)
        return;

    // A removed key or a value that is not the object form is a refusal, not a fallback: the shell has
    // not been told which sink is the default, so it shows nothing rather than a different device's
    // volume under the same name.
    const std::optional<QString> name =
        value == nullptr ? std::nullopt : sinkNameFromMetadata(fromPipeWire(value));
    if (!name.has_value()) {
        qCInfo(quantum::app::audioLog) << DefaultSinkKey << "was cleared or is not a sink name; the "
                                                             "volume readout has nothing to show";
        wantedSinkName.clear();
        releaseSink();
        publish();
        return;
    }

    if (*name == wantedSinkName && sink != nullptr)
        return;
    wantedSinkName = *name;
    qCInfo(quantum::app::audioLog) << "the default sink is" << *name;

    const auto id = sinkIds.constFind(*name);
    if (id == sinkIds.constEnd()) {
        // The default was named for a sink the registry has not announced yet. The binding happens in
        // `onGlobal` when it arrives; nothing waits for it.
        releaseSink();
    } else {
        bindSink(*id, *name);
    }
    publish();
}

void PipeWireService::Impl::bindSink(uint32_t id, const QString& name)
{
    releaseSink();

    sink = static_cast<pw_node*>(pw_registry_bind(registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (sink == nullptr) {
        qCWarning(quantum::app::audioLog) << "cannot bind the sink" << name;
        return;
    }
    sinkGlobalId = id;
    sinkName = name;
    pw_node_add_listener(sink, &sinkHook, &nodeEvents(), this);

    // The subscription is the event source, in the header's own words: "Automatically emit param events
    // for the given ids when they are changed." The enum that follows is only for the value that already
    // exists — a subscription reports changes, and the reading this shell starts from is not one.
    uint32_t props = SPA_PARAM_Props;
    pw_node_subscribe_params(sink, &props, 1);
    pw_node_enum_params(sink, 0, SPA_PARAM_Props, 0, 1, nullptr);
}

void PipeWireService::Impl::releaseSink()
{
    if (sink != nullptr) {
        spa_hook_remove(&sinkHook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(sink));
        sink = nullptr;
    }
    sinkGlobalId = SPA_ID_INVALID;
    sinkName.clear();
    haveReading = false;
    state = SinkState{};
}

void PipeWireService::Impl::onNodeInfo(const pw_node_info* info)
{
    if (info == nullptr || sink == nullptr)
        return;
    // Which params a node holds and when they change is what `pw_node_info.change_mask` is for: the
    // library sets `PW_NODE_CHANGE_MASK_PARAMS` and names the ids in `info->params` when any of them
    // moved. Reading that is how a client that is not subscribed to a param still learns it changed, and
    // it is what makes this module event-driven rather than a reader on a clock.
    if ((info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) == 0)
        return;
    for (uint32_t index = 0; index < info->n_params; ++index) {
        if (info->params[index].id == SPA_PARAM_Props) {
            pw_node_enum_params(sink, 0, SPA_PARAM_Props, 0, 1, nullptr);
            return;
        }
    }
}

void PipeWireService::Impl::onNodeParam(uint32_t id, const spa_pod* param)
{
    if (sink == nullptr || id != SPA_PARAM_Props)
        return;

    const std::optional<SinkState> reading = parseProps(param);
    if (!reading.has_value()) {
        // The daemon answered the Props request with something that is not a volume. Refusing it leaves
        // whatever was last published standing, which is the daemon's own answer rather than a guess, and
        // says so in the log — a volume that stops moving is otherwise indistinguishable from a volume
        // that did not change.
        qCWarning(quantum::app::audioLog) << "the Props param for" << sinkName
                                          << "is not a volume reading; the last one stands";
        return;
    }

    const bool first = !haveReading;
    state = *reading;
    haveReading = true;
    if (first)
        qCInfo(quantum::app::audioLog) << "reading" << sinkName << "from the daemon";
    publish();
}

void PipeWireService::Impl::onCoreError(int result, const char* message)
{
    // A fatal core error is the daemon going away — WirePlumber restarting it, or the session ending.
    // The connection is not reusable after one, so the whole thing is torn down and rebuilt, and the
    // reading is withdrawn in between: a bar showing the volume from before a daemon restart would be
    // showing a number that is no longer anyone's.
    qCWarning(quantum::app::audioLog) << "PipeWire reported" << result << fromPipeWire(message)
                                      << "- reattaching";
    // The teardown runs on the GUI thread, because `pw_thread_loop_stop` waits for this very callback to
    // return and cannot be called from it.
    QMetaObject::invokeMethod(
        service,
        [this] {
            if (loop == nullptr)
                return;
            detach();
            publish();
            retryLater();
        },
        Qt::QueuedConnection);
}

// --- what the shell asks -------------------------------------------------------------------------

void PipeWireService::Impl::publish()
{
    const bool available = hasReading();
    const bool muted = state.muted;
    // Both units from the one factor, here, where the factor is: the two numbers a readout can be drawn in
    // are computed together and cross the thread boundary together, so the bar can never show a percentage
    // and a decibel value that came from two different readings.
    const int percent = available ? percentFromLinear(state.linearVolume) : 0;
    const double decibels = available ? decibelsFromLinear(state.linearVolume) : 0.0;
    const QString name = sinkName;
    QMetaObject::invokeMethod(
        service,
        [this, available, muted, percent, decibels, name] {
            service->applyReading(available, muted, percent, decibels, name);
        },
        Qt::QueuedConnection);
}

void PipeWireService::Impl::stepVolume(int direction)
{
    if (direction != 1 && direction != -1)
        return;
    if (loop == nullptr || sink == nullptr || !haveReading) {
        qCInfo(quantum::app::audioLog) << "no reading to step from; nothing was written";
        return;
    }

    // The current value is read and turned into a new one in one turn of the lock, so two wheel notches
    // in the same event-loop turn cannot both step from the same starting point and lose a step.
    pw_thread_loop_lock(loop);
    const SinkState current = state;
    pw_thread_loop_unlock(loop);

    // One notch, in the unit the readout is drawn in: the step's *distance* is the configuration's and which of
    // the two distances applies is too, so the arithmetic itself is the pure half's — what is left here is
    // which factor to write. It starts from the daemon's own factor rather than from the rounded percentage,
    // which is what makes a decibel notch a fixed gain: a step derived from a rounded percentage would be a
    // step in a number that had already lost the precision the gain needs.
    const double next = linearAfterWheelStep(current.linearVolume, direction, service->wheelStepUnit(),
                                             service->stepPercent(), service->stepDecibels(),
                                             PipeWireService::MaxPercent);
    const PropsWrite write(current.channels, next, current.muted);
    if (write.pod() == nullptr)
        return;

    pw_thread_loop_lock(loop);
    const int result = sink == nullptr ? -ENOTSUP : pw_node_set_param(sink, SPA_PARAM_Props, 0, write.pod());
    pw_thread_loop_unlock(loop);
    if (result < 0)
        // Both renderings of one factor, because which one the person was reading is what the gesture was:
        // a step that looks wrong in the unit they are not using is not a second write.
        qCWarning(quantum::app::audioLog) << "the daemon refused a wheel step to" << percentFromLinear(next)
                                          << "% (" << decibelsFromLinear(next) << "dB):" << result;
}

void PipeWireService::Impl::setVolumePercent(int percent)
{
    if (loop == nullptr || sink == nullptr || !haveReading) {
        qCInfo(quantum::app::audioLog) << "no reading to set from; nothing was written";
        return;
    }
    const int target = percent < 0 ? 0 : qMin(percent, PipeWireService::MaxPercent);
    pw_thread_loop_lock(loop);
    const SinkState current = state;
    pw_thread_loop_unlock(loop);

    const PropsWrite write(current.channels, linearFromPercent(target), current.muted);
    if (write.pod() == nullptr)
        return;

    pw_thread_loop_lock(loop);
    const int result = sink == nullptr ? -ENOTSUP : pw_node_set_param(sink, SPA_PARAM_Props, 0, write.pod());
    pw_thread_loop_unlock(loop);
    if (result < 0)
        qCWarning(quantum::app::audioLog) << "the daemon refused a volume of" << target << "%:" << result;
}

void PipeWireService::Impl::toggleMute()
{
    if (loop == nullptr || sink == nullptr || !haveReading) {
        qCInfo(quantum::app::audioLog) << "no reading to mute; nothing was written";
        return;
    }
    // The volume is carried through unchanged: a mute that reset the volume would lose a setting the
    // person chose, and `wpctl set-mute` keeps it too.
    pw_thread_loop_lock(loop);
    const SinkState current = state;
    pw_thread_loop_unlock(loop);

    const PropsWrite write(current.channels, current.linearVolume, !current.muted);
    if (write.pod() == nullptr)
        return;

    pw_thread_loop_lock(loop);
    const int result = sink == nullptr ? -ENOTSUP : pw_node_set_param(sink, SPA_PARAM_Props, 0, write.pod());
    pw_thread_loop_unlock(loop);
    if (result < 0)
        qCWarning(quantum::app::audioLog) << "the daemon refused the mute change:" << result;
}

// --- the callbacks, which exist only to put a method call on this object's own thread --------------------
//
// Each forwards to the instance the library was handed as its `data` pointer, and nothing decides anything:
// a callback holding logic of its own would be logic that runs on PipeWire's thread and nowhere near a test.
// The parameters a callback does not use are left unnamed rather than marked, which is both quieter and a
// truer statement — there is nothing to mark because there is nothing there.
//
// Each table is built once. The version field is set first and by hand: it is the struct's ABI revision
// rather than one of its callbacks, and the library refuses a table whose version it does not know.

const pw_core_events& PipeWireService::Impl::coreEvents()
{
    static const pw_core_events table = [] {
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        // The fatal error is the one event this service acts on, and it is the daemon going away.
        events.error = [](void* data, uint32_t, int, int result, const char* message) {
            static_cast<Impl*>(data)->onCoreError(result, message);
        };
        return events;
    }();
    return table;
}

const pw_registry_events& PipeWireService::Impl::registryEvents()
{
    static const pw_registry_events table = [] {
        pw_registry_events events{};
        events.version = PW_VERSION_REGISTRY_EVENTS;
        events.global = [](void* data, uint32_t id, uint32_t, const char* type, uint32_t,
                           const spa_dict* props) { static_cast<Impl*>(data)->onGlobal(id, type, props); };
        events.global_remove = [](void* data, uint32_t id) {
            static_cast<Impl*>(data)->onGlobalRemoved(id);
        };
        return events;
    }();
    return table;
}

const pw_metadata_events& PipeWireService::Impl::metadataEvents()
{
    static const pw_metadata_events table = [] {
        pw_metadata_events events{};
        events.version = PW_VERSION_METADATA_EVENTS;
        events.property = [](void* data, uint32_t, const char* key, const char*,
                             const char* value) -> int {
            static_cast<Impl*>(data)->onMetadataProperty(key, value);
            return 0;
        };
        return events;
    }();
    return table;
}

const pw_node_events& PipeWireService::Impl::nodeEvents()
{
    static const pw_node_events table = [] {
        pw_node_events events{};
        events.version = PW_VERSION_NODE_EVENTS;
        events.info = [](void* data, const pw_node_info* info) {
            static_cast<Impl*>(data)->onNodeInfo(info);
        };
        events.param = [](void* data, int, uint32_t id, uint32_t, uint32_t, const spa_pod* param) {
            static_cast<Impl*>(data)->onNodeParam(id, param);
        };
        return events;
    }();
    return table;
}

// --- the service ----------------------------------------------------------------------------------

PipeWireService::PipeWireService(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>(this))
{
}

PipeWireService::~PipeWireService()
{
    // `stop()` and not just `detach()`: the destructor must also cancel the intent, so that a backoff timer
    // armed a moment ago finds nothing to attach to. The timer is a child of this object and would be
    // destroyed with it, but the lambda checks the intent anyway and is the cheaper thing to be right about.
    stop();
}

void PipeWireService::setStepPercent(int percent)
{
    if (percent < MinimumStepPercent) {
        qCWarning(quantum::app::audioLog) << "refusing a volume step of" << percent
                                          << "percentage points; the step stays at" << stepPercent_;
        return;
    }
    if (percent == stepPercent_)
        return;
    stepPercent_ = percent;
    // Said here rather than where the value came from, because this is the object the step belongs to and the
    // only one that sees every way it can change — `bar.audio.step_percent` at startup, and a later edit to
    // that key through the same setter. It is also the only way a running shell's step is observable from
    // outside the process, which is what the live case in `niri-live-layershell-test` reads back; the
    // cadence in `SysMonService` is recorded for the same reason.
    qCInfo(quantum::app::audioLog) << "a wheel notch moves the volume by" << stepPercent_
                                   << "percentage points, which is the step the percentage readout uses";
    emit stepPercentChanged();
}

void PipeWireService::setStepDecibels(double decibels)
{
    // `!(x >= floor)` rather than `x < floor`, so a NaN is refused as well as a zero: a step that is not a
    // number is not a small step, and the shape is the one `decibelsFromLinear` uses for its own refusals.
    if (!(decibels >= MinimumStepDecibels)) {
        qCWarning(quantum::app::audioLog)
            << "refusing a volume step of" << decibels << "dB: the bar draws decibels to one decimal place, so "
            << "a step shorter than" << MinimumStepDecibels
            << "dB is a notch that changes nothing a person can see; the step stays at" << stepDecibels_ << "dB";
        return;
    }
    if (decibels == stepDecibels_)
        return;
    stepDecibels_ = decibels;
    qCInfo(quantum::app::audioLog) << "a wheel notch moves the volume by" << stepDecibels_
                                   << "dB, which is the step the decibel readout uses";
    emit stepDecibelsChanged();
}

void PipeWireService::setWheelStepUnit(WheelStepUnit unit)
{
    if (unit == wheelStepUnit_)
        return;
    wheelStepUnit_ = unit;
    // The token goes into the prose with `arg` rather than through `QDebug` directly, because streaming a
    // `QString` appends it in quotes: the record would read `both in "decibel"`, which is a machine's
    // punctuation in a sentence a person reads — and the live test in `niri-live-layershell-test` matches the
    // text of a running shell's record, so the quoting would be a spelling of the message rather than a fact
    // about the shell.
    qCInfo(quantum::app::audioLog)
        << QStringLiteral("the volume readout and the wheel's step are both in %1")
               .arg(QString::fromLatin1(wheelStepUnitToken(wheelStepUnit_).data()));
    emit wheelStepUnitChanged();
}

void PipeWireService::setWheelStepUnit(QStringView token)
{
    const std::optional<WheelStepUnit> unit = wheelStepUnitFromToken(token);
    if (!unit.has_value()) {
        // The second opinion on a spelling, and the same division of labour as the step's floor: the schema
        // refuses an unknown token in the file, and this refuses one that somehow got past it. What arrives
        // here on a real shell is the value the schema accepted, so the interesting case is a caller that is
        // not the configuration.
        qCWarning(quantum::app::audioLog)
            << QStringLiteral("refusing the volume unit \"%1\": this shell draws a volume in percent or "
                              "decibels; the readout and the step stay in %2")
                   .arg(token.toString(), QString::fromLatin1(wheelStepUnitToken(wheelStepUnit_).data()));
        return;
    }
    setWheelStepUnit(*unit);
}

void PipeWireService::start(const QString& remoteName)
{
    impl_->remoteName = remoteName;
    impl_->wanted = true;
    impl_->attach();
}

void PipeWireService::stop()
{
    impl_->wanted = false;
    impl_->detach();
    // The reading goes with the connection, and the notification goes with the reading: a bar left holding
    // the last sink's number after the connection ended would be showing a value with nothing behind it.
    applyReading(false, false, 0, 0.0, QString());
}

void PipeWireService::toggleMute()
{
    impl_->toggleMute();
}

void PipeWireService::stepVolume(int direction)
{
    impl_->stepVolume(direction);
}

void PipeWireService::setVolumePercent(int percent)
{
    impl_->setVolumePercent(percent);
}

void PipeWireService::applyReading(bool available, bool muted, int percent, double decibels,
                                   const QString& sinkName)
{
    // Signalled only when one of the published values moved. A mute is not a volume change and a volume
    // change is not a mute, but both arrive in one reading, so the comparison is over all of them — a notify
    // that fired on every reading would wake every binding on the bar for a daemon that repeats itself, and
    // `audio-test` pins that it does not.
    //
    // The percentage and the decibels are both compared, and they are not the same trigger: the percentage is
    // a rounded integer and the decibels are not, so a volume change too small to move the percentage by a
    // point still moves the decibel value and is still a change a bar drawn in dB has to redraw. Comparing
    // only the integer would leave such a bar showing the volume before the change.
    if (available == available_ && muted == muted_ && percent == volumePercent_
        && decibels == volumeDecibels_) {
        return;
    }
    const bool becameAvailable = available && !available_;
    available_ = available;
    muted_ = muted;
    volumePercent_ = percent;
    volumeDecibels_ = decibels;
    if (becameAvailable)
        qCInfo(quantum::app::audioLog)
            << "the volume readout is live:" << sinkName << percent << "%," << decibels << "dB";
    emit readingChanged();
}

void PipeWireService::registerQmlSingleton(PipeWireService& service)
{
    qmlRegisterSingletonInstance(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion,
                                 quantum::qml::ModuleMinorVersion, QmlTypeName, &service);
}

}  // namespace quantum::audio
