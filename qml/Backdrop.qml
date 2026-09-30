import QtQuick
import QuantumShell 1.0

// The transparent surface behind a panel: a press anywhere on it says the person clicked outside the panel.
//
// It is created and destroyed by `src/app/Backdrop.cpp` for the launcher, the control centre and the notification
// history. It is on the top layer and the panels are on the overlay layer, which is what puts the panel above it:
// the protocol orders layers, and does not order two surfaces on the same layer (measured on sway, where a
// backdrop on the overlay layer beside the panel took the clicks that were meant for the panel). It draws nothing
// and reserves nothing: the
// exclusive zone of -1 asks to extend under the bar and every other reserved edge, so the whole output is covered,
// and it takes no keyboard, which stays with the panel.
//
// It only emits; what closing means is the panel's service's, and the host that owns both surfaces answers.
LayerShellWindow {
    id: backdrop

    signal dismissed()

    layer: LayerShellWindow.Top
    // All four edges: both axes stretch, so the compositor names the size (the surface asks for zero).
    anchors: LayerShellWindow.TopEdge | LayerShellWindow.BottomEdge | LayerShellWindow.LeftEdge | LayerShellWindow.RightEdge
    keyboardInteractivity: LayerShellWindow.NoKeyboard
    layerNamespace: "quantum-shell-backdrop"
    exclusiveZone: -1

    color: "transparent"
    visible: false

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: backdrop.dismissed()
    }
}
