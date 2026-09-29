import QtQuick
import QuantumShell 1.0

// A notification as the session sees it for a moment: a surface on the top layer of an output, anchored to
// the top-right corner and reserving nothing.
//
// There is one of these per output, created by `src/app/ToastHost.cpp` when a notification arrives and
// destroyed when it is withdrawn — it is the one surface in the shell that comes and goes, because it exists
// to carry a message that a person reads and then does not need. It is not a layer surface the bar's kind is:
// the namespace is its own (`quantum-shell-toast`, part of the frozen public interface like the bar's is),
// and it reserves no exclusive zone — a notification is drawn *over* whatever is on screen rather than
// shrinking it, and niri will not move a window for it.
//
// Every layer-shell property below goes straight into the zwlr_layer_surface_v1 the C++ integration creates,
// the way the bar's do, so the numbers here are what the compositor is asked for.
LayerShellWindow {
    id: toastWindow

    // The sender's own facts, as the spec hands them and as the daemon publishes them: the application name,
    // the summary and the body. Assigned by `ToastHost` as initial properties, because a toast is created when
    // the notification arrives and there is nothing else for the component to read them from.
    required property string toastApplication
    required property string toastSummary
    required property string toastBody

    layer: LayerShellWindow.Top
    anchors: LayerShellWindow.TopEdge | LayerShellWindow.RightEdge
    keyboardInteractivity: LayerShellWindow.NoKeyboard

    // The toast's own namespace, part of the frozen public interface the bar's is: it begins with
    // `quantum-shell-` and is the name `niri msg layers` reports for it. It is not the bar's namespace and
    // not the configuration's — a notification is a surface of its own kind, and a namespace the bar's is
    // would make a toast and a bar the same surface in the compositor's layer list.
    layerNamespace: "quantum-shell-toast"

    // Nothing is reserved: the two properties the bar sets to shrink the tiling area are left alone, and the
    // toast is drawn over it. A notification that shrank a window underneath it would be an interruption the
    // person did not ask for, and the spec's own model of a notification is one that is drawn and then goes.
    exclusiveZone: 0

    // The toast's own size, on the axis it is not stretched along: anchored to the top and right edges only,
    // so the width is this window's and the compositor chooses the height — a notification is as tall as its
    // text, and the text is the one thing here that knows how long it is.
    readonly property color foreground: Config.bar.colors.foreground
    readonly property color muted: Config.bar.colors.muted
    readonly property color accent: Config.bar.colors.accent
    readonly property string face: Config.bar.font.family
    readonly property int fontSize: Config.bar.font.size
    readonly property int fontWeight: Config.bar.font.weight

    color: "#12131a"
    visible: false

    Rectangle {
        id: toast
        anchors.fill: parent
        anchors.margins: 10
        radius: 8
        color: "#1a1b26"

        Column {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 4

            Text {
                id: application
                text: toastWindow.toastApplication
                color: toastWindow.muted
                font.family: toastWindow.face
                font.pixelSize: toastWindow.fontSize
                font.weight: toastWindow.fontWeight
                elide: Text.ElideRight
                width: parent.width
            }

            Text {
                id: summary
                text: toastWindow.toastSummary
                color: toastWindow.foreground
                font.family: toastWindow.face
                font.pixelSize: toastWindow.fontSize
                font.weight: toastWindow.fontWeight
                elide: Text.ElideRight
                width: parent.width
            }

            Text {
                id: body
                text: toastWindow.toastBody
                color: toastWindow.foreground
                font.family: toastWindow.face
                font.pixelSize: toastWindow.fontSize
                font.weight: toastWindow.fontWeight
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                width: parent.width
                visible: toastWindow.toastBody !== ""
            }
        }
    }

    // Nothing presents the surface from here, and nothing hides it either: `ToastHost` does both, after
    // assigning the screen, for the same reason the bar's host does — showing the window is what creates the
    // Wayland surface and assigns the layer role against an output, and the role is assigned once.
}