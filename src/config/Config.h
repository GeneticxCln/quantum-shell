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

// The `[bar]` table as QML reads it: `Config.bar`.
class ConfigBar : public QObject {
    Q_OBJECT

    Q_PROPERTY(int height READ height NOTIFY heightChanged)
    Q_PROPERTY(QString layerNamespace READ layerNamespace NOTIFY layerNamespaceChanged)

public:
    explicit ConfigBar(QObject* parent = nullptr);

    int height() const { return values_.height; }
    QString layerNamespace() const { return values_.layerNamespace; }

    // The validated values themselves, for a caller that needs the struct rather than the properties —
    // `qsctl config get` resolves a key path against these, and doing that through QML-visible properties
    // would mean one lookup per leaf instead of one lookup in one place.
    const BarConfig& values() const { return values_; }

    // Applies validated values, emitting a signal for each property whose value actually changed and
    // nothing at all for the rest.
    void apply(const BarConfig& values);

Q_SIGNALS:
    void heightChanged();
    void layerNamespaceChanged();

private:
    BarConfig values_;
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

private:
    ConfigBar bar_;
};

}  // namespace quantum::config
