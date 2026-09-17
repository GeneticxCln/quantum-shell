#include "dbus/NetworkService.h"

#include "QmlModule.h"
#include "app/Logging.h"

#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QQmlEngine>

namespace quantum::dbus {
namespace {

// The object path that means "no object" on the bus, and the test for a path this service may ask about, are
// both in `dbus/DbusProperties.h`: the battery module follows a daemon the same way and must treat an absent
// reference identically, and a second copy of this check is a second place for it to drift.
constexpr QLatin1StringView PropertiesInterfaceName{"org.freedesktop.DBus.Properties"};
constexpr QLatin1StringView PropertiesChangedName{"PropertiesChanged"};

// The method that reads every property of an object in one round trip, and the interface whose properties are
// wanted. `GetAll` is what makes a chain walk cost one call per object rather than one per property.
constexpr QLatin1StringView GetAllMethod{"GetAll"};

}  // namespace

NetworkService::NetworkService(QObject* parent) : QObject(parent) {}

NetworkService::~NetworkService()
{
    stop();
}

void NetworkService::start(const QDBusConnection& connection, const QString& serviceName)
{
    stop();

    connection_ = connection;
    serviceName_ = serviceName;
    started_ = true;
    generation_ = 1;

    if (!connection_.isConnected()) {
        // No bus at all, which is a machine with no system bus running rather than a NetworkManager that is
        // stopped: the readout stays empty and the reason is written down. There is nothing to retry — a bus
        // connection is made once, and a shell started without one was started before the bus it needs.
        qCWarning(quantum::app::networkLog) << "cannot follow" << serviceName_ << ": the bus connection is not connected:"
                              << connection_.lastError().message();
        return;
    }

    watcher_ = std::make_unique<QDBusServiceWatcher>(
        serviceName_, connection_,
        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration);
    // The two events that make a daemon restart survive: it left, so the reading is withdrawn; it arrived, so the
    // chain is read again from the manager. Both are the daemon's own signal rather than anything this service
    // polls for.
    QObject::connect(watcher_.get(), &QDBusServiceWatcher::serviceRegistered, this, [this] { attach(); });
    QObject::connect(watcher_.get(), &QDBusServiceWatcher::serviceUnregistered, this, [this] { withdraw(); });

    // A daemon that was already running when the shell started produces no registration signal, so the service
    // asks the bus whether the name is owned — and asks it *asynchronously*, because this is the GUI thread:
    // a blocking call here would freeze the bar for as long as the bus took to answer, on a machine where the
    // bus is exactly the thing that is not responding.
    QDBusConnectionInterface* bus = connection_.interface();
    if (!bus) {
        qCWarning(quantum::app::networkLog) << "cannot follow" << serviceName_ << ": the bus has no interface object";
        return;
    }
    auto* pending = new QDBusPendingCallWatcher(bus->asyncCall(QStringLiteral("NameHasOwner"), serviceName_), this);
    QObject::connect(pending, &QDBusPendingCallWatcher::finished, this, [this, pending] {
        pending->deleteLater();
        if (!started_) {
            // `stop()` ran while the bus was answering, so this reply describes a question nobody is asking
            // any more. Reading the daemon after a stop would be a connection the shell believes it closed.
            return;
        }
        const QDBusPendingReply<bool> reply = *pending;
        if (reply.isError()) {
            qCWarning(quantum::app::networkLog) << "cannot tell whether" << serviceName_ << "is running:" << reply.error().message();
            return;
        }
        if (reply.value()) {
            attach();
        }
    });

    // One record per start, naming the unique name this process was given on the bus. It is what makes "this
    // shell asks the daemon nothing" a claim that can be measured from outside the process: a bus monitor shows
    // every method call a sender makes, and the sender here is a name the shell's own record names.
    qCInfo(quantum::app::networkLog) << "following" << serviceName_ << "as" << connection_.baseService();
}

void NetworkService::stop()
{
    started_ = false;
    watcher_.reset();
    // Withdrawing also abandons every read still in flight, which is the half that matters: a reply that arrives
    // after the shell stopped following a daemon must not become a reading.
    withdraw();
    connection_ = QDBusConnection(QString());
}

void NetworkService::refreshNow()
{
    if (!started_ || !connection_.isConnected()) {
        return;
    }
    // A fresh walk from the manager, which is the same thing `attach()` does minus its record: the point of this
    // is to re-read a daemon that is still there, not to announce a second attachment to it. The generation
    // bump is what makes it safe to call while a walk is in flight.
    walkFromManager(++generation_);
}

void NetworkService::attach()
{
    qCInfo(quantum::app::networkLog) << "attached to" << serviceName_ << "as" << connection_.baseService();
    walkFromManager(++generation_);
}

void NetworkService::walkFromManager(int generation)
{
    read(Subscription::Manager, managerObject_, ManagerPath.toString(), ManagerInterface.toString(),
         generation, [this, generation] { beginChainWalk(generation); });
}

void NetworkService::beginChainWalk(int generation)
{
    // The objects of the chain that was in the reading are no longer part of it, so their rules go before the new
    // ones are added: a rule left behind would merge a signal from an object nothing draws into the state of the
    // object that replaced it. The manager's own rule stays — it is what said the chain moved, and it is what
    // says the next move.
    unsubscribe(Subscription::Connection);
    unsubscribe(Subscription::Device);
    unsubscribe(Subscription::Wireless);
    unsubscribe(Subscription::AccessPoint);
    activeConnection_ = Object();
    deviceObject_ = Object();
    wirelessObject_ = Object();
    accessPointObject_ = Object();

    const auto primary = managerObject_.properties.pathValue(PropertyPrimaryConnection);
    if (!isObjectPath(primary)) {
        // No primary connection: the manager's own state is the whole reading, and it is published as such —
        // `offline` with nothing beside it, which is what a machine with nothing connected is worth.
        finishWalk(generation);
        return;
    }

    read(Subscription::Connection, activeConnection_, *primary, ActiveConnectionInterface.toString(),
         generation,
         [this, generation] {
             // The device: the connection's own `Devices` list. A connection may name more than one — a bond
             // names its members, a bridge its ports — and the readout draws the connection rather than each of
             // its parts, so the first is the one it is reachable through. The alternative, drawing every device
             // of a bond, is a readout this bar has no room for and a person no use for.
             const QStringList devices = activeConnection_.properties.pathList(PropertyDevices);
             if (devices.isEmpty() || !isObjectPath(devices.first())) {
                 finishWalk(generation);
                 return;
             }
             read(Subscription::Device, deviceObject_, devices.first(), DeviceInterface.toString(), generation,
                  [this, generation] { afterDevice(generation); });
         });
}

void NetworkService::afterDevice(int generation)
{
    // What a device is decides what else can be read from it: the signal quality lives on the wireless half of a
    // wireless device's own object path, and asking a wired device for that interface would be asking the daemon
    // for something that object does not have — a refusal per connection, which is exactly the kind of noise that
    // makes a log useless.
    const quint32 deviceType = deviceObject_.properties.uint32Value(PropertyDeviceType).value_or(0);
    if (deviceKindFromDeviceType(deviceType) != DeviceKind::Wifi) {
        unsubscribe(Subscription::Wireless);
        unsubscribe(Subscription::AccessPoint);
        wirelessObject_ = Object();
        accessPointObject_ = Object();
        finishWalk(generation);
        return;
    }

    read(Subscription::Wireless, wirelessObject_, deviceObject_.path, WirelessInterface.toString(), generation,
         [this, generation] { afterWireless(generation); });
}

void NetworkService::afterWireless(int generation)
{
    // The access point the device is on, as the wireless interface names it. A wireless device that is not
    // associated names `/`, which is a device with no signal rather than a signal of zero.
    const auto accessPoint = wirelessObject_.properties.pathValue(PropertyActiveAccessPoint);
    if (!isObjectPath(accessPoint)) {
        unsubscribe(Subscription::AccessPoint);
        accessPointObject_ = Object();
        finishWalk(generation);
        return;
    }

    read(Subscription::AccessPoint, accessPointObject_, *accessPoint, AccessPointInterface.toString(), generation,
         [this, generation] { finishWalk(generation); });
}

void NetworkService::finishWalk(int generation)
{
    if (generation != generation_) {
        return;
    }
    publish();
}

void NetworkService::read(Subscription object, Object& target, const QString& path, const QString& interface,
                          int generation, std::function<void()> then)
{
    subscribe(object, path);
    target.path = path;
    target.readGeneration = generation;
    target.properties.beginRead();
    getAll(path, interface,
           [this, generation, &target, path, then = std::move(then)](const QVariantMap& all) {
               // Two guards, and both are needed. The generation says whether the *walk* this read belongs to is
               // still the one in progress: when the chain has moved, the objects to read are different objects,
               // and a reply for the old ones must not be patched in beside them. The path says whether this
               // object is still the one in the reading: a roam that names a second access point while the first
               // one's reply is still in flight passes the first guard and has to fail this one, or the reading
               // would end up describing a BSS the device has already left.
               if (generation != generation_ || target.path != path) {
                   // The read this reply belonged to is over either way, and the object has to be told so: it was
                   // marked as reading and any change that overtook this reply was held behind it. Aborting only
                   // the read *this* reply owns leaves a newer read of the same object — its generation has
                   // already replaced this number — with the changes that are held for it.
                   if (target.readGeneration == generation) {
                       target.properties.abortRead();
                   }
                   return;
               }
               target.properties.adoptAll(all);
               then();
           },
           [&target, generation, this] {
               // The object refused to describe itself, or the daemon is gone. The walk ends here rather than
               // carrying on to objects it cannot name, and the reading is published from what the chain does
               // have — so an object that cannot be read leaves the rest of the reading standing, which is what
               // a bar should show when one part of the chain is missing.
               //
               // The abort is guarded the same way the reply's is, and for the same reason: a refusal for a read
               // this object is no longer waiting on belongs to an older walk, and clearing the flag for it
               // would end the read a newer walk has in flight.
               if (target.readGeneration == generation) {
                   target.properties.abortRead();
               }
               finishWalk(generation);
           });
}


void NetworkService::getAll(const QString& path, const QString& interface,
                            std::function<void(const QVariantMap&)> applied, std::function<void()> failed)
{
    // Note what this function does *not* do: it does not decide whether a reply is still wanted. The walk a
    // reply belongs to is known to the callbacks that receive it, and only there — which is why the check moved
    // out of this watcher and into them. Dropping the reply here instead left a hole: the object had already been
    // told a read was in flight and any change that overtook it was being held behind that read, so a reply that
    // returned without telling it anything left it waiting for ever, and every later change was held behind a
    // reply that would never come. A read the object was told about always ends now, on every path.
    if (!connection_.isConnected()) {
        // The bus this service was started on is gone or was never reachable. There is nothing to ask and
        // nothing to retry — a bus connection is made once — so this is a refusal with a reason rather than a
        // call that fails somewhere further from the cause.
        qCWarning(quantum::app::networkLog) << "cannot read" << interface << "of" << path
                                            << ": the bus connection is not connected";
        failed();
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(serviceName_, path,
                                                       PropertiesInterfaceName,
                                                       GetAllMethod);
    call << interface;

    auto* pending = new QDBusPendingCallWatcher(connection_.asyncCall(call), this);
    QObject::connect(pending, &QDBusPendingCallWatcher::finished, this,
                     [this, pending, path, interface, applied = std::move(applied),
                      failed = std::move(failed)] {
                         pending->deleteLater();
                         const QDBusPendingReply<QVariantMap> reply = *pending;
                         if (reply.isError()) {
                             // The object the daemon named cannot be read. That is a reading this service does
                             // not have: the parts already read stay as they were and the reason is written down,
                             // rather than the whole reading being thrown away for one object that refused.
                             //
                             // The record is written even for a reply from a walk that has been abandoned, and
                             // that is deliberate: the object could not be read, which is true of it whatever the
                             // shell is doing now, and a late refusal is a fact about the daemon rather than noise.
                             qCWarning(quantum::app::networkLog) << "cannot read" << interface << "of" << path << ":"
                                                   << reply.error().message();
                             failed();
                             return;
                         }
                         applied(reply.value());
                     });
}

bool NetworkService::subscribe(Subscription object, const QString& path)
{
    if (path.isEmpty()) {
        return false;
    }
    const int index = static_cast<int>(object);
    if (matches_[index] == path) {
        // Already following exactly this object with exactly this handler: re-adding the rule would leave the
        // first one in place and unmatched by `unsubscribe`, which is how a rule leaks.
        return true;
    }
    unsubscribe(object);
    // Every rule is `PropertiesChanged` on one object path, with the handler that object's changes belong to.
    // The interface in the rule is the standard `org.freedesktop.DBus.Properties` rather than the object's own,
    // because that is where the signal is declared; each handler checks the interface named in the payload's
    // first argument, since two interfaces can change on one path — a wireless device announces its own
    // properties and its wireless half's from the same object.
    const char* slot = ruleSlot(object);
    if (slot == nullptr
        || !connection_.connect(serviceName_, path, PropertiesInterfaceName,
                                PropertiesChangedName, this, slot)) {
        qCWarning(quantum::app::networkLog) << "cannot follow" << path << ":"
                                           << connection_.lastError().message();
        return false;
    }
    matches_[index] = path;
    return true;
}

void NetworkService::unsubscribe(Subscription object)
{
    const int index = static_cast<int>(object);
    if (matches_[index].isEmpty()) {
        return;
    }
    // Removed with the handler it was added with, which is `ruleSlot`'s whole point: a D-Bus rule is identified
    // by its receiver method as well as its path, so a second spelling here would leave it installed and the
    // service still merged with a signal it believes it has stopped following.
    const char* slot = ruleSlot(object);
    if (slot != nullptr) {
        connection_.disconnect(serviceName_, matches_[index], PropertiesInterfaceName,
                               PropertiesChangedName, this, slot);
    }
    matches_[index].clear();
}

void NetworkService::unsubscribeAll()
{
    for (int index = 0; index < kSubscriptionCount; ++index) {
        unsubscribe(static_cast<Subscription>(index));
    }
}

const char* NetworkService::ruleSlot(Subscription object) const
{
    // One name per object, and the same name is what `subscribe` installs and what `unsubscribe` removes. The
    // slot strings are `SLOT()`'s own spelling of each handler's signature, which is the spelling Qt's D-Bus
    // signal API addresses a receiver method by.
    switch (object) {
    case Subscription::Manager:
        return SLOT(onManagerProperties(QString, QVariantMap, QStringList));
    case Subscription::Connection:
        return SLOT(onConnectionProperties(QString, QVariantMap, QStringList));
    case Subscription::Device:
        return SLOT(onDeviceProperties(QString, QVariantMap, QStringList));
    case Subscription::Wireless:
        return SLOT(onWirelessProperties(QString, QVariantMap, QStringList));
    case Subscription::AccessPoint:
        return SLOT(onAccessPointProperties(QString, QVariantMap, QStringList));
    case Subscription::Count:
        break;
    }
    return nullptr;
}

void NetworkService::onManagerProperties(const QString& interface, const QVariantMap& changed,
                                         const QStringList& invalidated)
{
    if (interface != QLatin1String(ManagerInterface)) {
        return;
    }
    const auto before = managerObject_.properties.pathValue(PropertyPrimaryConnection);
    managerObject_.properties.applyChange(changed, invalidated);
    const auto after = managerObject_.properties.pathValue(PropertyPrimaryConnection);
    if (before != after) {
        // The chain moved. The objects on the other side of the move have described nothing about themselves yet,
        // so they are read — once each, and only because a signal named them. Note what did *not* happen: no
        // timer, no interval, and no re-reading of the objects that did not move.
        beginChainWalk(++generation_);
        return;
    }
    publish();
}

void NetworkService::onConnectionProperties(const QString& interface, const QVariantMap& changed,
                                            const QStringList& invalidated)
{
    if (interface != QLatin1String(ActiveConnectionInterface)) {
        return;
    }
    activeConnection_.properties.applyChange(changed, invalidated);
    // A connection's name can change while it is in use — NetworkManager renames the active connection when the
    // SSID is learned on some devices — and nothing else about it moves this readout, so this is a repaint and
    // not a walk.
    publish();
}

void NetworkService::onDeviceProperties(const QString& interface, const QVariantMap& changed,
                                        const QStringList& invalidated)
{
    if (interface != QLatin1String(DeviceInterface)) {
        return;
    }
    const auto previousType = deviceObject_.properties.uint32Value(PropertyDeviceType);
    const auto previousAccessPoint = deviceObject_.properties.pathValue(PropertyActiveAccessPoint);
    deviceObject_.properties.applyChange(changed, invalidated);
    const auto currentType = deviceObject_.properties.uint32Value(PropertyDeviceType);
    const auto currentAccessPoint = deviceObject_.properties.pathValue(PropertyActiveAccessPoint);

    if (previousType != currentType) {
        // What the device is has changed, which changes which interfaces it has: carrying on from the device
        // rather than through it is what makes a device that gained or lost its wireless half come out right, and
        // it reads the wireless half only when there is one to read.
        afterDevice(generation_);
        return;
    }
    if (previousAccessPoint != currentAccessPoint) {
        // The device moved to another access point. `afterWireless` is re-entered rather than the walk restarted
        // from the manager, so the readout never passes through an offline state on a roam.
        afterWireless(generation_);
        return;
    }
    publish();
}

void NetworkService::onWirelessProperties(const QString& interface, const QVariantMap& changed,
                                          const QStringList& invalidated)
{
    if (interface != QLatin1String(WirelessInterface)) {
        return;
    }
    const auto before = wirelessObject_.properties.pathValue(PropertyActiveAccessPoint);
    wirelessObject_.properties.applyChange(changed, invalidated);
    const auto after = wirelessObject_.properties.pathValue(PropertyActiveAccessPoint);
    if (before != after) {
        // This is the route a roam arrives by on the interface that owns the property; a device announces the
        // same move on its own interface, and both are followed because NetworkManager documents the property on
        // both. Whichever arrives first does the read, and the one that arrives second finds the access point
        // already current and publishes without a call.
        afterWireless(generation_);
        return;
    }
    publish();
}

void NetworkService::onAccessPointProperties(const QString& interface, const QVariantMap& changed,
                                             const QStringList& invalidated)
{
    if (interface != QLatin1String(AccessPointInterface)) {
        return;
    }
    accessPointObject_.properties.applyChange(changed, invalidated);
    // The strength, which is the property of an access point that moves while everything else stays still. No
    // call to the daemon: the new value was in the signal.
    publish();
}

void NetworkService::publish()
{
    const NetworkReading next = composeReading(managerObject_.properties, activeConnection_.properties,
                                               deviceObject_.properties, accessPointObject_.properties);
    // A reading is the daemon's own state, and the manager is the object the whole chain is reached through: it
    // is the one object a reading cannot be composed without. So `available` is whether the manager has described
    // itself, not whether this service has ever heard from anything — which is what makes a walk that failed at
    // the manager leave the readout a dash rather than draw a machine with no state and no name as `offline`,
    // a fact nothing told us.
    const bool available = managerObject_.properties.uint32Value(PropertyState).has_value();
    const bool changed = !readingSet_ || available != available_ || !(next == reading_);
    reading_ = next;
    readingSet_ = true;
    available_ = available;
    if (changed) {
        emit readingChanged();
    }
}

void NetworkService::withdraw()
{
    // Bumped first: replies still in flight belong to the daemon that is gone, and a late one applied after this
    // point would resurrect a reading nothing is maintaining.
    ++generation_;
    unsubscribeAll();
    managerObject_.properties.clear();
    activeConnection_.properties.clear();
    deviceObject_.properties.clear();
    wirelessObject_.properties.clear();
    accessPointObject_.properties.clear();
    managerObject_ = Object();
    activeConnection_ = Object();
    deviceObject_ = Object();
    wirelessObject_ = Object();
    accessPointObject_ = Object();

    const bool had = available_ || readingSet_;
    available_ = false;
    readingSet_ = false;
    reading_ = NetworkReading();
    if (had) {
        emit readingChanged();
    }
}

void NetworkService::registerQmlSingleton(NetworkService& service)
{
    qmlRegisterSingletonInstance(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion,
                                 quantum::qml::ModuleMinorVersion, QmlTypeName, &service);
}

}  // namespace quantum::dbus
