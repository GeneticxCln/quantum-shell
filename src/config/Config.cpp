#include "config/Config.h"

#include "QmlModule.h"

#include <QQmlEngine>

namespace quantum::config {

ConfigSystem::ConfigSystem(SystemConfig& values, QObject* parent) : QObject(parent), values_(values) {}

void ConfigSystem::apply(const SystemConfig& values) {
    // Compared before assigning, like every other leaf: a value that did not change must not wake a
    // binding — and here it must not re-arm a timer either, because the service connected to this signal
    // re-arms on every emission.
    if (values_.sampleIntervalMs != values.sampleIntervalMs) {
        values_.sampleIntervalMs = values.sampleIntervalMs;
        emit sampleIntervalMsChanged();
    }
    // Each of the three below has a binding of its own on the widget, so each speaks for itself: hiding the
    // CPU readout must not re-evaluate the memory one's form, and a change to the form must not resize
    // anything.
    if (values_.showCpu != values.showCpu) {
        values_.showCpu = values.showCpu;
        emit showCpuChanged();
    }
    if (values_.showMemory != values.showMemory) {
        values_.showMemory = values.showMemory;
        emit showMemoryChanged();
    }
    if (values_.memoryFormat != values.memoryFormat) {
        values_.memoryFormat = values.memoryFormat;
        emit memoryFormatChanged();
    }
}

ConfigAudio::ConfigAudio(AudioConfig& values, QObject* parent) : QObject(parent), values_(values) {}

ConfigColors::ConfigColors(ColorsConfig& values, QObject* parent) : QObject(parent), values_(values) {}

ConfigFont::ConfigFont(FontConfig& values, QObject* parent) : QObject(parent), values_(values) {}

void ConfigFont::apply(const FontConfig& values) {
    // Each value compared on its own, the way every table here applies: a size change reflows the text and
    // leaves the family and the weight where they were.
    if (values_.family != values.family) {
        values_.family = values.family;
        emit familyChanged();
    }
    if (values_.size != values.size) {
        values_.size = values.size;
        emit sizeChanged();
    }
    if (values_.weight != values.weight) {
        values_.weight = values.weight;
        emit weightChanged();
    }
}

void ConfigColors::apply(const ColorsConfig& values) {
    // Each colour compared on its own, so an edit to the accent repaints what the accent marks and leaves
    // everything else alone — which is the difference between changing a colour and reloading the bar.
    if (values_.foreground != values.foreground) {
        values_.foreground = values.foreground;
        emit foregroundChanged();
    }
    if (values_.muted != values.muted) {
        values_.muted = values.muted;
        emit mutedChanged();
    }
    if (values_.accent != values.accent) {
        values_.accent = values.accent;
        emit accentChanged();
    }
    if (values_.urgent != values.urgent) {
        values_.urgent = values.urgent;
        emit urgentChanged();
    }
}

void ConfigAudio::apply(const AudioConfig& values) {
    // Each property compared on its own, so an edit to one key emits exactly one signal and hiding the
    // readout does not wake a binding on the step.
    if (values_.showVolume != values.showVolume) {
        values_.showVolume = values.showVolume;
        emit showVolumeChanged();
    }
    if (values_.volumeScale != values.volumeScale) {
        values_.volumeScale = values.volumeScale;
        emit volumeScaleChanged();
    }
    if (values_.stepPercent != values.stepPercent) {
        values_.stepPercent = values.stepPercent;
        emit stepPercentChanged();
    }
    if (values_.stepDecibels != values.stepDecibels) {
        values_.stepDecibels = values.stepDecibels;
        emit stepDecibelsChanged();
    }
}

ConfigNetwork::ConfigNetwork(NetworkConfig& values, QObject* parent) : QObject(parent), values_(values) {}

void ConfigNetwork::apply(const NetworkConfig& values) {
    // Each property compared on its own, so an edit to one key emits exactly one signal and switching off the
    // name does not wake a binding on the signal quality.
    if (values_.showStatus != values.showStatus) {
        values_.showStatus = values.showStatus;
        emit showStatusChanged();
    }
    if (values_.showName != values.showName) {
        values_.showName = values.showName;
        emit showNameChanged();
    }
    if (values_.showStrength != values.showStrength) {
        values_.showStrength = values.showStrength;
        emit showStrengthChanged();
    }
}

ConfigBattery::ConfigBattery(BatteryConfig& values, QObject* parent) : QObject(parent), values_(values) {}

void ConfigBattery::apply(const BatteryConfig& values) {
    // Each property compared on its own, so an edit to one key emits exactly one signal and switching off the
    // level does not wake a binding on the time.
    if (values_.showStatus != values.showStatus) {
        values_.showStatus = values.showStatus;
        emit showStatusChanged();
    }
    if (values_.showPercentage != values.showPercentage) {
        values_.showPercentage = values.showPercentage;
        emit showPercentageChanged();
    }
    if (values_.showTime != values.showTime) {
        values_.showTime = values.showTime;
        emit showTimeChanged();
    }
}

ConfigMedia::ConfigMedia(MediaConfig& values, QObject* parent) : QObject(parent), values_(values) {}

void ConfigMedia::apply(const MediaConfig& values) {
    if (values_.showMedia != values.showMedia) {
        values_.showMedia = values.showMedia;
        emit showMediaChanged();
    }
}

ConfigNotifications::ConfigNotifications(NotificationsConfig& values, QObject* parent)
    : QObject(parent), values_(values) {}

void ConfigNotifications::apply(const NotificationsConfig& values) {
    if (values_.showNotifications != values.showNotifications) {
        values_.showNotifications = values.showNotifications;
        emit showNotificationsChanged();
    }
    // The toast's default expiry, on the same signal as the readout's visibility: both are a change to one
    // table, and the toast's expiry is the value a sender's `-1` resolves to at the moment it is read rather
    // than at the moment the file was written.
    if (values_.timeoutMs != values.timeoutMs) {
        values_.timeoutMs = values.timeoutMs;
        emit showNotificationsChanged();
    }
}

ConfigOsd::ConfigOsd(OsdConfig& values, QObject* parent) : QObject(parent), values_(values) {}

void ConfigOsd::apply(const OsdConfig& values) {
    if (values_.showOsd != values.showOsd) {
        values_.showOsd = values.showOsd;
        emit showOsdChanged();
    }
    if (values_.timeoutMs != values.timeoutMs) {
        values_.timeoutMs = values.timeoutMs;
        emit timeoutMsChanged();
    }
}

ConfigBar::ConfigBar(QObject* parent)
    : QObject(parent), system_(values_.system, this), audio_(values_.audio, this),
      network_(values_.network, this), battery_(values_.battery, this), media_(values_.media, this),
      notifications_(values_.notifications, this), osd_(values_.osd, this), colors_(values_.colors, this),
      font_(values_.font, this) {}

void ConfigBar::apply(const BarConfig& values) {
    // Each property is compared on its own, so an edit to one key emits exactly one signal. Assigning
    // first and emitting unconditionally would be simpler and would wake every binding under
    // `Config.bar` on every keystroke in the file.
    if (values_.height != values.height) {
        values_.height = values.height;
        emit heightChanged();
    }
    if (values_.layerNamespace != values.layerNamespace) {
        values_.layerNamespace = values.layerNamespace;
        emit layerNamespaceChanged();
    }
    // The nested tables diff themselves, on the same values: each holds a reference into a field of
    // `values_`, so by the time this returns, `values()` reports what the file said as well as these two
    // properties do.
    system_.apply(values.system);
    audio_.apply(values.audio);
    network_.apply(values.network);
    battery_.apply(values.battery);
    notifications_.apply(values.notifications);
    osd_.apply(values.osd);
    media_.apply(values.media);
    colors_.apply(values.colors);
    font_.apply(values.font);
}

ConfigLauncher::ConfigLauncher(LauncherConfig& values, QObject* parent) : QObject(parent), values_(values) {}

void ConfigLauncher::apply(const LauncherConfig& values) {
    if (values_.maxResults != values.maxResults) {
        values_.maxResults = values.maxResults;
        emit maxResultsChanged();
    }
}

Config::Config(QObject* parent) : QObject(parent), launcher_(launcherValues_) {
    bar_.setParent(this);
    launcher_.setParent(this);
}

void Config::apply(const ConfigValues& values) {
    bar_.apply(values.bar);
    launcher_.apply(values.launcher);
}

ConfigValues Config::values() const {
    // Rebuilt from the tree rather than cached beside it: the tree is the applied state, and a second copy
    // kept in step by hand is a second thing that can be wrong. `ConfigBar::values()` already carries both
    // nested tables — each writes into a field of the struct it returns — so this is a copy of one table
    // and not a walk, and neither table has a second place its values live.
    ConfigValues values;
    values.bar = bar_.values();
    values.launcher = launcher_.values();
    return values;
}

void Config::registerQmlSingleton(Config& config) {
    // The nested classes are not types QML can name or create, but `Config.bar` and its five tables hand one of
    // each to a binding, and QML reaches their properties through their meta-objects. Anonymous registration is
    // what says "known to the engine, not part of the module's interface" — a named registration would add a
    // second public name for a class no QML file has any reason to construct.
    qmlRegisterAnonymousType<ConfigLauncher>(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion);
    qmlRegisterAnonymousType<ConfigSystem>(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion);
    qmlRegisterAnonymousType<ConfigAudio>(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion);
    qmlRegisterAnonymousType<ConfigNetwork>(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion);
    qmlRegisterAnonymousType<ConfigBattery>(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion);
    qmlRegisterAnonymousType<ConfigBar>(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion);
    qmlRegisterAnonymousType<ConfigColors>(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion);
    qmlRegisterAnonymousType<ConfigFont>(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion);
    qmlRegisterSingletonInstance(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion,
                                 quantum::qml::ModuleMinorVersion, Config::QmlTypeName, &config);
}

}  // namespace quantum::config
