// The audio readout's connection to PipeWire: the default sink's volume and mute state, exposed to QML,
// kept current by events rather than by asking.
//
// This module is the counter-example to `src/system/`, which is the shell's only reading with a clock of
// its own. Audio has a real event source and this file uses it: `pw_node_subscribe_params`, which the
// installed header documents as "Automatically emit param events for the given ids when they are
// changed", plus the node's own `info` event whose `change_mask` names `PW_NODE_CHANGE_MASK_PARAMS`. So
// nothing here asks the daemon anything on a schedule, and a volume changed by any other tool on the
// desktop — `wpctl`, a mixer, a headset's own button — arrives as a callback.
//
// What it reads, and where each name was verified rather than recalled:
//
//   * the `default` metadata object, key `default.audio.sink`, value `{"name":"<node.name>"}` with type
//     `Spa:String:JSON` — read back from this session with `pw-metadata -n default`, and the object form
//     is the one `/usr/share/pipewire/pipewire.conf`'s own commented example writes;
//   * a sink is a node whose `media.class` is `Audio/Sink`, and the metadata names it by `node.name`;
//   * the volume is `SPA_PROP_channelVolumes`, an array of floats, and the mute state `SPA_PROP_mute` —
//     both in `spa/param/props.h`. Neither is in the node's *properties* on this session, which was
//     checked rather than assumed: `audio.volume` and `audio.mute` are absent from the sink's prop dict,
//     so the Props param is the only place the reading exists.
//
// The percentage convention — the cube root of the linear factor, WirePlumber's own — is in
// `AudioVolume.h` beside the functions that apply it, with the measurement that establishes it.
//
// **The default sink is the only sink this reads.** A per-sink control is the control centre's, and the
// bar has one volume to show: guessing a sink when the metadata names none would put a different device's
// number on the bar, so an unnamed default is `available == false` and a dash, not a fallback.
//
// **Threading.** PipeWire's callbacks run on the service's own thread loop, and QML reads the properties
// on the GUI thread, so every reading crosses through a queued invocation and nothing shared is touched
// from both. A write goes the other way: `pw_node_set_param` is called under the thread loop's lock from
// the GUI thread, which is the documented way to talk to a proxy owned by another thread.
#pragma once

// The pure half, for the two things this header's surface is made of that live there: `WheelStepUnit`, which
// says which unit a notch is measured in, and the token it is resolved from. Including it costs nothing that
// the comment below is about — `AudioVolume.h` has no SPA include and only forward-declares `spa_pod`, so a
// header that registers a QML type still pulls no `libpipewire` into the schema, the IPC or a test.
#include "audio/AudioVolume.h"

#include <QObject>
#include <QString>

#include <memory>

namespace quantum::audio {

class PipeWireService : public QObject {
    Q_OBJECT

public:
    // The wheel's step in percentage points, and the value `[bar.audio].step_percent` defaults to. Stated
    // here because this is the code that applies it: 5 points is one twentieth of full scale, which on a
    // 100-point scale is coarse enough that four turns cover a fifth of the range and fine enough that a
    // deliberate single turn is visible. `ConfigSchema.h` declares the same number as the file's default
    // and `config-test` compares the two at compile time, so the two spellings cannot drift — the same
    // rule the sampling cadence is held to.
    inline static constexpr int DefaultStepPercent = 5;

    // The shortest step the service will apply. One point, because a step of zero is a wheel that does
    // nothing at all — not a small step, no step — and that is a setting to refuse by name rather than to
    // accept and quietly ignore. `ConfigSchema.h` declares the same floor as the file's, and `config-test`
    // compares the two at compile time.
    inline static constexpr int MinimumStepPercent = 1;

    // The wheel's step in decibels, and the value `[bar.audio].step_decibels` defaults to. It applies when the
    // unit below is `Decibel`, which is the same answer the readout's own unit is — see `AudioVolume.h` for
    // why one question has one answer.
    //
    // One decibel, because that is the step a mixer's own coarse control has and because it is a distance a
    // person can hear as a deliberate change rather than a jitter: about a tenth of the range the desktop's
    // percentage calls a tenth of full scale at the top, and seven of them double or halve the amplitude.
    inline static constexpr double DefaultStepDecibels = 1.0;

    // The shortest step this service will apply in decibels, and it is the readout's own resolution: the bar
    // draws decibels to one decimal place, so a step below a tenth would be a wheel notch that changes nothing
    // a person can see. It is the counterpart of the percentage floor above, which is one point of a readout
    // drawn as whole points — the floor of a step is the resolution of the unit it is measured in.
    // `ConfigSchema.h` declares the same floor as the file's, and `config-test` compares the two at compile
    // time.
    inline static constexpr double MinimumStepDecibels = 0.1;

    // The ceiling the shell writes and the bar's percentage is clamped to. 100 is what every other volume
    // control on the desktop calls full scale; PipeWire itself allows above it — `channelmix.max-volume`
    // defaults to a linear 10.0 — but amplifying past unity is a deliberate act rather than a wheel turned
    // twice too far, and it belongs with the control centre, not with a scroll gesture on a bar.
    inline static constexpr int MaxPercent = 100;

    explicit PipeWireService(QObject* parent = nullptr);
    // Defined out of line because `Impl` is incomplete here; it detaches, so a service never outlives its
    // thread loop.
    ~PipeWireService() override;

    Q_PROPERTY(bool available READ available NOTIFY readingChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY readingChanged)
    Q_PROPERTY(int volumePercent READ volumePercent NOTIFY readingChanged)
    Q_PROPERTY(double volumeDecibels READ volumeDecibels NOTIFY readingChanged)
    Q_PROPERTY(int stepPercent READ stepPercent WRITE setStepPercent NOTIFY stepPercentChanged)
    Q_PROPERTY(double stepDecibels READ stepDecibels WRITE setStepDecibels NOTIFY stepDecibelsChanged)

    // Whether the shell has a reading for the sink the metadata names as the default. False before the
    // first Props param arrives, while the daemon is away, and when the metadata names no sink — the three
    // moments there is no volume to show, which are one flag because the widget draws the same thing for
    // all of them.
    bool available() const { return available_; }

    // The sink's mute state, and its volume in the two units a person can be shown. All three are only
    // meaningful while `available()` is true: unlike `SysMonService`'s CPU percentage there is no "one reading
    // is a baseline" here — a Props param *is* the volume — so these are simply the last reading, and the flag
    // is what says whether there has been one. Both numbers are 0 while unavailable rather than arbitrary,
    // because neither type has an empty value and the widget is written to read the flag.
    //
    // They are two renderings of *one* factor and are computed from it in one place, on the PipeWire thread,
    // so they cannot disagree: the percentage is WirePlumber's cube root — what every other volume control on
    // the desktop shows — and the decibels are the physical gain, `20 * log10(linear)`, which is what the
    // desktop's other volume tool prints. Neither is derivable from the other, which is why both cross the
    // thread boundary rather than one of them being recomputed by whichever side draws it; `AudioVolume.h`
    // carries both conventions and the measurements that establish them.
    bool muted() const { return muted_; }
    int volumePercent() const { return volumePercent_; }
    double volumeDecibels() const { return volumeDecibels_; }

    // How far one wheel notch moves the volume, the value `[bar.audio].step_percent` carries. Settable
    // because the step is the person's: the composition root hands the configured value over before the
    // engine loads and follows later edits. A value below 1 is refused rather than accepted, because a
    // step of zero is a wheel that does nothing and a negative one is a wheel that turns backwards.
    int stepPercent() const { return stepPercent_; }
    void setStepPercent(int percent);

    // The same distance in the other unit, the value `[bar.audio].step_decibels` carries, with the same rules:
    // settable because it is the person's, a value below `MinimumStepDecibels` refused by name rather than
    // accepted, and a notify signal so a binding — the bar's own documentation of itself, a live test reading
    // the running shell — can follow it.
    double stepDecibels() const { return stepDecibels_; }
    void setStepDecibels(double decibels);

    // Which of the two steps a notch applies, read by every wheel gesture the bar sends. Set from
    // `[bar.audio].volume_scale` by the composition root — the same token the readout is drawn in, because a
    // notch measured in one unit while the person reads another is the disagreement this setting exists to
    // remove — and resolved from its token by `wheelStepUnitFromToken`, which refuses a spelling this module
    // does not know.
    //
    // Deliberately *not* a `Q_PROPERTY`, unlike the two steps: no binding has a use for it. What QML reads is
    // the unit it draws, which is `Config`'s, and what it asks for is a notch — which unit that notch is
    // measured in is not the widget's to choose, or the bar's rendering would be deciding the volume.
    WheelStepUnit wheelStepUnit() const { return wheelStepUnit_; }
    void setWheelStepUnit(WheelStepUnit unit);
    // The same, from the token `[bar.audio].volume_scale` carries — the door the composition root uses, so the
    // mapping from a file's spelling to this module's enum happens inside the module that has to honour it
    // rather than in a line of `main.cpp` that no test can call. An unknown spelling is refused by name and the
    // unit stays where it was; `AudioVolume.h` says why the token's mirror in the schema is a compile-time
    // check rather than a shared constant.
    void setWheelStepUnit(QStringView token);

    // Attaches to the daemon and begins following the default sink. Called once from the composition root;
    // a failure to attach is not fatal — the shell keeps running with the bar's volume readout drawing its
    // empty state — and the service retries with backoff, so a daemon started after the shell is found.
    //
    // `remoteName` is the PipeWire remote to attach to, by the library's own name for one: the
    // `remote.name` property from `keys.h`, the thing `pw-cli -r` sets and the string a daemon's
    // `core.name` gives it. Empty — the default, and what the shell passes — means PipeWire's own default,
    // which on a session is the desktop's daemon. A test names its own so that it can never reach the
    // session's, which is the same reason `SysMonService` takes the directory it reads from.
    void start(const QString& remoteName = QString());

    // Detaches from the daemon and withdraws the reading, and stops the retrying: nothing is attached and
    // nothing will be attempted until `start()` again. The destructor calls it, so a service that is going
    // away never leaves a thread loop or a pending backoff behind. It is public because a caller that keeps
    // the object while the daemon under it changes — a test that stops its own daemon, a future session
    // handover — has to be able to end the connection without destroying the object.
    void stop();

    // The gestures the bar's volume widget performs, as `NiriActions` performs the strip's. Each is a
    // request to the daemon rather than a local change: what is published afterwards is the daemon's own
    // answer, so the bar can never drift from the mixer.
    Q_INVOKABLE void toggleMute();
    // One wheel notch: `direction` is +1 louder, -1 quieter, anything else ignored. The step comes from
    // `stepPercent`, and a step while muted changes the volume without unmuting — which is what
    // `wpctl set-volume` does on a muted sink, and the behaviour a wheel that also unmuted would surprise
    // someone with.
    Q_INVOKABLE void stepVolume(int direction);
    Q_INVOKABLE void setVolumePercent(int percent);

    // Registers `service` so a QML file can bind to it:
    //
    //     import QuantumShell 1.0
    //     Text { text: PipeWireService.available ? PipeWireService.volumePercent + "%" : "—" }
    //
    // The instance stays owned by C++; QML only reads it and calls its gestures. The module URI and
    // version come from `QmlModule.h`, the one place they are written down.
    static void registerQmlSingleton(PipeWireService& service);

    // The type name above, declared once, for the reason `SysMonService`'s is: a QML file is written
    // against it, so a rename that is not a rename everywhere fails at load time rather than at build
    // time. `audio_test.cpp` mirrors it and compares its copy at compile time.
    inline static constexpr auto QmlTypeName = "PipeWireService";

signals:
    void readingChanged();
    void stepPercentChanged();
    void stepDecibelsChanged();
    void wheelStepUnitChanged();

private:
    // Everything PipeWire-shaped, defined in the .cpp so that no header that registers a QML type pulls
    // `libpipewire` into the configuration schema, the IPC or the bar's own test.
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Applies a reading to the published properties. Reached only through `Impl`'s queued invocation, so
    // it always runs on the GUI thread and the fields below need no lock: a reading arrives whole, and QML
    // never reads a half-applied pair. A nested class is a member of its enclosing class and so already has
    // access here — no friendship is needed, and the sink's name is carried only so a change can be logged
    // where the reading is published rather than from the PipeWire thread's own output.
    void applyReading(bool available, bool muted, int percent, double decibels, const QString& sinkName);

    bool available_ = false;
    bool muted_ = false;
    int volumePercent_ = 0;
    // Negative infinity for a reading of silence, which is what the factor of a muted-to-zero sink is worth
    // in decibels: the widget draws that as the mathematical symbol rather than as a number, and
    // `decibelsFromLinear` says why a floor would be a lie.
    double volumeDecibels_ = 0.0;
    int stepPercent_ = DefaultStepPercent;
    double stepDecibels_ = DefaultStepDecibels;
    // The default is the schema's default, and it is the unit the desktop's other controls are in: a shell that
    // has not been configured agrees with `wpctl get-volume` both in what it shows and in how far a notch moves.
    WheelStepUnit wheelStepUnit_ = WheelStepUnit::Percent;
};

}  // namespace quantum::audio
