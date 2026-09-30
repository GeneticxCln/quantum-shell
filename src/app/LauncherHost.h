#pragma once

#include "app/PanelHost.h"

namespace quantum::apps {
class LauncherService;
}

namespace quantum::app {

// The launcher's surface: one window, on the primary output, that exists while the service says the launcher is
// open. All of what that means is `PanelHost`'s; this says which service holds the state.
class LauncherHost : public PanelHost
{
    Q_OBJECT

public:
    // Both must outlive this object. The surface is created against `engine` from `launcherUrl`;
    // a `backdropUrl` gives it the transparent surface behind it that a click outside the panel closes it through.
    explicit LauncherHost(quantum::apps::LauncherService& service, QQmlEngine& engine, const QUrl& launcherUrl,
        const QUrl& backdropUrl = {}, QObject* parent = nullptr);
};

}  // namespace quantum::app
