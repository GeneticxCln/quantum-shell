#include "app/HistoryHost.h"

#include "dbus/NotificationService.h"

namespace quantum::app {

HistoryHost::HistoryHost(quantum::dbus::NotificationService& service, QQmlEngine& engine, const QUrl& historyUrl,
    const QUrl& backdropUrl, QObject* parent)
    : PanelHost(
          {[&service] { return service.notificationHistoryOpen(); },
           [&service](bool open) { service.setNotificationHistoryOpen(open); }},
          engine, historyUrl, backdropUrl, QStringLiteral("the notification history"), parent)
{
    connect(&service, &quantum::dbus::NotificationService::notificationHistoryOpenChanged, this,
            &HistoryHost::handleOpenChanged);
}

}  // namespace quantum::app
