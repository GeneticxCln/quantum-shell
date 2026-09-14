#include "wayland/LayerShellSurface.h"

#include "app/Logging.h"
#include "wayland/LayerShellWindow.h"

#include <QtWaylandClient/private/qwaylandwindow_p.h>

#include <QDebug>
#include <QScreen>

#include <wayland-client.h>

namespace QuantumShell {

LayerShellSurface::LayerShellSurface(QtWaylandClient::QWaylandWindow *window,
                                     QtWayland::zwlr_layer_shell_v1 *shell,
                                     LayerShellWindow *layerWindow)
    : QtWaylandClient::QWaylandShellSurface(window)
    , mWaylandWindow(window)
    , mLayerWindow(layerWindow)
{
    struct ::wl_surface *surface = window->wlSurface();
    if (surface == nullptr) {
        qCWarning(quantum::app::waylandLog)
            << "LayerShellSurface: the window has no wl_surface; not assigning the layer role";
        return;
    }

    // Recorded, not just sent: the protocol assigns a surface's role once, so this string is the name
    // the compositor will list the surface under for its whole life, and the only way to change it is to
    // create a new surface. Keeping it is what lets the handler below say so instead of letting a
    // configured namespace differ from the one niri has.
    mNamespace = mLayerWindow->layerNamespace();

    init(shell->get_layer_surface(surface,
                                  nullptr,
                                  static_cast<uint32_t>(mLayerWindow->layer()),
                                  mNamespace));

    sendConfiguration();

    // The initial commit carries no buffer, which is what the protocol requires before the
    // compositor answers with configure. Qt attaches its first buffer only when it paints, and the
    // window stays unmapped until configure arrives.
    wl_surface_commit(surface);

    QObject::connect(mLayerWindow, &LayerShellWindow::configurationChanged, this, [this] {
        if (!isInitialized())
            return;

        // The namespace cannot be re-sent: it is an argument to get_layer_surface, which may be called
        // once per surface. A configured change to it is therefore a change this surface cannot honour,
        // and saying that plainly is the difference between a user knowing a restart is needed and a
        // user believing the file they just edited had no effect.
        if (mLayerWindow->layerNamespace() != mNamespace) {
            mNamespace = mLayerWindow->layerNamespace();
            qCWarning(quantum::app::waylandLog)
                << QStringLiteral("LayerShellSurface: namespace changed to \"%1\"; a layer surface's "
                                  "namespace is fixed when its role is assigned, so it takes effect on the "
                                  "next start")
                       .arg(mNamespace);
        }

        sendConfiguration();
        commit();
    });
}

LayerShellSurface::~LayerShellSurface()
{
    if (isInitialized())
        destroy();
}

bool LayerShellSurface::isExposed() const
{
    // This is the whole handshake, and the reason it is here rather than in a flag of our own: Qt asks
    // the shell surface whether the window is exposed, and paints — attaching a buffer — only while the
    // answer is yes. Reporting exposed before the compositor's initial configure has been acknowledged
    // is exactly the protocol error niri answers with "must ack the initial configure before attaching
    // buffer". Reporting not-exposed holds Qt back for one round trip, and the resize that applyConfigure
    // performs once configure arrives is what starts the painting.
    return mConfigured;
}

namespace {

// The size to propose on one axis, which the protocol makes a narrow choice.
//
//   - On a stretched axis — one anchored to both of its opposite edges — zero is the only legal value,
//     because the compositor chooses that size and a client-chosen one is a protocol error.
//   - On any other axis the client must name a size, and zero is *not* a way of declining: niri answers
//     a zero width without the left and right anchors with
//     "width 0 requested without setting left and right anchors".
//
// Hence the last resort. The size the window declares is the answer whenever it has one, and it does not
// always: the role is assigned while the window is being mapped, and a QML width bound to the screen has
// not necessarily resolved by then. The screen's own size is a real reading of the output rather than a
// guess, and on the axis a surface with nothing declared would fall back to anyway.
int proposeAlongAxis(bool stretched, int windowSize, int screenSize) {
    if (stretched)
        return 0;
    return windowSize > 0 ? windowSize : qMax(0, screenSize);
}

}  // namespace

QSize LayerShellSurface::proposedSize() const
{
    const LayerShellWindow::Anchors anchors = mLayerWindow->anchors();
    const bool stretchesHorizontally =
        anchors.testFlag(LayerShellWindow::LeftEdge) && anchors.testFlag(LayerShellWindow::RightEdge);
    const bool stretchesVertically =
        anchors.testFlag(LayerShellWindow::TopEdge) && anchors.testFlag(LayerShellWindow::BottomEdge);

    const QScreen *screen = mLayerWindow->screen();
    const QSize screenSize = screen ? screen->geometry().size() : QSize();

    return QSize(proposeAlongAxis(stretchesHorizontally, mLayerWindow->width(), screenSize.width()),
                 proposeAlongAxis(stretchesVertically, mLayerWindow->height(), screenSize.height()));
}

void LayerShellSurface::sendConfiguration()
{
    if (!isInitialized())
        return;

    const QSize size = proposedSize();
    set_size(static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()));
    set_anchor(static_cast<uint32_t>(mLayerWindow->anchors().toInt()));
    set_exclusive_zone(mLayerWindow->exclusiveZone());

    const QMargins margins = mLayerWindow->margins();
    set_margin(margins.top(), margins.right(), margins.bottom(), margins.left());

    set_keyboard_interactivity(static_cast<uint32_t>(mLayerWindow->keyboardInteractivity()));

    // set_layer arrived in version 2, so a compositor that bound version 1 has no such request and
    // sending it would be a protocol error. The generated class exposes no version(), and the
    // surface is created at the version the shell object was bound at, so the proxy carries it.
    if (wl_proxy_get_version(reinterpret_cast<struct ::wl_proxy *>(object())) >= 2)
        set_layer(static_cast<uint32_t>(mLayerWindow->layer()));
}

void LayerShellSurface::commit()
{
    if (struct ::wl_surface *surface = mWaylandWindow->wlSurface())
        wl_surface_commit(surface);
}

void LayerShellSurface::zwlr_layer_surface_v1_configure(uint32_t serial, uint32_t width, uint32_t height)
{
    // Acked before anything else, and before the window is allowed to paint: the acknowledgement is the
    // event niri is waiting for, and a buffer that arrives before it is a protocol error.
    ack_configure(serial);

    // Zero on an axis is the compositor leaving the size to the client, so fall back to the size the
    // window asked for rather than collapsing to nothing.
    const int resolvedWidth = width > 0 ? static_cast<int>(width) : mLayerWindow->width();
    const int resolvedHeight = height > 0 ? static_cast<int>(height) : mLayerWindow->height();
    mPendingSize = QSize(resolvedWidth, resolvedHeight);

    mConfigured = true;
    applyConfigureWhenPossible();

    // Qt does not re-ask a shell surface whether it is exposed on its own: isExposed() is read when the
    // window is mapped and the answer is remembered. Without this the surface stays unpainted forever
    // with no buffer ever attached, which is a bar that exists in the compositor's layer list and draws
    // nothing. QWaylandWindow::updateExposure is the documented way to say that a property exposure
    // depends on has changed.
    mWaylandWindow->updateExposure();
}

void LayerShellSurface::applyConfigure()
{
    if (mPendingSize.isEmpty())
        return;
    resizeFromApplyConfigure(mPendingSize);
}

void LayerShellSurface::zwlr_layer_surface_v1_closed()
{
    // The compositor has unmapped the surface: the output was removed, or the user asked for the
    // surface to go away. The window closes rather than lingering invisible.
    mLayerWindow->close();
}

} // namespace QuantumShell
