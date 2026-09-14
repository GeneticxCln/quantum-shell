#include "wayland/LayerShellIntegration.h"

#include "app/Logging.h"
#include "wayland/LayerShellSurface.h"
#include "wayland/LayerShellWindow.h"

#include <QtWaylandClient/private/qwaylandwindow_p.h>

#include <QDebug>

namespace QuantumShell {

namespace {

// AGENTS.md freezes the shell's layer-shell namespaces as quantum-shell-*. The check lives here,
// next to the request that carries the string, because this is the only place a namespace enters
// the protocol: a foreign one would be a surface the shell cannot claim back from the compositor.
constexpr QLatin1StringView nsPrefix("quantum-shell-");

} // namespace

LayerShellIntegration::LayerShellIntegration()
    : QWaylandShellIntegrationTemplate<LayerShellIntegration>(
          QtWayland::zwlr_layer_shell_v1::interface()->version)
{
}

QtWaylandClient::QWaylandShellSurface *LayerShellIntegration::createShellSurface(QtWaylandClient::QWaylandWindow *window)
{
    auto *layerWindow = qobject_cast<LayerShellWindow *>(window->window());
    if (layerWindow == nullptr) {
        // Quantum Shell is a shell: the windows it creates are bar, dock, OSD and notification
        // surfaces, all of which are layer surfaces. A window that is not a LayerShellWindow has no
        // role this integration can give it, and silently mapping it as a bar would be worse than
        // saying so.
        qCWarning(quantum::app::waylandLog)
            << QStringLiteral("LayerShellIntegration: %1 is not a LayerShellWindow, so it gets no layer "
                              "surface")
                   .arg(window->window()->title());
        return nullptr;
    }

    const QString layerNamespace = layerWindow->layerNamespace();
    if (!layerNamespace.startsWith(nsPrefix)) {
        qCWarning(quantum::app::waylandLog)
            << QStringLiteral("LayerShellIntegration: refusing layer surface with namespace \"%1\"; every "
                              "Quantum Shell namespace begins with %2")
                   .arg(layerNamespace, QString::fromLatin1(nsPrefix));
        return nullptr;
    }

    return new LayerShellSurface(window, this, layerWindow);
}

} // namespace QuantumShell
