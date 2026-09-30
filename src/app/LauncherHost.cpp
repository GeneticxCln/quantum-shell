#include "app/LauncherHost.h"

#include "apps/LauncherService.h"

namespace quantum::app {

LauncherHost::LauncherHost(quantum::apps::LauncherService& service, QQmlEngine& engine, const QUrl& launcherUrl,
    const QUrl& backdropUrl, QObject* parent)
    : PanelHost(
          {[&service] { return service.isOpen(); },
           [&service](bool open) { service.setOpen(open); }},
          engine, launcherUrl, backdropUrl, QStringLiteral("the launcher"), parent)
{
    connect(&service, &quantum::apps::LauncherService::openChanged, this,
            &LauncherHost::handleOpenChanged);
}

}  // namespace quantum::app
