// The network readout's connection to NetworkManager: the connection in use, the device it runs on and that
// device's signal quality, exposed to QML, kept current by events rather than by asking.
//
// This module exists because NetworkManager publishes `PropertiesChanged` on every object it owns — the
// manager, each active connection, each device, each access point — so a network that changes state, roams or
// loses signal announces it, and there is nothing for the shell to poll. That is the same shape the audio
// module has and the opposite of `src/system/`, and it is why `src/dbus/` takes no waiver: nothing here asks
// the daemon anything on a schedule. What the service asks for is asked *once per object*, when a signal tells
// it the chain has moved to an object it has not read, and everything after that is a signal.
//
// What it reads, and where each name was verified rather than recalled — every one of them read off this
// session with `busctl introspect --xml-interface`, the daemon's own description of what it serves:
//
//   * the manager object on the well-known name `org.freedesktop.NetworkManager`, whose `State` (32-bit,
//     70 here), `Connectivity` (4 here) and `PrimaryConnection` (`/org/freedesktop/NetworkManager/
//     ActiveConnection/2` here) are the global half of the reading;
//   * the active connection's `Id` — `"Victor Salin 5 GHz"` here, the SSID for wifi — and its `Type`
//     (`"802-11-wireless"`), beside its `Devices` list, which is how the device is reached;
//   * the device's `Interface` (`"wlan0"` here), `DeviceType` (2, wifi) and `ActiveConnection`;
//   * the wireless half of that same object path, where `ActiveAccessPoint` names
//     `/org/freedesktop/NetworkManager/AccessPoint/1`, whose `Strength` is the byte 74 on this session.
//
// The D-Bus names are declared once in `NetworkStatus.h` and mirrored at compile time by `network-test`,
// because a wrong D-Bus name cannot fail a build — it fails by drawing nothing, which is the one failure a
// readout cannot report to itself.
//
// **A reading is the whole chain or nothing.** A device the daemon has published without an access point has
// no strength rather than a zero, and a machine with no primary connection has a state and no name — so a
// disconnected bar says `offline` and does not draw a device that is not carrying anything.
//
// **Nothing here blocks the GUI thread.** Every call to the daemon is asynchronous and every property arrives
// on the GUI thread through a queued signal, so there is no thread loop, no lock and no waiting: a reading is
// assembled from what has arrived and published as one value.
//
// **A reply that arrives late is dropped rather than applied.** The chain is re-read when a signal says it
// moved, and the daemon can go away while those reads are in flight — which is exactly the case a restart
// creates. Each walk carries a generation, and a reply from an abandoned walk is discarded: without that, a
// reply from the daemon that went away would be applied to the state of the daemon that replaced it, and the
// bar would show a connection that had already ended.
#pragma once

#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QObject>
#include <QString>
#include <QVector>

#include <array>
#include <functional>
#include <memory>

#include "dbus/NetworkStatus.h"

namespace quantum::dbus {

class NetworkService : public QObject {
    Q_OBJECT

public:
    explicit NetworkService(QObject* parent = nullptr);
    ~NetworkService() override;

    // Whether the shell has a reading at all: false before NetworkManager has answered, and while the daemon is
    // not on the bus. It is not the same question as `state()`, and the two together are what the widget draws
    // from — `!available` is a dash, while `state() == "disconnected"` is a machine that is deliberately
    // offline and is drawn as exactly that, because a daemon that says "nothing is connected" has answered.
    Q_PROPERTY(bool available READ available NOTIFY readingChanged)
    // The daemon's own state as a token — `disconnected`, `connecting` or `connected` — which is what the
    // widget switches on, and the only spelling of it that crosses into QML.
    Q_PROPERTY(QString state READ state NOTIFY readingChanged)
    // The active connection's name: the SSID for wifi, the profile's name for anything else. Empty when there
    // is no primary connection, and empty is drawn as nothing rather than as a stand-in — a name the daemon
    // has not published is a name this readout does not have.
    Q_PROPERTY(QString connectionName READ connectionName NOTIFY readingChanged)
    // The device's interface name, `wlan0` or `enp39s0`. Published because it is the one fact that tells two
    // connections of the same kind apart on a machine that has more than one, and because a test can compare it
    // against the daemon's own answer without depending on a name a person chose.
    Q_PROPERTY(QString interfaceName READ interfaceName NOTIFY readingChanged)
    // What the device is, as a token: `wifi`, `ethernet` or `other`. Wifi is the one that has a strength.
    Q_PROPERTY(QString deviceKind READ deviceKind NOTIFY readingChanged)
    // The signal quality, 0-100, and whether there is one. Two properties rather than a sentinel because a
    // strength of zero is a real reading — a device on the edge of range — and `-1` as "no strength" invites a
    // widget to draw a signal that does not exist. Only wifi has a strength; anything else has `false` here.
    Q_PROPERTY(bool hasStrength READ hasStrength NOTIFY readingChanged)
    Q_PROPERTY(int strength READ strength NOTIFY readingChanged)
    // The daemon's verdict on whether the network works, as a token: `none`, `portal`, `limited` or `full`.
    // Empty while the daemon has not said — which is not `full`, and the widget draws the same nothing for both,
    // because a marker's job is to flag trouble and "I have not heard" is not trouble.
    Q_PROPERTY(QString connectivity READ connectivity NOTIFY readingChanged)

    bool available() const { return available_; }
    QString state() const { return connectionStateToken(reading_.state); }
    QString connectionName() const { return reading_.name; }
    QString interfaceName() const { return reading_.interfaceName; }
    QString deviceKind() const { return deviceKindToken(reading_.kind); }
    bool hasStrength() const { return reading_.strength.has_value(); }
    // Zero while there is no strength, the same convention `PipeWireService` uses for a number it does not have:
    // the flag above says whether it means anything, and the widget is written to read the flag.
    int strength() const { return reading_.strength.value_or(0); }
    QString connectivity() const { return reading_.connectivity ? connectivityToken(*reading_.connectivity) : QString(); }

    // Begins following NetworkManager on `connection`. Called once from the composition root. A daemon that is
    // not running is not a failure: the shell keeps running, the readout draws its empty state, and the service
    // attaches when the daemon appears — which is what makes a NetworkManager restarted under a running shell
    // recover on its own.
    //
    // `connection` and `serviceName` are parameters for the reason `SysMonService` takes the directory it reads
    // from and `PipeWireService` takes the remote it attaches to: a test can name a bus of its own, which is how
    // `network-test` drives every signal this module handles without a daemon and without the session's bus.
    // `serviceName` is the second half of the same: a test owns the name on its own bus, so nothing it does can
    // reach the NetworkManager the desktop is using.
    void start(const QDBusConnection& connection, const QString& serviceName = NetworkManagerService.toString());

    // Stops following the daemon and withdraws the reading: every subscription is removed, the reading becomes
    // unavailable, and replies still in flight are abandoned rather than applied. The destructor calls it, so a
    // service that is going away leaves no match rule and no watcher behind.
    void stop();

    // Re-reads the chain because a caller asked, which is not the same as a schedule and is deliberately not
    // one: nothing in this module calls it, and the only thing that does is a test proving the read path against
    // a daemon it has just changed behind the shell's back. The name says what it costs — the gesture a future
    // control centre will call is not this.
    void refreshNow();

    // Registers `service` so a QML file can bind to it:
    //
    //     import QuantumShell 1.0
    //     Text { text: NetworkService.available ? NetworkService.state : "—" }
    //
    // The instance stays owned by C++; QML only reads it. The module URI and version come from `QmlModule.h`,
    // the one place they are written down.
    static void registerQmlSingleton(NetworkService& service);

    // The type name above, declared once, for the reason `PipeWireService`'s is: a QML file is written against
    // it, so a rename that is not a rename everywhere fails at load time rather than at build time.
    // `network_test.cpp` mirrors it and compares its copy at compile time.
    inline static constexpr auto QmlTypeName = "NetworkService";

signals:
    // One signal for the whole reading, which is what a readout needs and what keeps the published fields from
    // being read half-updated: the service compares the reading it just composed with the one before it and
    // emits only when something actually differs, so a `PropertiesChanged` that announced a property this
    // readout does not draw costs no repaint.
    void readingChanged();

private:
    // One object of the chain: its properties as last read, the path that reaches it, and the match rule that
    // keeps it current. They are members rather than locals because a `PropertiesChanged` carries only what
    // moved — the reading is recomposed from the merged state, which is what makes the signal path cost no calls
    // to the daemon at all.
    struct Object {
        ObjectProperties properties;
        QString path;
        // The walk whose read of this object is in flight, which is what tells a reply apart from a *stale*
        // reply to the same object. It matters because the object is told a read is in flight before the call is
        // issued, and a change that arrives before the reply is held rather than merged: if a reply that has been
        // abandoned by a newer walk simply returned, the object would be left believing a read is still on its
        // way, and every later change would be held behind a reply that will never come — a readout that stops
        // moving, which is the quieter half of the race `ObjectProperties` exists for. So an abandoned reply
        // aborts the read *it* owns, and only that one: a newer read of the same object has already replaced this
        // number, and its held changes must survive it.
        int readGeneration = 0;
    };

    // Which object a subscription belongs to. Named rather than addressed by index so that replacing one
    // object's rule is a statement about that object — the alternative, dropping every rule and re-adding them,
    // would stop listening to the manager for as long as an access point took to answer, which is how a state
    // change gets missed.
    enum class Subscription {
        Manager,
        Connection,
        Device,
        Wireless,
        AccessPoint,
        Count,
    };
    static constexpr int kSubscriptionCount = static_cast<int>(Subscription::Count);

    // Attaches to the daemon: the watcher first, then one read of the manager, whose reply starts the chain
    // walk. Idempotent in the sense that matters: a second attach after a withdraw re-reads everything.
    void attach();
    // Walks the chain, one object at a time, because each step's *reply* names the next object: the manager
    // names the active connection, that connection names the device, the device's wireless half names the
    // access point. `generation` is the walk a reply belongs to; every callback drops a reply from a walk that
    // has been abandoned since — which is what keeps a daemon that went away from writing into the state of the
    // daemon that replaced it.
    void walkFromManager(int generation);
    void beginChainWalk(int generation);
    // The two points a signal can re-enter the walk at, which is why they are methods rather than continuations
    // written inside the read that precedes them: a device whose active access point changed, and a wireless
    // device whose access point list did, are both "carry on from here" rather than "start again", and a chain
    // restarted from the manager on every roam would be a readout that flickers through an offline state.
    void afterDevice(int generation);
    void afterWireless(int generation);
    // Reads one object in full and calls `then` when its reply has been adopted. The object's rule is put in
    // place *before* the read is issued, and the object is told a read is in flight before that: a change that
    // arrives between the two must be held rather than merged into an object with no other properties yet, which
    // the reply would then wipe. Getting that order wrong is a strength that goes stale for as long as the reply
    // takes, and on a busy radio that is the common case rather than a rare one.
    void read(Subscription object, Object& target, const QString& path, const QString& interface, int generation,
              std::function<void()> then);
    // The end of a walk: the subscriptions are made to describe the objects now in the reading, and the reading
    // is published. One place rather than one per branch, because a branch that forgot to subscribe would leave
    // an object followed by nothing — a strength that stops moving while the connection stays up.
    void finishWalk(int generation);
    // Asks one object for all its properties, and hands the reply — or the refusal — to `applied`. `interface` is
    // the D-Bus interface whose properties are wanted; the object path is the object.
    void getAll(const QString& path, const QString& interface,
                std::function<void(const QVariantMap&)> applied, std::function<void()> failed);
    // Points one object's match rule at `path`, dropping whichever rule that object had before. The handler is
    // chosen by the object rather than passed in, so a rule cannot be added for one object and removed for
    // another — a D-Bus match is identified by its handler as well as its path, and a mismatch would leave a rule
    // alive and the service merged with a signal it believes it has stopped following.
    bool subscribe(Subscription object, const QString& path);
    void unsubscribe(Subscription object);
    void unsubscribeAll();
    // The handler a subscription's changes are delivered to, named once per object and used by both halves of
    // the rule above: Qt's D-Bus signal API identifies a rule by the receiver's slot as well as by the path, so
    // adding and removing it are the same string and a second spelling is a rule that never comes off.
    const char* ruleSlot(Subscription object) const;
    // Recomposes the reading from the chain and publishes it if it differs from the last one, so a signal about a
    // property this readout does not draw costs no repaint.
    void publish();

private Q_SLOTS:
    // The hand-off from a D-Bus signal to the chain. Each of these merges one `PropertiesChanged` payload and
    // then decides whether it moved the chain — the manager naming a different primary connection, or a device
    // naming a different active access point — in which case the objects on the other side of that move have to
    // be read, because nothing has described them yet. A change that moves nothing costs no call to the daemon.
    //
    // They are slots rather than ordinary members because that is what a D-Bus match is addressed by: Qt's
    // signal API finds the receiver's method in its meta-object, so a handler absent from it is a rule that
    // cannot be installed at all. The first argument is the interface whose properties changed — the signal
    // carries it and the match rule does not require it — which is why each one starts by checking it: two
    // interfaces can change on one object path, and a wireless device announces its own properties and its
    // wireless half's from the same path.
    void onManagerProperties(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);
    void onConnectionProperties(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);
    void onDeviceProperties(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);
    void onWirelessProperties(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);
    void onAccessPointProperties(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);

private:
    // Withdraws the reading and every subscription, leaving the service attached to nothing. Called when the
    // daemon leaves the bus, and by `stop()`.
    void withdraw();

    // The bus this service follows the daemon on. `QDBusConnection` has no default constructor — every one of
    // them names a bus — so the value held before `start()` is the one Qt gives for a name no connection was
    // ever made under: not connected, and every call on it fails rather than reaching somewhere unintended.
    // `start()` replaces it with the caller's, and `stop()` puts it back.
    QDBusConnection connection_{QString()};
    QString serviceName_;
    std::unique_ptr<QDBusServiceWatcher> watcher_;
    // The path of the rule currently installed for each object, empty meaning none: what `unsubscribe` needs in
    // order to remove exactly the rule that was added.
    std::array<QString, kSubscriptionCount> matches_;
    Object managerObject_;
    Object activeConnection_;
    Object deviceObject_;
    Object wirelessObject_;
    Object accessPointObject_;
    NetworkReading reading_;
    // Bumped by every walk, so replies from an abandoned one can be recognised and dropped. Its first value is
    // deliberately not zero: a reply cannot be mistaken for "no walk" by accident.
    int generation_ = 1;
    bool readingSet_ = false;
    bool available_ = false;
    bool started_ = false;
};

}  // namespace quantum::dbus
