#pragma once

#include <QObject>

namespace quantum::apps {
class LauncherService;
}
namespace quantum::dbus {
class NotificationService;
}

namespace quantum::app {

class CalendarService;
class ControlCenterService;

// The panels that take the screen's attention — the launcher, the control centre and the notification history —
// are one at a time.
//
// Each has a state of its own (`open`) because each is toggled from its own place: an IPC verb, a click on the bar.
// Nothing made them exclusive, so opening a second while the first was up left two panels and two backdrops on the
// screen, the upper backdrop taking the click that was meant to dismiss the lower one. Opening one now closes the
// others, through their own services, so each host tears its surface down the way it does for any close. It is a
// rule about the set and so it lives here, in the composition root's hands, rather than in three services that
// would have to know about each other.
class PanelGroup : public QObject
{
    Q_OBJECT

public:
    // All three must outlive this object.
    PanelGroup(quantum::apps::LauncherService& launcher, ControlCenterService& controlCenter,
               quantum::dbus::NotificationService& notifications, CalendarService& calendar,
               QObject* parent = nullptr);

private:
    quantum::apps::LauncherService* launcher_ = nullptr;
    ControlCenterService* controlCenter_ = nullptr;
    quantum::dbus::NotificationService* notifications_ = nullptr;
    CalendarService* calendar_ = nullptr;
};

}  // namespace quantum::app
