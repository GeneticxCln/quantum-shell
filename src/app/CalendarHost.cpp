#include "app/CalendarHost.h"

#include "app/CalendarService.h"

namespace quantum::app {

CalendarHost::CalendarHost(CalendarService& service, QQmlEngine& engine, const QUrl& panelUrl,
    const QUrl& backdropUrl, QObject* parent)
    : PanelHost(
          {[&service] { return service.isOpen(); },
           [&service](bool open) { service.setOpen(open); }},
          engine, panelUrl, backdropUrl, QStringLiteral("the calendar"), parent)
{
    connect(&service, &CalendarService::openChanged, this, &CalendarHost::handleOpenChanged);
}

}  // namespace quantum::app
