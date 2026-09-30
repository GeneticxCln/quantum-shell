import QtQuick
// The palette comes from the configuration, which is a singleton of this module (`Config`), so this file
// imports it the way `qml/Workspaces.qml` imports the module for `NiriService`. Without the import the
// bindings below are a `ReferenceError` at load and the bar draws transparent black — which is what the
// palette slot in `bar-interaction-test` caught when this was first written without it.
import QuantumShell 1.0

// What the bar shows and how it is arranged.
//
// It holds no value of its own: the colours and the type below describe how the bar looks, and every string
// and number drawn comes from a service behind one of the readouts — `NiriService` for the workspaces,
// `SysMonService` for the system status, `PipeWireService`, `NetworkService`, `BatteryService`,
// `MediaService` and `NotificationService` for the groups at the trailing edge, and the system clock.
// There is nothing here that would still be shown if those stopped answering.
//
// The arrangement is the groups below and nothing else. A widget is placed by being declared inside the
// group it belongs to, carrying no coordinate, no anchor and no offset of its own; the bar decides where
// each group sits and how tall it is, and the group decides the order, the spacing and the height its
// widgets get. A widget's declaration is therefore its look and its content, and never its position.
//
// Three groups, and the middle one arrived the way the other two did — with the widget that belongs in it.
// There is still no group for a widget that does not exist: a region exists when something is declared in
// it, so the toast surface notifications still owe the roadmap will arrive with its own group or in one of
// these, and not before.
Item {
    id: bar

    // Which output this bar is on, named the way niri names it. `qml/Main.qml` reads it from the output its
    // own surface was created against and passes it down, because a widget knows nothing about the window
    // behind it. Only the workspace strip needs it: every other readout is a reading of the machine rather
    // than of a monitor, and is the same number on both bars.
    //
    // Empty means the bar was not placed on an output — which is how this component is loaded when it is
    // loaded on its own, without a shell around it. The strip then draws the whole model rather than
    // nothing, because "no output" is not "an output with no workspaces".
    property string outputName: ""

    // The palette, from `[bar.colors]` (`Config.bar.colors`). They are `color` properties bound to strings the
    // schema has already checked are `#` and six or eight hex digits, which is the division of labour every
    // value in this file follows: `src/config/` decides what a value may be and refuses anything else by name,
    // and QML decides what to draw with it. A colour the file got wrong never reaches here, so there is no
    // fallback to write and nothing to draw in black.
    //
    // Four values and not a theme's worth of tokens: these are the four the widgets below actually draw with,
    // and a palette entry nothing reads would be surface nobody asked for.
    readonly property color foreground: Config.bar.colors.foreground
    readonly property color muted: Config.bar.colors.muted
    readonly property color accent: Config.bar.colors.accent
    readonly property color urgent: Config.bar.colors.urgent

    // The typeface, from `[bar.font]` (`Config.bar.font`) rather than a literal, and the two sizes the
    // widgets that draw larger than the body derive from the one number: the clock by one, the two readouts
    // that draw a line of prose by two. That is the hierarchy the bar had before the table existed — 12, 13
    // and 14 as its widgets were drawn — preserved exactly by the default of 12, so changing one number
    // reflows the whole bar rather than leaving three sizes to drift apart.
    //
    // The offsets are here rather than in the widgets because this is where the hierarchy is declared once; a
    // widget knows it is bigger than the body, and the number it is bigger by is this file's business.
    readonly property string face: Config.bar.font.family
    readonly property int fontSize: Config.bar.font.size
    readonly property int fontWeight: Config.bar.font.weight

    // The leading edge: the workspace strip, and whatever else belongs before everything else.
    CapsuleGroup {
        name: "left"
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        height: bar.height

        Workspaces {
            outputName: bar.outputName
            foreground: bar.foreground
            muted: bar.muted
            accent: bar.accent
            urgent: bar.urgent
            face: bar.face
            fontSize: bar.fontSize
            fontWeight: bar.fontWeight
        }

        Windows {
            outputName: bar.outputName
            foreground: bar.foreground
            muted: bar.muted
            accent: bar.accent
            urgent: bar.urgent
            face: bar.face
            fontSize: bar.fontSize
            fontWeight: bar.fontWeight
        }
    }

    // The centre: the system's own readings, and whatever else belongs in the middle. Centred on the bar
    // rather than on the room the side groups left, which is what a group cannot decide for itself — see
    // `CapsuleGroup.qml`, where the case that breaks (a bar narrower than its three groups) is written down.
    CapsuleGroup {
        name: "centre"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        height: bar.height

        SystemMonitor {
            foreground: bar.foreground
            muted: bar.muted
            face: bar.face
            fontSize: bar.fontSize
            fontWeight: bar.fontWeight
        }

    }

    // The trailing edge: the network, the battery, the media player, the notification readout, the volume,
    // the clock, and whatever else belongs after everything else. Each of these arrived in this group rather than one of their own
    // because a group arrives with the widget that belongs in it, and these belong at the trailing edge
    // with the other system state.
    CapsuleGroup {
        name: "right"
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: bar.height

        // The order is the machine's condition first, then what is playing, then the control, then the
        // clock. The network is the outermost of those facts — a connection is what everything else on this
        // edge is happening through — and the battery is the other one about the machine's own state, so the
        // two read together and the media follows them as what is happening, and the volume follows that as
        // the one widget here a person changes by hand; the notification readout sits between the media
        // player and the volume as the other thing a person only reads. The time keeps the corner.
        KeyboardLayout {
            foreground: bar.foreground
            muted: bar.muted
            face: bar.face
            fontSize: bar.fontSize
            fontWeight: bar.fontWeight
        }

        Network {
            foreground: bar.foreground
            muted: bar.muted
            urgent: bar.urgent
            face: bar.face
            fontSize: bar.fontSize
            fontWeight: bar.fontWeight
        }

        Battery {
            foreground: bar.foreground
            muted: bar.muted
            urgent: bar.urgent
            face: bar.face
            fontSize: bar.fontSize
            fontWeight: bar.fontWeight
        }

        Media {
            foreground: bar.foreground
            muted: bar.muted
            accent: bar.accent
            face: bar.face
            // Two up: a line of prose is drawn larger than the readouts' numbers, which is the hierarchy the
            // bar had and is preserved by the default.
            fontSize: bar.fontSize + 2
            fontWeight: bar.fontWeight
        }

        Notifications {
            foreground: bar.foreground
            muted: bar.muted
            face: bar.face
            fontSize: bar.fontSize + 2
            fontWeight: bar.fontWeight
        }

        Volume {
            foreground: bar.foreground
            muted: bar.muted
            accent: bar.accent
            face: bar.face
            fontSize: bar.fontSize
            fontWeight: bar.fontWeight
        }

        Clock {
            color: bar.foreground
            face: bar.face
            // One up: the time is the readout a person reads most and the only one on the bar without a
            // label beside it, which is why it drew at 13 while the numbers drew at 12.
            fontSize: bar.fontSize + 1
            fontWeight: bar.fontWeight
        }
    }
}
