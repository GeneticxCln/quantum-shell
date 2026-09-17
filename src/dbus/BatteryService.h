// The battery readout's connection to UPower: the charge the daemon reports, whether it is going in or out, how
// long it says that will take and whether it is worried — exposed to QML, kept current by events.
//
// This module exists because UPower publishes `PropertiesChanged` on both objects it has that matter: the
// manager, whose `OnBattery` is the machine's own answer about where its power is coming from, and the single
// device the daemon names as the thing a bar should draw. A level that moves, a cable that is plugged in and a
// battery that went flat each arrive as a signal, which is the same shape the audio and network modules have and
// the opposite of `src/system/` — so `src/dbus/` takes no waiver: nothing here asks the daemon anything on a
// schedule. What it asks for is asked *once per object*, when the daemon's own signals say the chain moved.
//
// What it reads, and where each name was verified rather than recalled — every one of them read off this session
// with `busctl --system introspect org.freedesktop.UPower`, the daemon's own description of what it serves:
//
//   * the manager object on the well-known name `org.freedesktop.UPower`, whose `OnBattery` is `b false` on this
//     desktop, beside its `DaemonVersion` (`1.91.4`) and `LidIsClosed`;
//   * the device `GetDisplayDevice()` names — `o "/org/freedesktop/UPower/devices/DisplayDevice"` here — whose
//     `IsPresent`, `Percentage`, `State`, `TimeToEmpty`, `TimeToFull` and `WarningLevel` are the reading. On a
//     desktop that device is *not present*: `IsPresent` is `false`, `Percentage` is `0`, `State` is `0`. On a
//     laptop it is the pack.
//
// The D-Bus names are declared once in `BatteryStatus.h` and mirrored at compile time by `battery-test`, because
// a wrong D-Bus name cannot fail a build — it fails by drawing nothing, which is the one failure a readout
// cannot report to itself.
//
// **A reading is both halves or nothing.** `available` is false until the manager has described itself *and* the
// display device has answered, because a manager alone says nothing about a battery: the device is what a
// battery reading is about, and the daemon naming it is the first half of reading it. What follows from that is
// the two states the widget draws from — `!available` is a dash ("I have not heard"), and `available && !present`
// is *nothing at all* ("the daemon says there is no battery"), which is the honest answer on every desktop.
//
// **Nothing here blocks the GUI thread.** Every call to the daemon is asynchronous and every property arrives on
// the GUI thread through a queued signal, so there is no thread loop, no lock and no waiting: a reading is
// assembled from what has arrived and published as one value.
//
// **A reply that arrives late is dropped rather than applied.** The display device is re-resolved when the
// daemon says its device set moved, and the daemon can go away while those reads are in flight — which is exactly
// the case a `systemctl restart upower` creates. Each walk carries a generation, and a reply from an abandoned
// walk is discarded: without that, a reply from the daemon that went away would be applied to the state of the
// daemon that replaced it, and the bar would show the battery of a machine that had already changed.
#pragma once

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QDBusServiceWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <array>
#include <functional>
#include <memory>

#include "dbus/BatteryStatus.h"

namespace quantum::dbus {

class BatteryService : public QObject {
    Q_OBJECT

public:
    explicit BatteryService(QObject* parent = nullptr);
    ~BatteryService() override;

    // Whether the shell has a reading at all: false before UPower has answered, and while the daemon is not on
    // the bus. It is not the same question as `present()`, and the two together are what the widget draws from —
    // `!available` is a dash, while `available && !present` is a machine with no battery, which is a complete
    // reading and is drawn as nothing.
    Q_PROPERTY(bool available READ available NOTIFY readingChanged)
    // Whether the daemon's display device says it is there. False on a desktop and on a machine whose battery the
    // daemon cannot see; the readout draws nothing in that case rather than a dash, because a dash would claim a
    // battery whose level could not be read and this is the daemon saying there is no battery to read.
    Q_PROPERTY(bool present READ present NOTIFY readingChanged)
    // Whether the machine is running on its battery rather than on mains, from the manager's own `OnBattery`.
    // Published even when there is no battery to draw, because it is the manager's answer and it is the one fact
    // this readout has on a machine the daemon can find no battery on.
    Q_PROPERTY(bool onBattery READ onBattery NOTIFY readingChanged)
    // The charge level, 0-100, and whether there is one. Two properties rather than a sentinel because a level of
    // zero is a real reading — a battery with nothing left — and `-1` as "no level" invites a widget to draw an
    // empty battery that is not there. False while the daemon has published no level, or one outside 0-100.
    Q_PROPERTY(bool hasPercentage READ hasPercentage NOTIFY readingChanged)
    Q_PROPERTY(int percentage READ percentage NOTIFY readingChanged)
    // What the device is doing, as a token: `unknown`, `charging`, `discharging`, `fully-charged`, `empty`,
    // `pending-charge` or `pending-discharge` — UPower's own spellings, which is what the widget switches on.
    Q_PROPERTY(QString state READ state NOTIFY readingChanged)
    // The daemon's own warning level as a token: `unknown`, `none`, `discharging`, `low`, `critical` or `action`.
    // Published because the level is the daemon's *judgement* about the battery and not a number this readout
    // could derive: which of the six it reports depends on the policy and the thresholds in its own configuration
    // files, so a bar that coloured by percentage would be inventing a rule the daemon already has one for.
    Q_PROPERTY(QString warning READ warning NOTIFY readingChanged)
    // The time the daemon says the charge has left to move, as text — `2:15` — and whether there is one. The
    // text rather than a number of seconds because the *unit* is a rendering decision that has an edge case
    // (`0:00` is not the same statement as "under a minute") and it belongs where it can be tested as a function
    // of a number, which is `formatDuration` in the pure half. Empty while there is no time to draw.
    Q_PROPERTY(bool hasTimeRemaining READ hasTimeRemaining NOTIFY readingChanged)
    Q_PROPERTY(QString timeRemaining READ timeRemaining NOTIFY readingChanged)

    bool available() const { return available_; }
    bool present() const { return reading_.present; }
    bool onBattery() const { return reading_.onBattery; }
    bool hasPercentage() const { return reading_.percentage.has_value(); }
    // Zero while there is no percentage, the same convention `PipeWireService` and `NetworkService` use for a
    // number they do not have: the flag above says whether it means anything, and the widget reads the flag.
    int percentage() const { return reading_.percentage.value_or(0); }
    QString state() const { return batteryStateToken(reading_.state); }
    QString warning() const { return batteryWarningToken(reading_.warning); }
    bool hasTimeRemaining() const { return reading_.timeRemaining.has_value(); }
    QString timeRemaining() const
    {
        return reading_.timeRemaining ? formatDuration(*reading_.timeRemaining) : QString();
    }

    // Begins following UPower on `connection`. Called once from the composition root. A daemon that is not
    // running is not a failure: the shell keeps running, the readout draws its empty state, and the service
    // attaches when the daemon appears — which is what makes an upowerd restarted under a running shell recover
    // on its own.
    //
    // `connection` and `serviceName` are parameters for the reason `NetworkService`'s are and `SysMonService`
    // takes the directory it reads from: a test can name a bus of its own, which is how `battery-test` drives
    // every signal this module handles without a daemon and without the session's bus. `serviceName` is the
    // second half of the same: a test owns the name on its own bus, so nothing it does can reach the UPower the
    // desktop is using.
    void start(const QDBusConnection& connection, const QString& serviceName = UPowerService.toString());

    // Stops following the daemon and withdraws the reading: every subscription is removed, the reading becomes
    // unavailable, and replies still in flight are abandoned rather than applied. The destructor calls it, so a
    // service that is going away leaves no match rule, no signal connection and no watcher behind.
    void stop();

    // Re-resolves the display device and reads it again because a caller asked, which is not the same as a
    // schedule and is deliberately not one: nothing in this module calls it, and the only thing that does is a
    // test proving the read path against a daemon it has just changed behind the shell's back. The name says what
    // it costs — the gesture a future control centre will call is not this.
    void refreshNow();

    // Registers `service` so a QML file can bind to it:
    //
    //     import QuantumShell 1.0
    //     Text { text: BatteryService.available ? BatteryService.percentage + "%" : "—" }
    //
    // The instance stays owned by C++; QML only reads it. The module URI and version come from `QmlModule.h`,
    // the one place they are written down.
    static void registerQmlSingleton(BatteryService& service);

    // The type name above, declared once, for the reason the other services' are: a QML file is written against
    // it, so a rename that is not a rename everywhere fails at load time rather than at build time.
    // `battery_test.cpp` mirrors it and compares its copy at compile time.
    inline static constexpr auto QmlTypeName = "BatteryService";

signals:
    // One signal for the whole reading, which is what a readout needs and what keeps the published fields from
    // being read half-updated: the service compares the reading it just composed with the one before it and emits
    // only when something actually differs, so a `PropertiesChanged` that announced a property this readout does
    // not draw costs no repaint.
    void readingChanged();

private:
    // Which subscription a connection belongs to. Named rather than addressed by index so that re-pointing one
    // rule at another object is a statement about that object — the alternative, dropping every rule and re-adding
    // them, would stop listening to the manager for as long as the device took to answer, which is how a cable
    // being plugged in gets missed.
    enum class Subscription {
        Manager,
        Device,
        Count,
    };
    static constexpr int kSubscriptionCount = static_cast<int>(Subscription::Count);

    // One object of the reading: its properties as last read, and the path that reaches it. Members rather than
    // locals because a `PropertiesChanged` carries only what moved — the reading is recomposed from the merged
    // state, which is what makes the signal path cost no calls to the daemon at all.
    struct Object {
        ObjectProperties properties;
        QString path;
        // The walk whose read of this object is in flight, which is what tells a reply apart from a *stale*
        // reply to the same object. It matters because the object is told a read is in flight before the call is
        // issued, and a change that arrives before the reply is held rather than merged: if a reply that has been
        // abandoned by a newer walk simply returned, the object would be left believing a read is still on its
        // way, and every later change would be held behind a reply that will never come — a readout that stops
        // moving, which is the quieter half of the race the held buffer exists for. So an abandoned reply aborts
        // the read *it* owns, and only that one: a newer read for the same object has already replaced this
        // number, and its held changes must survive.
        int readGeneration = 0;
    };

    // Attaches to the daemon: the watcher first, then one read of the manager, whose reply says whether there is
    // a device to ask for. Idempotent in the sense that matters: a second attach after a withdraw re-reads
    // everything.
    void attach();
    // Reads the manager, then asks it for the display device and reads that. `generation` is the walk a reply
    // belongs to; every callback drops a reply from a walk that has been abandoned since — which is what keeps a
    // daemon that went away from writing into the state of the daemon that replaced it.
    void walkFromManager(int generation);
    // The device half of the walk, entered from the manager's read and again when the daemon says its device set
    // moved. It asks for the path every time rather than remembering it, because the question `GetDisplayDevice`
    // answers is the one the signal just changed the answer to.
    void walkDisplayDevice(int generation);
    // Reads one object in full and calls `then` when its reply has been adopted. The object's rule is put in place
    // *before* the read is issued, and the object is told a read is in flight before that: a change that arrives
    // between the two must be held rather than merged into an object with no other properties yet, which the
    // reply would then wipe. Getting that order wrong is a level that goes stale for as long as the reply takes.
    void read(Subscription object, Object& target, const QString& path, const QString& interface, int generation,
              std::function<void()> then);
    // Asks the daemon for the method that names the display device. It is the one call in this module that is not
    // a property read, and its reply is a path rather than a property value.
    void askForDisplayDevice(int generation, std::function<void(const QString&)> named,
                            std::function<void()> failed);
    // Asks one object for all its properties, and hands the reply — or the refusal — to `applied`. `interface` is
    // the D-Bus interface whose properties are wanted; the object path is the object.
    void getAll(const QString& path, const QString& interface,
                std::function<void(const QVariantMap&)> applied, std::function<void()> failed);
    // Points one object's rule at `path`, dropping whichever rule that object had before. The handler is chosen
    // by the object rather than passed in, so a rule cannot be added for one object and removed for another — a
    // D-Bus match is identified by its handler as well as its path, and a mismatch would leave a rule alive and
    // the service merged with a signal it believes it has stopped following.
    bool subscribe(Subscription object, const QString& path);
    void unsubscribe(Subscription object);
    void unsubscribeAll();
    // The handler a subscription's changes are delivered to, named once per object and used by both halves of the
    // rule above: Qt's D-Bus signal API identifies a rule by the receiver's slot as well as by the path, so
    // adding and removing it are the same string and a second spelling is a rule that never comes off.
    const char* ruleSlot(Subscription object) const;
    // Recomposes the reading from the two objects and publishes it if it differs from the last one, so a signal
    // about a property this readout does not draw costs no repaint.
    void publish();

private Q_SLOTS:
    // The hand-off from a D-Bus signal to the walk. Each of these merges one `PropertiesChanged` payload and then
    // decides whether it moved the reading — the manager's device set changing is the one move that requires
    // another question, because the object on the other side of it has not been named yet. A change that moves
    // nothing costs no call to the daemon.
    //
    // They are slots rather than ordinary members because that is what a D-Bus match is addressed by: Qt's signal
    // API finds the receiver's method in its meta-object, so a handler absent from it is a rule that cannot be
    // installed at all. The first argument is the interface whose properties changed — the signal carries it and
    // the match rule does not require it — which is why each one starts by checking it.
    void onManagerProperties(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);
    void onDeviceProperties(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);
    // The daemon's two device-set signals. Both mean the same thing to this module — ask again which object
    // represents the battery — so both are delivered here. They carry the path of the device that came or went,
    // which this module does not need: what matters is that the set moved, not which member moved.
    void onDevicesChanged(const QDBusObjectPath& device);

private:
    // Withdraws the reading and every subscription, leaving the service attached to nothing. Called when the
    // daemon leaves the bus, and by `stop()`.
    void withdraw();

    // The bus this service follows the daemon on. `QDBusConnection` has no default constructor — every one of
    // them names a bus — so the value held before `start()` is the one Qt gives for a name no connection was ever
    // made under: not connected, and every call on it fails rather than reaching somewhere unintended. `start()`
    // replaces it with the caller's, and `stop()` puts it back.
    QDBusConnection connection_{QString()};
    QString serviceName_;
    std::unique_ptr<QDBusServiceWatcher> watcher_;
    // The path of the rule currently installed for each object, empty meaning none: what `unsubscribe` needs in
    // order to remove exactly the rule that was added.
    std::array<QString, kSubscriptionCount> matches_;
    Object managerObject_;
    Object deviceObject_;
    BatteryReading reading_;
    // Whether this daemon's walk has reached an answer about the display device — it was read, or the daemon
    // named none. It is not cleared when a re-resolution starts, so a device set that moved keeps the last
    // reading on screen until the daemon answers about the new one rather than flashing the empty state; it is
    // cleared only when the daemon itself leaves, where nothing it said is part of what the next one says.
    bool deviceConcluded_ = false;
    // Bumped by every walk, so replies from an abandoned one can be recognised and dropped. Its first value is
    // deliberately not zero: a reply cannot be mistaken for "no walk" by accident.
    int generation_ = 1;
    bool readingSet_ = false;
    bool available_ = false;
    bool started_ = false;
};

}  // namespace quantum::dbus
