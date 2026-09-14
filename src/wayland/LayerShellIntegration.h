#pragma once

#include <QtWaylandClient/private/qwaylandshellintegration_p.h>

#include "wayland/LayerShellProtocol.h"

namespace QuantumShell {

// Route 1 of QUANTUM_SHELL.md § Layer-Shell Implementation Strategy: the shell's own protocol
// bindings, attached to the wl_surface Qt's Wayland platform plugin created. Qt has no public
// layer-shell client API, so the role is set here and Qt keeps ownership of the surface, its
// buffer and its rendering.
//
// QWaylandShellIntegrationTemplate is used the way Qt intends — with the integration itself as its
// template argument. It binds the registry global by casting itself to T and calling T::init, so T has
// to be the type that inherits it; naming the generated protocol class there instead does not compile,
// because the cast is made from inside the template and a base of a derived class is no help. The
// generated class is inherited here alongside it, which is where interface() and init() come from.
class LayerShellIntegration
    : public QtWaylandClient::QWaylandShellIntegrationTemplate<LayerShellIntegration>,
      public QtWayland::zwlr_layer_shell_v1
{
public:
    LayerShellIntegration();

    QtWaylandClient::QWaylandShellSurface *createShellSurface(QtWaylandClient::QWaylandWindow *window) override;
};

} // namespace QuantumShell
