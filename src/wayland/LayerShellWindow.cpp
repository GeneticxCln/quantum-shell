#include "wayland/LayerShellWindow.h"

#include "app/Logging.h"

#include <QDebug>

namespace QuantumShell {

LayerShellWindow::LayerShellWindow(QWindow *parent)
    : QQuickWindow(parent)
{
}

void LayerShellWindow::setLayer(Layer layer)
{
    if (m_layer == layer)
        return;
    m_layer = layer;
    Q_EMIT configurationChanged();
}

void LayerShellWindow::setAnchors(Anchors anchors)
{
    if (m_anchors == anchors)
        return;
    m_anchors = anchors;
    Q_EMIT configurationChanged();
}

void LayerShellWindow::setExclusiveZone(int zone)
{
    if (m_exclusiveZone == zone)
        return;
    m_exclusiveZone = zone;
    Q_EMIT configurationChanged();
}

void LayerShellWindow::setKeyboardInteractivity(KeyboardInteractivity interactivity)
{
    if (m_keyboardInteractivity == interactivity)
        return;
    m_keyboardInteractivity = interactivity;
    Q_EMIT configurationChanged();
}

void LayerShellWindow::setLayerNamespace(const QString &layerNamespace)
{
    if (m_layerNamespace == layerNamespace)
        return;
    m_layerNamespace = layerNamespace;
    Q_EMIT configurationChanged();
}

void LayerShellWindow::setMargins(const QMargins &margins)
{
    if (m_margins == margins)
        return;
    m_margins = margins;
    Q_EMIT configurationChanged();
}

void LayerShellWindow::present()
{
    if (isVisible())
        return;
    if (m_layerNamespace.isEmpty()) {
        // A layer surface must carry a namespace, and the shell's namespaces are part of its frozen
        // public interface (AGENTS.md). Refusing here is the honest outcome: a surface created with
        // an empty or foreign namespace would be one the shell cannot claim.
        qCWarning(quantum::app::waylandLog)
            << "LayerShellWindow: refusing to show a surface with no layer namespace set";
        return;
    }
    show();
}

} // namespace QuantumShell
