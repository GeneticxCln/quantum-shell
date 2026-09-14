#include "config/Config.h"

#include "QmlModule.h"

#include <QQmlEngine>

namespace quantum::config {

ConfigBar::ConfigBar(QObject* parent) : QObject(parent) {}

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
}

Config::Config(QObject* parent) : QObject(parent) {
    bar_.setParent(this);
}

void Config::apply(const ConfigValues& values) {
    bar_.apply(values.bar);
}

ConfigValues Config::values() const {
    // Rebuilt from the tree rather than cached beside it: the tree is the applied state, and a second copy
    // kept in step by hand is a second thing that can be wrong. There is exactly one table today, so this
    // is a copy and not a walk.
    ConfigValues values;
    values.bar = bar_.values();
    return values;
}

void Config::registerQmlSingleton(Config& config) {
    // `ConfigBar` is not a type QML can name or create, but `Config.bar` hands one to a binding, and QML
    // reaches its properties through its meta-object. Anonymous registration is what says "known to the
    // engine, not part of the module's interface" — a named registration would add a second public name
    // for a class no QML file has any reason to construct.
    qmlRegisterAnonymousType<ConfigBar>(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion);
    qmlRegisterSingletonInstance(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion,
                                 quantum::qml::ModuleMinorVersion, "Config", &config);
}

}  // namespace quantum::config
