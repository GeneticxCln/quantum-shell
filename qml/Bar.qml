import QtQuick

// What the bar shows and how it is arranged.
//
// It holds no value of its own: the colours and the type below describe how the bar looks, and every string
// and number drawn comes from `NiriService`, from `SysMonService` or from the system clock. There is nothing
// here that would still be shown if the compositor stopped answering and /proc stopped being readable.
//
// The arrangement is the groups below and nothing else. A widget is placed by being declared inside the
// group it belongs to, carrying no coordinate, no anchor and no offset of its own; the bar decides where
// each group sits and how tall it is, and the group decides the order, the spacing and the height its
// widgets get. A widget's declaration is therefore its look and its content, and never its position.
//
// Three groups, and the middle one arrived the way the other two did — with the widget that belongs in it.
// There is still no group for a widget that does not exist: a region exists when something is declared in
// it, so the media readout the roadmap lists will arrive with its own group or in one of these, and not
// before.
Item {
    id: bar

    readonly property color foreground: "#c8cad8"
    readonly property color muted: "#5a5d70"
    readonly property color accent: "#7aa2f7"
    readonly property color urgent: "#f7768e"
    readonly property string face: "Inter"

    // The leading edge: the workspace strip, and whatever else belongs before everything else.
    CapsuleGroup {
        name: "left"
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        height: bar.height

        Workspaces {
            foreground: bar.foreground
            muted: bar.muted
            accent: bar.accent
            urgent: bar.urgent
            face: bar.face
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
        }

    }

    // The trailing edge: the network, the battery, the media player, the volume, the clock, and whatever
    // else belongs after everything else. Each of these arrived in this group rather than one of their own
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
        // the one widget here a person changes by hand. The time keeps the corner.
        Network {
            foreground: bar.foreground
            muted: bar.muted
            urgent: bar.urgent
            face: bar.face
        }

        Battery {
            foreground: bar.foreground
            muted: bar.muted
            urgent: bar.urgent
            face: bar.face
        }

        Media {
            foreground: bar.foreground
            muted: bar.muted
            accent: bar.accent
            face: bar.face
        }

        Volume {
            foreground: bar.foreground
            muted: bar.muted
            accent: bar.accent
            face: bar.face
        }

        Clock {
            color: bar.foreground
            face: bar.face
        }
    }
}
