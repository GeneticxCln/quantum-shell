import QtQuick
import QuantumShell 1.0

// The bar: one layer-shell surface, on one output.
//
// There is one of these per output, and `src/app/BarHost.cpp` is what creates them — one instance of this
// file per screen, with the screen handed in as `targetScreen` before the component completes, because a
// layer surface is created against the output its window is on and the output cannot be changed after the
// role is assigned. That is why `targetScreen` is required rather than defaulted: a bar with no output is
// not a bar on the primary monitor, it is a bar nowhere, and it would be mapped against whichever output
// the compositor picked for a window that never asked.
//
// Every layer-shell property below goes straight into the zwlr_layer_surface_v1 the C++ integration
// creates, so the numbers here are what the compositor is asked for rather than a description of how
// this file draws itself. `niri msg layers` reports the namespace, the layer and the keyboard
// interactivity this window was created with.
LayerShellWindow {
    id: barWindow

    // Which output this bar is on, named as niri names it. Assigned by `src/app/BarHost.cpp` as an initial
    // property, because it cannot be read from here: `screen` is not a QML property of this window — Qt
    // exposes it on the QML `Window` type rather than on `QQuickWindow` — so a bar has no way to look up the
    // output it was placed on. `BarHost` is also what assigns the window's screen, and it does that before the
    // surface is presented.
    //
    // The name is what the workspace strip is filtered by: a workspace names the output it is on, so the strip
    // on this bar draws this output's workspaces and no others (`qml/Workspaces.qml`).
    required property string outputName
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

    // No width is asked for. The bar is anchored to both horizontal edges, and on an axis a surface is
    // stretched along the client must ask for zero — the compositor decides that size — which is exactly what
    // the integration sends (`LayerShellSurface::proposeAlongAxis` returns zero for a stretched axis). On an
    // axis that is *not* stretched what the surface is created with is the output's own size, read there from
    // the window's screen; that is why the screen is assigned in C++ before the surface is presented.
    color: "#12131a"
    visible: false

    Bar {
        id: bar
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        // The strip is per-output, so the bar tells it which output it is on rather than the widget asking
        // the window it was placed in — a widget is placed by the group it is declared in and knows nothing
        // about the surface behind it.
        outputName: barWindow.outputName
    }

    // Nothing presents the surface from here. `BarHost` does it, after assigning the screen: showing the window
    // is what creates the Wayland surface and assigns the layer role against that output, the protocol assigns
    // the role once, and the protocol forbids attaching a buffer before the compositor has acknowledged the
    // first configure — so the output has to be right before the window is mapped.
}
