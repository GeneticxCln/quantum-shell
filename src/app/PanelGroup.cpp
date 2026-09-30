#include "app/PanelGroup.h"

#include "app/CalendarService.h"
#include "app/ControlCenterService.h"
#include "apps/LauncherService.h"
#include "dbus/NotificationService.h"

namespace quantum::app {

PanelGroup::PanelGroup(quantum::apps::LauncherService& launcher, ControlCenterService& controlCenter,
                       quantum::dbus::NotificationService& notifications, CalendarService& calendar,
                       QObject* parent)
    : QObject(parent)
    , launcher_(&launcher)
    , controlCenter_(&controlCenter)
    , notifications_(&notifications)
    , calendar_(&calendar)
{
    // Only the transition to open closes the others: a panel closing must not close anything, or the panel a
    // person just opened would be closed by the one it replaced announcing its own close.
    connect(launcher_, &quantum::apps::LauncherService::openChanged, this, [this] {
        if (!launcher_->isOpen())
            return;
        controlCenter_->setOpen(false);
        notifications_->setNotificationHistoryOpen(false);
        calendar_->setOpen(false);
    });
    connect(controlCenter_, &ControlCenterService::openChanged, this, [this] {
        if (!controlCenter_->isOpen())
            return;
        launcher_->setOpen(false);
        notifications_->setNotificationHistoryOpen(false);
        calendar_->setOpen(false);
    });
    connect(notifications_, &quantum::dbus::NotificationService::notificationHistoryOpenChanged, this, [this] {
        if (!notifications_->notificationHistoryOpen())
            return;
        launcher_->setOpen(false);
        controlCenter_->setOpen(false);
        calendar_->setOpen(false);
    });
    connect(calendar_, &CalendarService::openChanged, this, [this] {
        if (!calendar_->isOpen())
            return;
        launcher_->setOpen(false);
        controlCenter_->setOpen(false);
        notifications_->setNotificationHistoryOpen(false);
    });
}

}  // namespace quantum::app
