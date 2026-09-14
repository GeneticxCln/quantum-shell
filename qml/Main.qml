import QtQuick
import QuantumShell 1.0

// The shell itself, as far as it exists: one bar, on the top layer of the output it is placed on.
//
// Every layer-shell property below goes straight into the zwlr_layer_surface_v1 the C++ integration
// creates, so the numbers here are what the compositor is asked for rather than a description of how
// this file draws itself. `niri msg layers` reports the namespace, the layer and the keyboard
// interactivity this window was created with.
LayerShellWindow {
    id: barWindow

    // The shell's namespaces are frozen public names (AGENTS.md); the integration refuses any surface
    // whose namespace is outside quantum-shell-*, so a configured value is checked before it reaches
    // niri, and again here by the integration itself. It comes from the configuration file rather than
    // from a string in this file.
    //
    // Unlike the height below, a change to this one cannot be applied to a surface that already exists:
    // the protocol assigns a surface's role — and its namespace with it — once. The integration reports
    // a live change rather than quietly keeping the old name.
    layerNamespace: Config.bar.layerNamespace

    // Top layer, pinned to the top edge and stretched across the output. Anchoring to both the left and
    // right edges is what makes the compositor choose the width — the surface asks for zero on that
    // axis, which is the only value the protocol allows when an axis is stretched.
    layer: LayerShellWindow.Top
    anchors: LayerShellWindow.TopEdge | LayerShellWindow.LeftEdge | LayerShellWindow.RightEdge
    keyboardInteractivity: LayerShellWindow.NoKeyboard

    // The bar's own height, reserved from the tiling area rather than drawn over it: niri will not put
    // a window underneath this strip. It is the configured height, and unlike the namespace it applies
    // to a surface that is already on screen: editing it resizes the bar instead of needing a restart.
    readonly property int barHeight: Config.bar.height
    height: barHeight
    exclusiveZone: barHeight

    // Qualified onto the window, because there is no bare `screen` in a QML file: unqualified it is a
    // ReferenceError, which this shell printed on every start while the bar still worked — the width it
    // feeds is the window's, and the stretched axis above makes the compositor choose the real one. It is
    // fixed rather than left alone because the same value is what a surface on an axis that is *not*
    // stretched would be created with, and a binding that fails silently is the thing this repository
    // refuses.
    width: barWindow.screen ? barWindow.screen.width : 0

    color: "#12131a"
    visible: false

    Bar {
        id: bar
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
    }

    // Not `visible: true`: the window is shown here, after every property above has been assigned.
    // Showing it is what creates the Wayland surface and assigns the layer role, and the protocol
    // forbids attaching a buffer before the compositor has acknowledged the first configure.
    Component.onCompleted: present()
}
