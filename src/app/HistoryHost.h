#pragma once

#include "app/PanelHost.h"

namespace quantum::dbus {
class NotificationService;
}

namespace quantum::app {

// The notification history panel: a surface a person asks for rather than one a notification causes, which is what
// distinguishes it from `ToastHost`. The state is the service's `notificationHistoryOpen`; the rest is `PanelHost`'s.
class HistoryHost : public PanelHost
{
    Q_OBJECT

public:
    // Both must outlive this object. The surface is created against `engine` from `historyUrl`;
    // a `backdropUrl` gives it the transparent surface behind it that a click outside the panel closes it through.
    explicit HistoryHost(quantum::dbus::NotificationService& service, QQmlEngine& engine, const QUrl& historyUrl,
        const QUrl& backdropUrl = {}, QObject* parent = nullptr);
};

}  // namespace quantum::app
