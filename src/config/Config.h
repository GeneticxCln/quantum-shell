// The configuration as QML reads it.
//
// `ConfigSchema` produces validated C++ values and `ConfigWatcher` decides when to read them; this is
// the object a binding is written against:
//
//     Bar { height: Config.bar.height }
//
// Each property has a NOTIFY signal of its own rather than one signal for the whole tree, and `apply`
// compares every value before emitting anything. That is the point of the class and the reason it is not
// a `QVariantMap`: a bar whose height did not change must not be asked to resize, and a QML binding must
// not re-evaluate because an unrelated key was edited. The design states it as
// `Config.bar.heightChanged()` (QUANTUM_SHELL.md § Configuration), and it is also why the tree is nested
// objects rather than one flat list of properties with dotted names — `Config.bar` is the `[bar]` table,
// and only the leaf that actually changed speaks.
//
// Nothing here loads, parses or watches anything: this object only holds the current values and reports
// a change. That is what lets the diffing be tested without a file, and the file be tested without a QML
// engine.
#pragma once

#include "config/ConfigSchema.h"

#include <QObject>
#include <QString>

namespace quantum::config {

// The `[bar.system]` table as QML reads it: `Config.bar.system`.
//
// A nested object rather than a `sampleIntervalMs` on `ConfigBar` for two reasons. The first is that the
// file has a `[bar.system]` table and this tree mirrors the file's tables, so the path a user edits and
// the path a binding writes resolve to the same place. The second is that the readouts still to come each
// own a table of their own, and one of them existing is what makes that shape real rather than declared.
//
// **It writes into `ConfigBar`'s `BarConfig`, not into a copy of it.** The object holds a reference to the
// `system` field of the parent's validated values, so the values `Config::values()` hands to `qsctl config
// get` and the values these properties report are the same values — a second `SystemConfig` beside the
// first is exactly the kind of copy that drifts, and the parent's own comment says why there is one place
// a default is written down.
class ConfigSystem : public QObject {
    Q_OBJECT

    Q_PROPERTY(int sampleIntervalMs READ sampleIntervalMs NOTIFY sampleIntervalMsChanged)
    Q_PROPERTY(bool showCpu READ showCpu NOTIFY showCpuChanged)
    Q_PROPERTY(bool showMemory READ showMemory NOTIFY showMemoryChanged)
    // A string rather than an enum, because the token is what a file says and what the widget switches on:
    // an int in between would be a third spelling to keep in step with the two that already exist, and the
    // schema has already refused every token the widget has no branch for. `MemoryFormats` in
    // `ConfigSchema.h` is the list, and `bar-interaction-test` drives each of them through the shipped QML.
    Q_PROPERTY(QString memoryFormat READ memoryFormat NOTIFY memoryFormatChanged)

public:
    // Takes the table it reports on by reference. There is no way to construct one without a table to
    // write into, which is the intended constraint: this object has no values of its own to fall back to.
    explicit ConfigSystem(SystemConfig& values, QObject* parent = nullptr);

    int sampleIntervalMs() const { return values_.sampleIntervalMs; }
    bool showCpu() const { return values_.showCpu; }
    bool showMemory() const { return values_.showMemory; }
    QString memoryFormat() const { return values_.memoryFormat; }

    const SystemConfig& values() const { return values_; }

    // Applies validated values, emitting a signal for each property whose value actually changed.
    void apply(const SystemConfig& values);

Q_SIGNALS:
    void sampleIntervalMsChanged();
    void showCpuChanged();
    void showMemoryChanged();
    void memoryFormatChanged();

private:
    SystemConfig& values_;
};

// The `[bar.audio]` table as QML reads it: `Config.bar.audio`.
//
// The same shape as `ConfigSystem` and for the same two reasons — the tree mirrors the file's tables, and
// the widget that owns a setting owns a table. It is a separate class rather than a second set of
// properties on `ConfigBar` because the two tables belong to two different widgets: a signal on the bar's
// own object would re-evaluate bindings under `Config.bar` for a change to the volume step.
//
// Like `ConfigSystem` it writes into `ConfigBar`'s `BarConfig` rather than into a copy, so `qsctl config
// get bar.audio.step_percent` and `Config.bar.audio.stepPercent` are one value and not two.
class ConfigAudio : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool showVolume READ showVolume NOTIFY showVolumeChanged)
    Q_PROPERTY(QString volumeScale READ volumeScale NOTIFY volumeScaleChanged)
    Q_PROPERTY(int stepPercent READ stepPercent NOTIFY stepPercentChanged)
    Q_PROPERTY(double stepDecibels READ stepDecibels NOTIFY stepDecibelsChanged)

public:
    // Takes the table it reports on by reference: there is no way to construct one without a table to
    // write into, which is the intended constraint.
    explicit ConfigAudio(AudioConfig& values, QObject* parent = nullptr);

    bool showVolume() const { return values_.showVolume; }
    // The unit the readout is drawn in, as the token the schema accepted: `percent` or `decibel`. Two things
    // read it and they are the two halves of one answer: the widget switches on this string to draw the
    // readout, and the service is handed the same token so that a wheel notch is measured in the unit the
    // person is reading. It is why the schema refuses a spelling outside its list rather than keeping the
    // default silently — the three sides are one list and not three.
    QString volumeScale() const { return values_.volumeScale; }
    // How far one notch moves the volume in each unit. Both are published whether or not they are the one in
    // force: the file's other value is the one that applies when the unit changes, so hiding it would make an
    // edit to the unit land on a value nothing had read.
    int stepPercent() const { return values_.stepPercent; }
    double stepDecibels() const { return values_.stepDecibels; }

    const AudioConfig& values() const { return values_; }

    // Applies validated values, emitting a signal for each property whose value actually changed.
    void apply(const AudioConfig& values);

Q_SIGNALS:
    void showVolumeChanged();
    void volumeScaleChanged();
    void stepPercentChanged();
    void stepDecibelsChanged();

private:
    AudioConfig& values_;
};

// The `[bar.network]` table as QML reads it: `Config.bar.network`. Holds a reference into the parent's own
// `NetworkConfig`, the same contract the two tables above have, so the values `qsctl config get` reports and the
// values these properties expose are one set rather than two kept in step by hand.
class ConfigNetwork : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool showStatus READ showStatus NOTIFY showStatusChanged)
    Q_PROPERTY(bool showName READ showName NOTIFY showNameChanged)
    Q_PROPERTY(bool showStrength READ showStrength NOTIFY showStrengthChanged)

public:
    explicit ConfigNetwork(NetworkConfig& values, QObject* parent = nullptr);

    // Whether the readout is drawn at all, whether the connection's own name is drawn beside it, and whether a
    // wireless connection's signal quality is. All three are read by the widget and none by the service: what
    // the service follows is the connection, and a hidden readout is not a switch on the following.
    bool showStatus() const { return values_.showStatus; }
    bool showName() const { return values_.showName; }
    bool showStrength() const { return values_.showStrength; }

    const NetworkConfig& values() const { return values_; }

    // Applies validated values, emitting a signal for each property whose value actually changed.
    void apply(const NetworkConfig& values);

Q_SIGNALS:
    void showStatusChanged();
    void showNameChanged();
    void showStrengthChanged();

private:
    NetworkConfig& values_;
};

// The `[bar.battery]` table as QML reads it: `Config.bar.battery`. Holds a reference into the parent's own
// `BatteryConfig`, the same contract the three tables above have, so the values `qsctl config get` reports and
// the values these properties expose are one set rather than two kept in step by hand.
class ConfigBattery : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool showStatus READ showStatus NOTIFY showStatusChanged)
    Q_PROPERTY(bool showPercentage READ showPercentage NOTIFY showPercentageChanged)
    Q_PROPERTY(bool showTime READ showTime NOTIFY showTimeChanged)

public:
    explicit ConfigBattery(BatteryConfig& values, QObject* parent = nullptr);

    // Whether the readout is drawn at all, whether the charge level is drawn beside it, and whether the time the
    // daemon reports is. All three are read by the widget and none by the service: what the service follows is
    // the daemon, and a hidden readout is not a switch on the following.
    bool showStatus() const { return values_.showStatus; }
    bool showPercentage() const { return values_.showPercentage; }
    bool showTime() const { return values_.showTime; }

    const BatteryConfig& values() const { return values_; }

    // Applies validated values, emitting a signal for each property whose value actually changed.
    void apply(const BatteryConfig& values);

Q_SIGNALS:
    void showStatusChanged();
    void showPercentageChanged();
    void showTimeChanged();

private:
    BatteryConfig& values_;
};

// The `[bar.media]` table as QML reads it: `Config.bar.media`.
//
// The same shape as `ConfigBattery` and for the same reason: the tree mirrors the file's tables, and this
// widget owns a setting. One flag, read by the widget and not by the service: what the service follows is
// MPRIS2 players, and a hidden readout is not a switch on the following.
//
// Like the tables above it writes into `ConfigBar`'s `BarConfig` rather than into a copy, so `qsctl config
// get bar.media.show_media` and `Config.bar.media.showMedia` are one value.
class ConfigMedia : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool showMedia READ showMedia NOTIFY showMediaChanged)

public:
    explicit ConfigMedia(MediaConfig& values, QObject* parent = nullptr);

    // Whether the readout is drawn at all. Read by the widget and not by the service: what the service
    // follows is the most recently active MPRIS2 player, and a hidden readout is not a switch on that.
    bool showMedia() const { return values_.showMedia; }

    const MediaConfig& values() const { return values_; }

    // Applies validated values, emitting a signal for each property whose value actually changed.
    void apply(const MediaConfig& values);

Q_SIGNALS:
    void showMediaChanged();

private:
    MediaConfig& values_;
};

// The `[bar.notifications]` table as QML reads it: `Config.bar.notifications`. One property, one signal,
// the shape every other readout's table takes.
class ConfigNotifications : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool showNotifications READ showNotifications NOTIFY showNotificationsChanged)

public:
    explicit ConfigNotifications(NotificationsConfig& values, QObject* parent = nullptr);

    // Whether the readout is drawn at all. Read by the widget and not by the service, for the same reason
    // `showMedia` is: owning the notifications name is not switched by drawing, and the shell takes the
    // name whether or not the readout is on screen.
    bool showNotifications() const { return values_.showNotifications; }

    const NotificationsConfig& values() const { return values_; }

    // Applies validated values, emitting a signal for each property whose value actually changed.
    void apply(const NotificationsConfig& values);

Q_SIGNALS:
    void showNotificationsChanged();

private:
    NotificationsConfig& values_;
};


// The `[bar]` table as QML reads it: `Config.bar`.
class ConfigBar : public QObject {
    Q_OBJECT

    Q_PROPERTY(int height READ height NOTIFY heightChanged)
    Q_PROPERTY(QString layerNamespace READ layerNamespace NOTIFY layerNamespaceChanged)
    // Constant, like `Config.bar` itself: the object is stable for the shell's life and the values inside
    // it speak through their own signals.
    Q_PROPERTY(quantum::config::ConfigNotifications* notifications READ notifications CONSTANT)
    Q_PROPERTY(quantum::config::ConfigSystem* system READ system CONSTANT)
    Q_PROPERTY(quantum::config::ConfigAudio* audio READ audio CONSTANT)
    Q_PROPERTY(quantum::config::ConfigNetwork* network READ network CONSTANT)
    Q_PROPERTY(quantum::config::ConfigBattery* battery READ battery CONSTANT)
    Q_PROPERTY(quantum::config::ConfigMedia* media READ media CONSTANT)

public:
    explicit ConfigBar(QObject* parent = nullptr);

    int height() const { return values_.height; }
    QString layerNamespace() const { return values_.layerNamespace; }

    ConfigSystem* system() { return &system_; }
    ConfigAudio* audio() { return &audio_; }
    ConfigNetwork* network() { return &network_; }
    ConfigBattery* battery() { return &battery_; }
    ConfigMedia* media() { return &media_; }

    // The validated values themselves, for a caller that needs the struct rather than the properties —
    // `qsctl config get` resolves a key path against these, and doing that through QML-visible properties
    // would mean one lookup per leaf instead of one lookup in one place.
    const BarConfig& values() const { return values_; }

    // Applies validated values, emitting a signal for each property whose value actually changed and
    // nothing at all for the rest.
    void apply(const BarConfig& values);
    ConfigNotifications* notifications() { return &notifications_; }

Q_SIGNALS:
    void heightChanged();
    void layerNamespaceChanged();

private:
    BarConfig values_;
    // Declared after `values_` because each refers to a field of it, and constructed with that reference.
    // The order here is also the construction order, which is why all six follow the struct they point into.
    ConfigSystem system_;
    ConfigAudio audio_;
    ConfigNetwork network_;
    ConfigBattery battery_;
    ConfigMedia media_;
    ConfigNotifications notifications_;
};

// The whole configuration, registered with QML as the `Config` singleton.
class Config : public QObject {
    Q_OBJECT

    // Constant: the object behind it is stable for the shell's life and changes are announced by its own
    // signals. A `barChanged` here as well would re-evaluate every binding under `Config.bar` on any
    // edit, which is the coarse notification this class exists to avoid.
    Q_PROPERTY(quantum::config::ConfigBar* bar READ bar CONSTANT)

public:
    explicit Config(QObject* parent = nullptr);

    ConfigBar* bar() { return &bar_; }

    void apply(const ConfigValues& values);

    // The whole validated configuration as it stands, which is what a non-QML consumer reads a value from.
    ConfigValues values() const;

    // Registers `config` so a QML file can bind to it. The instance stays owned by C++; QML only reads
    // it. The module URI and version come from `QmlModule.h`, which is the one place they are written
    // down, because they are the same module `NiriService` is registered into.
    static void registerQmlSingleton(Config& config);

    // The name the singleton is registered under, declared once. It is interface of the same kind as
    // `NiriService`'s and `SysMonService`'s — a QML file is written against it, so a rename that is not a
    // rename everywhere would fail at load time rather than at build time. `bar-interaction-test` mirrors it
    // and compares its copy at compile time, because it registers this object for the bar it loads.
    inline static constexpr auto QmlTypeName = "Config";

private:
    ConfigBar bar_;
};

}  // namespace quantum::config
