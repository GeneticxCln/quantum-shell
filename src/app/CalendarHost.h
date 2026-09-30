#pragma once

#include "app/PanelHost.h"

namespace quantum::app {

class CalendarService;

// The calendar's surface, on the primary output while the service says it is open. All of what that means is
// `PanelHost`'s; this says which service holds the state.
class CalendarHost : public PanelHost
{
    Q_OBJECT

public:
    // Both must outlive this object. The surface is created against `engine` from `panelUrl`;
    // a `backdropUrl` gives it the transparent surface behind it that a click outside the panel closes it through.
    explicit CalendarHost(CalendarService& service, QQmlEngine& engine, const QUrl& panelUrl,
        const QUrl& backdropUrl = {}, QObject* parent = nullptr);
};

}  // namespace quantum::app
