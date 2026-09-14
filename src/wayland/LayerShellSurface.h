#pragma once

#include <QtWaylandClient/private/qwaylandshellsurface_p.h>

#include "wayland/LayerShellProtocol.h"

#include <QSize>
#include <QString>

namespace QuantumShell {

class LayerShellWindow;

// The zwlr_layer_surface_v1 role for one window.
//
// Lifecycle the protocol demands, and what this class does about it: get_layer_surface assigns the
// role, every property is set while the surface still has no buffer, the client commits once with
// nothing attached, and only after the compositor's configure is acknowledged may a buffer be
// attached. Qt attaches the buffer when it first paints, so the surface asks Qt to wait by keeping
// the window unmapped until configure arrives.
class LayerShellSurface : public QtWaylandClient::QWaylandShellSurface,
                          public QtWayland::zwlr_layer_surface_v1
{
    Q_OBJECT

public:
    LayerShellSurface(QtWaylandClient::QWaylandWindow *window,
                      QtWayland::zwlr_layer_shell_v1 *shell,
                      LayerShellWindow *layerWindow);
    ~LayerShellSurface() override;

    bool isExposed() const override;
    void applyConfigure() override;

    // Whether the compositor has answered the initial commit. Until it has, the surface has no size
    // and no buffer may be attached to it.
    bool isConfigured() const { return mConfigured; }

protected:
    void zwlr_layer_surface_v1_configure(uint32_t serial, uint32_t width, uint32_t height) override;
    void zwlr_layer_surface_v1_closed() override;

private:
    // Sends the current anchors, exclusive zone, margins, keyboard interactivity and layer.
    void sendConfiguration();
    // The size to propose to the compositor: zero on any axis the anchors make it stretch along.
    QSize proposedSize() const;
    void commit();

    QtWaylandClient::QWaylandWindow *mWaylandWindow = nullptr;
    LayerShellWindow *mLayerWindow = nullptr;
    // The namespace this surface's role was assigned with. A layer surface may be given a role once, so
    // this is what the compositor has the surface listed under, whatever a later edit to the window says.
    QString mNamespace;
    QSize mPendingSize;
    // False until the compositor's initial configure has been acknowledged. Qt paints, and therefore
    // attaches a buffer, only while the window reports itself exposed — so this is what holds Qt back
    // for the one round trip the protocol requires, rather than a race against the render loop.
    bool mConfigured = false;
};

} // namespace QuantumShell
