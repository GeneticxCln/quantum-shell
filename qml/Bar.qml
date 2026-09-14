import QtQuick

// What the bar shows and how it is arranged.
//
// It holds no value of its own: the colours and the type below describe how the bar looks, and every
// string and number drawn comes from NiriService or from the system clock. There is nothing here that
// would still be shown if the compositor stopped answering.
Item {
    id: bar

    // The bar's palette and type, in one place so the two children below cannot drift apart.
    readonly property color foreground: "#c8cad8"
    readonly property color muted: "#5a5d70"
    readonly property color accent: "#7aa2f7"
    readonly property color urgent: "#f7768e"
    readonly property string face: "Inter"

    Workspaces {
        id: workspaceStrip
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height
        foreground: bar.foreground
        muted: bar.muted
        accent: bar.accent
        urgent: bar.urgent
        face: bar.face
    }

    Clock {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        color: bar.foreground
        face: bar.face
    }
}
