#pragma once

#include <QtWaylandClient/private/qwaylandshellintegrationplugin_p.h>

namespace QuantumShell {

// The shell is selected by setting QT_WAYLAND_SHELL_INTEGRATION=quantum-shell, which is how Qt
// looks up a shell integration by key. Keys are read from the metadata file below.
class LayerShellPlugin : public QtWaylandClient::QWaylandShellIntegrationPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QWaylandShellIntegrationFactoryInterface_iid FILE "quantum-shell-layershell.json")

public:
    QtWaylandClient::QWaylandShellIntegration *create(const QString &key, const QStringList &paramList) override;
};

} // namespace QuantumShell
