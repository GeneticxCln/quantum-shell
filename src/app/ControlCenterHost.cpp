#include "app/ControlCenterHost.h"

#include "app/ControlCenterService.h"

namespace quantum::app {

ControlCenterHost::ControlCenterHost(ControlCenterService& service, QQmlEngine& engine, const QUrl& panelUrl,
    const QUrl& backdropUrl, QObject* parent)
    : PanelHost(
          {[&service] { return service.isOpen(); },
           [&service](bool open) { service.setOpen(open); }},
          engine, panelUrl, backdropUrl, QStringLiteral("the control centre"), parent)
{
    connect(&service, &ControlCenterService::openChanged, this,
            &ControlCenterHost::handleOpenChanged);
}

}  // namespace quantum::app
