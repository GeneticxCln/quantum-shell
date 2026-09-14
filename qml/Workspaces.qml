import QtQuick
import QuantumShell 1.0

// The workspace strip: one capsule per workspace, in the order the model reports them.
//
// The model is `NiriService.workspaces`, which is niri's own workspace list — so the strip reorders and
// marks itself focused because the compositor said so, from an event, with nothing here deciding what
// is on screen.
Item {
    id: root

    property color foreground
    property color muted
    property color accent
    property color urgent
    property string face

    Row {
        id: strip
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        Repeater {
            model: NiriService.workspaces

            delegate: Rectangle {
                id: capsule

                required property var modelData

                // niri's own flags, read rather than inferred: `isFocused` is the workspace the
                // compositor has focused, `isActive` one it considers visible on its output.
                readonly property bool focused: modelData.isFocused === true
                readonly property bool active: modelData.isActive === true
                readonly property bool urgent: modelData.isUrgent === true

                width: Math.max(strip.spacing, caption.implicitWidth + 16)
                height: 22
                radius: height / 2
                color: focused ? root.accent : "transparent"
                border.width: (!focused && active) || urgent ? 1 : 0
                border.color: urgent ? root.urgent : root.muted

                Text {
                    id: caption
                    anchors.centerIn: parent
                    // A named workspace shows its name; an unnamed one shows the index niri reports for
                    // it, which is what niri itself calls it.
                    text: capsule.modelData.name !== "" ? capsule.modelData.name
                                                        : String(capsule.modelData.idx)
                    color: capsule.focused ? "#12131a" : root.foreground
                    font.family: root.face
                    font.pixelSize: 12
                }
            }
        }
    }

    // The honest empty state, and it is a real check rather than a permanent label: `connected` is
    // niri's subscription being acknowledged, so this appears only when the strip above it is empty
    // because there is no compositor to fill it — not because there are no workspaces.
    Row {
        id: noCompositor
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6
        visible: !NiriService.connected

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 6
            height: 6
            radius: 3
            color: root.urgent
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "niri"
            color: root.muted
            font.family: root.face
            font.pixelSize: 12
        }
    }
}
