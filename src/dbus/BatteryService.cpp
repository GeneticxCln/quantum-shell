#include "dbus/BatteryService.h"

#include "QmlModule.h"
#include "app/Logging.h"

#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QQmlEngine>

namespace quantum::dbus {
namespace {

// The interface a `PropertiesChanged` signal is declared on and the method that reads every property of an
// object in one round trip. Both are the standard D-Bus ones rather than UPower's, which is why they are not in
// `BatteryStatus.h` beside the daemon's own names — nothing about them is UPower's to decide.
constexpr QLatin1StringView PropertiesInterfaceName{"org.freedesktop.DBus.Properties"};
constexpr QLatin1StringView PropertiesChangedName{"PropertiesChanged"};
constexpr QLatin1StringView GetAllMethod{"GetAll"};

}  // namespace

BatteryService::BatteryService(QObject* parent) : QObject(parent) {}

BatteryService::~BatteryService()
{
    stop();
}

void BatteryService::start(const QDBusConnection& connection, const QString& serviceName)
{
    stop();

    connection_ = connection;
    serviceName_ = serviceName;
    started_ = true;
    generation_ = 1;

    if (!connection_.isConnected()) {
        // No bus at all, which is a machine with no system bus running rather than a UPower that is stopped: the
        // readout stays empty and the reason is written down. There is nothing to retry — a bus connection is
        // made once, and a shell started without one was started before the bus it needs.
        qCWarning(quantum::app::batteryLog) << "cannot follow" << serviceName_
                                            << ": the bus connection is not connected:"
                                            << connection_.lastError().message();
        return;
    }

    watcher_ = std::make_unique<QDBusServiceWatcher>(
        serviceName_, connection_,
        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration);
    // The two events that make a daemon restart survive: it left, so the reading is withdrawn; it arrived, so the
    // walk is read again from the manager. Both are the daemon's own signal rather than anything this service
    // polls for.
    QObject::connect(watcher_.get(), &QDBusServiceWatcher::serviceRegistered, this, [this] { attach(); });
    QObject::connect(watcher_.get(), &QDBusServiceWatcher::serviceUnregistered, this, [this] { withdraw(); });

    // The daemon's own two signals for its device set moving. They are ordinary signal connections rather than
    // `PropertiesChanged` match rules — a different D-Bus mechanism, used here because they are different signals:
    // UPower declares them on the manager interface and they carry no properties. Their payload is the path of the
    // device that came or went, which this service deliberately ignores, and both are delivered to one handler
    // because both mean the same thing to it.
    connection_.connect(serviceName_, UPowerManagerPath.toString(), UPowerManagerInterface.toString(), UPowerSignalDeviceAdded,
                        this, SLOT(onDevicesChanged(QDBusObjectPath)));
    connection_.connect(serviceName_, UPowerManagerPath.toString(), UPowerManagerInterface.toString(), UPowerSignalDeviceRemoved,
                        this, SLOT(onDevicesChanged(QDBusObjectPath)));

    // A daemon that was already running when the shell started produces no registration signal, so the service
    // asks the bus whether the name is owned — and asks it *asynchronously*, because this is the GUI thread: a
    // blocking call here would freeze the bar for as long as the bus took to answer, on a machine where the bus
    // is exactly the thing that is not responding.
    QDBusConnectionInterface* bus = connection_.interface();
    if (!bus) {
        qCWarning(quantum::app::batteryLog) << "cannot follow" << serviceName_
                                            << ": the bus has no interface object";
        return;
    }
    auto* pending = new QDBusPendingCallWatcher(bus->asyncCall(QStringLiteral("NameHasOwner"), serviceName_), this);
    QObject::connect(pending, &QDBusPendingCallWatcher::finished, this, [this, pending] {
        pending->deleteLater();
        if (!started_) {
            // `stop()` ran while the bus was answering, so this reply describes a question nobody is asking any
            // more. Reading the daemon after a stop would be a connection the shell believes it closed.
            return;
        }
        const QDBusPendingReply<bool> reply = *pending;
        if (reply.isError()) {
            qCWarning(quantum::app::batteryLog) << "cannot tell whether" << serviceName_
                                                << "is running:" << reply.error().message();
            return;
        }
        if (reply.value()) {
            attach();
        }
    });

    // One record per start, naming the unique name this process was given on the bus. It is what makes "this
    // shell asks the daemon nothing" a claim that can be measured from outside the process: a bus monitor shows
    // every method call a sender makes, and the sender here is a name the shell's own record names.
    qCInfo(quantum::app::batteryLog) << "following" << serviceName_ << "as" << connection_.baseService();
}

void BatteryService::stop()
{
    started_ = false;
    watcher_.reset();
    // Withdrawing also abandons every read still in flight, which is the half that matters: a reply that arrives
    // after the shell stopped following a daemon must not become a reading.
    withdraw();
    connection_ = QDBusConnection(QString());
}

void BatteryService::refreshNow()
{
    if (!started_ || !connection_.isConnected()) {
        return;
    }
    // A fresh walk from the manager, which is the same thing `attach()` does minus its record: the point of this
    // is to re-read a daemon that is still there, not to announce a second attachment to it. The generation bump
    // is what makes it safe to call while a walk is in flight.
    walkFromManager(++generation_);
}

void BatteryService::attach()
{
    qCInfo(quantum::app::batteryLog) << "attached to" << serviceName_ << "as" << connection_.baseService();
    walkFromManager(++generation_);
}

void BatteryService::walkFromManager(int generation)
{
    read(Subscription::Manager, managerObject_, UPowerManagerPath.toString(), UPowerManagerInterface.toString(), generation,
         [this, generation] { walkDisplayDevice(generation); });
}

void BatteryService::walkDisplayDevice(int generation)
{
    // The device the reading was taken from is no longer necessarily the device this daemon names, so its rule
    // goes before the new one is added: a rule left behind would merge a signal from an object nothing draws into
    // the state of the object that replaced it. The manager's own rule stays — it is what said the set moved, and
    // it is what says the next move.
    unsubscribe(Subscription::Device);
    deviceObject_ = Object();

    askForDisplayDevice(
        generation,
        [this, generation](const QString& path) {
            if (!isObjectPath(path)) {
                // The daemon named no device, or named the null path. That is not a failure to read — it is the
                // daemon saying there is nothing to report on — so the reading is published as the manager's half
                // alone, which is exactly the machine-with-no-battery case on a desktop whose UPower has no
                // devices at all.
                // The daemon has answered about the device, and the answer is that there is none. That is a
                // concluded reading and not a missing one, which is the difference this flag carries: a machine
                // with no battery draws nothing, where a device that could not be read draws a dash.
                deviceConcluded_ = true;
                if (generation == generation_) {
                    publish();
                }
                return;
            }
            read(Subscription::Device, deviceObject_, path, UPowerDeviceInterface.toString(), generation,
                 [this, generation] {
                     deviceConcluded_ = true;
                     if (generation == generation_) {
                         publish();
                     }
                 });
        },
        [this, generation] {
            // The daemon could not name a device at all. The reading is published from what the walk does have,
            // which is the manager's half, and the reason is in the record `askForDisplayDevice` wrote — so a
            // machine whose battery cannot be read draws a dash rather than claiming there is none.
            if (generation == generation_) {
                publish();
            }
        });
}

void BatteryService::read(Subscription object, Object& target, const QString& path, const QString& interface,
                          int generation, std::function<void()> then)
{
    subscribe(object, path);
    target.path = path;
    target.readGeneration = generation;
    target.properties.beginRead();
    getAll(path, interface,
           [this, generation, &target, path, then = std::move(then)](const QVariantMap& all) {
               // Two guards, and both are needed. The generation says whether the *walk* this read belongs to is
               // still the one in progress: when the daemon has named a different device, a reply for the old one
               // must not be patched in beside it. The path says whether this object is still the one in the
               // reading: a device set that moved twice while a reply was in flight passes the first guard and
               // has to fail this one, or the reading would end up describing a battery the daemon has stopped
               // reporting on.
               if (generation != generation_ || target.path != path) {
                   return;
               }
               target.properties.adoptAll(all);
               then();
           },
           [&target, generation, this] {
               // The object refused to describe itself, or the daemon is gone. The walk ends here rather than
               // carrying on to an object it cannot name, and the reading is published from what the walk does
               // have — so a device that cannot be read leaves the manager's half standing, which is what a bar
               // should show when one part of the reading is missing.
               //
               // The abort is guarded the same way the reply's is, and for the same reason: a refusal for a read
               // this object is no longer waiting on belongs to an older walk, and clearing the flag for it would
               // end the read a newer walk has in flight.
               if (target.readGeneration == generation) {
                   target.properties.abortRead();
               }
               if (generation == generation_) {
                   publish();
               }
           });
}

void BatteryService::askForDisplayDevice(int generation, std::function<void(const QString&)> named,
                                         std::function<void()> failed)
{
    if (!connection_.isConnected()) {
        qCWarning(quantum::app::batteryLog) << "cannot ask" << serviceName_ << "for the display device:"
                                            << "the bus connection is not connected";
        failed();
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(serviceName_, UPowerManagerPath.toString(),
                                                       UPowerManagerInterface.toString(), UPowerMethodGetDisplayDevice);
    auto* pending = new QDBusPendingCallWatcher(connection_.asyncCall(call), this);
    QObject::connect(pending, &QDBusPendingCallWatcher::finished, this,
                     [this, pending, generation, named = std::move(named), failed = std::move(failed)] {
                         pending->deleteLater();
                         // A reply from a walk that has been abandoned is dropped here rather than at the call
                         // site, so that no reply can reach a callback belonging to a daemon that has gone: the
                         // callbacks themselves are written to assume the daemon is answering.
                         if (generation != generation_) {
                             return;
                         }
                         const QDBusPendingReply<QDBusObjectPath> reply = *pending;
                         if (reply.isError()) {
                             qCWarning(quantum::app::batteryLog)
                                 << "cannot ask" << serviceName_ << "for the display device:" << reply.error().message();
                             failed();
                             return;
                         }
                         named(reply.value().path());
                     });
}

void BatteryService::getAll(const QString& path, const QString& interface,
                            std::function<void(const QVariantMap&)> applied, std::function<void()> failed)
{
    // Note what this function does *not* do: it does not decide whether a reply is still wanted. The walk a reply
    // belongs to is known to the callbacks that receive it, and only there — which is why the check lives in them
    // rather than in this watcher. Dropping the reply here instead left a hole: the object had already been told a
    // read was in flight and any change that overtook it was being held behind that read, so a reply that returned
    // without telling it anything left it waiting for ever, and every later change was held behind a reply that
    // would never come. A read the object was told about always ends now, on every path.
    if (!connection_.isConnected()) {
        // The bus this service was started on is gone or was never reachable. There is nothing to ask and nothing
        // to retry — a bus connection is made once — so this is a refusal with a reason rather than a call that
        // fails somewhere further from the cause.
        qCWarning(quantum::app::batteryLog) << "cannot read" << interface << "of" << path
                                            << ": the bus connection is not connected";
        failed();
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(serviceName_, path, PropertiesInterfaceName, GetAllMethod);
    call << interface;

    auto* pending = new QDBusPendingCallWatcher(connection_.asyncCall(call), this);
    QObject::connect(pending, &QDBusPendingCallWatcher::finished, this,
                     [this, pending, path, interface, applied = std::move(applied),
                      failed = std::move(failed)] {
                         pending->deleteLater();
                         const QDBusPendingReply<QVariantMap> reply = *pending;
                         if (reply.isError()) {
                             // The device the daemon named cannot be read. That is a reading this service does not
                             // have: the part already read stays as it was and the reason is written down, rather
                             // than the whole reading being thrown away for one object that refused. The record is
                             // written even for a reply from an abandoned walk, because the device could not be
                             // read whatever the shell is doing now.
                             qCWarning(quantum::app::batteryLog)
                                 << "cannot read" << interface << "of" << path << ":" << reply.error().message();
                             failed();
                             return;
                         }
                         applied(reply.value());
                     });
}

bool BatteryService::subscribe(Subscription object, const QString& path)
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
    // Every rule is `PropertiesChanged` on one object path, with the handler that object's changes belong to. The
    // interface in the rule is the standard `org.freedesktop.DBus.Properties` rather than the object's own,
    // because that is where the signal is declared; each handler checks the interface named in the payload's first
    // argument.
    const char* slot = ruleSlot(object);
    if (slot == nullptr
        || !connection_.connect(serviceName_, path, PropertiesInterfaceName, PropertiesChangedName, this, slot)) {
        qCWarning(quantum::app::batteryLog) << "cannot follow" << path << ":" << connection_.lastError().message();
        return false;
    }
    matches_[index] = path;
    return true;
}

void BatteryService::unsubscribe(Subscription object)
{
    const int index = static_cast<int>(object);
    if (matches_[index].isEmpty()) {
        return;
    }
    // Removed with the handler it was added with, which is `ruleSlot`'s whole point: a D-Bus rule is identified by
    // its receiver method as well as its path, so a second spelling here would leave it installed and the service
    // still merged with a signal it believes it has stopped following.
    const char* slot = ruleSlot(object);
    if (slot != nullptr) {
        connection_.disconnect(serviceName_, matches_[index], PropertiesInterfaceName, PropertiesChangedName, this,
                               slot);
    }
    matches_[index].clear();
}

void BatteryService::unsubscribeAll()
{
    for (int index = 0; index < kSubscriptionCount; ++index) {
        unsubscribe(static_cast<Subscription>(index));
    }
    // The two device-set signals are connections rather than match rules, so they are removed by name here. Both
    // are dropped with the same description they were made with, for the same reason the match rules are.
    connection_.disconnect(serviceName_, UPowerManagerPath.toString(), UPowerManagerInterface.toString(), UPowerSignalDeviceAdded,
                           this, SLOT(onDevicesChanged(QDBusObjectPath)));
    connection_.disconnect(serviceName_, UPowerManagerPath.toString(), UPowerManagerInterface.toString(), UPowerSignalDeviceRemoved,
                           this, SLOT(onDevicesChanged(QDBusObjectPath)));
}

const char* BatteryService::ruleSlot(Subscription object) const
{
    // One name per object, and the same name is what `subscribe` installs and what `unsubscribe` removes. The slot
    // strings are `SLOT()`'s own spelling of each handler's signature, which is the spelling Qt's D-Bus signal API
    // addresses a receiver method by.
    switch (object) {
    case Subscription::Manager:
        return SLOT(onManagerProperties(QString, QVariantMap, QStringList));
    case Subscription::Device:
        return SLOT(onDeviceProperties(QString, QVariantMap, QStringList));
    case Subscription::Count:
        break;
    }
    return nullptr;
}

void BatteryService::onManagerProperties(const QString& interface, const QVariantMap& changed,
                                         const QStringList& invalidated)
{
    if (interface != QLatin1String(UPowerManagerInterface)) {
        return;
    }
    managerObject_.properties.applyChange(changed, invalidated);
    // `OnBattery` is the manager's half of the reading and it moves on its own — a machine that unplugs itself
    // announces it here. No call to the daemon: the new value was in the signal.
    publish();
}

void BatteryService::onDeviceProperties(const QString& interface, const QVariantMap& changed,
                                        const QStringList& invalidated)
{
    if (interface != QLatin1String(UPowerDeviceInterface)) {
        return;
    }
    deviceObject_.properties.applyChange(changed, invalidated);
    publish();
}

void BatteryService::onDevicesChanged(const QDBusObjectPath& device)
{
    // The daemon's device set moved, which is the one event that can change *which object* represents the
    // battery — a pack swapped into a bay, a UPS arriving, a wireless mouse that stopped being reported. The
    // signal carries the device that came or went and this module does not use it: the question it has to ask is
    // not about that device but about which one the daemon now calls the display device, and only the daemon can
    // answer it. So the walk is re-entered at the device, under a new generation so that a reply for the old one
    // is dropped.
    Q_UNUSED(device);
    walkDisplayDevice(++generation_);
}

void BatteryService::publish()
{
    const BatteryReading next = composeReading(managerObject_.properties, deviceObject_.properties);
    // The manager's `OnBattery` present is what says the manager described itself; `deviceConcluded_` is what says
    // the walk reached an answer about the device — it was read, or the daemon named none. Both are required, and
    // what a half-reading would mean is the reason: a manager alone has no battery to draw a level for, so a
    // reading published from it would be `available` with `present` false — the word for "this machine has no
    // battery" said about a machine whose battery has simply not answered yet. That is the one claim this module
    // must not make, and this is the line that stops it.
    const bool managerAnswered = managerObject_.properties.boolValue(UPowerPropertyOnBattery).has_value();
    const bool available = managerAnswered && deviceConcluded_;
    const bool changed = !readingSet_ || available != available_ || !(next == reading_);
    reading_ = next;
    readingSet_ = true;
    available_ = available;
    if (changed) {
        emit readingChanged();
    }
}

void BatteryService::withdraw()
{
    // Bumped first: replies still in flight belong to the daemon that is gone, and a late one applied after this
    // point would resurrect a reading nothing is maintaining.
    ++generation_;
    unsubscribeAll();
    managerObject_.properties.clear();
    deviceObject_.properties.clear();
    managerObject_ = Object();
    deviceObject_ = Object();

    const bool had = available_ || readingSet_;
    available_ = false;
    readingSet_ = false;
    deviceConcluded_ = false;
    reading_ = BatteryReading();
    if (had) {
        emit readingChanged();
    }
}

void BatteryService::registerQmlSingleton(BatteryService& service)
{
    qmlRegisterSingletonInstance(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion,
                                 quantum::qml::ModuleMinorVersion, QmlTypeName, &service);
}

}  // namespace quantum::dbus
