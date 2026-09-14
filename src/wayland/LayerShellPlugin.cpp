#include "wayland/LayerShellPlugin.h"

#include "wayland/LayerShellIntegration.h"

namespace QuantumShell {

QtWaylandClient::QWaylandShellIntegration *LayerShellPlugin::create(const QString &key, const QStringList &paramList)
{
    Q_UNUSED(paramList)

    if (key != QLatin1String("quantum-shell"))
        return nullptr;

    return new LayerShellIntegration;
}

} // namespace QuantumShell
