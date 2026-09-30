import QtQuick
import QuantumShell 1.0

// The keyboard layout niri is using, and the click that switches to the next one.
//
// The reading is `NiriService.keyboardLayout` — niri's own list of layouts and the index it reports as current,
// followed from its `KeyboardLayoutsChanged` and `KeyboardLayoutSwitched` events, so a switch made by a key
// binding moves this as much as a click on it does. The click asks niri for `SwitchLayout { layout: Next }`
// (`NiriActions.switchLayoutNext`) and draws nothing itself: what changes on screen is the event that comes back.
//
// It is drawn only when there is something to switch between. A machine with one layout — or a compositor that
// has not reported any — has no indicator, and takes no room: a permanent label naming the only layout would be
// a control that does nothing.
Item {
    id: root

    objectName: "keyboardLayout"

    property color foreground
    property color muted
    property string face
    property int fontSize: 12
    property int fontWeight: 400

    readonly property var layout: NiriService.keyboardLayout
    readonly property bool shown: layout.names !== undefined && layout.names.length > 1

    width: shown ? capsule.width : 0
    visible: shown

    Rectangle {
        id: capsule
        anchors.verticalCenter: parent.verticalCenter
        width: caption.implicitWidth + 16
        height: 22
        radius: height / 2
        color: "transparent"
        border.width: 1
        border.color: root.muted

        Text {
            id: caption
            // The layout's name is the compositor's text: drawn as the characters it is.
            textFormat: Text.PlainText
            anchors.centerIn: parent
            text: root.layout.currentName !== undefined ? root.layout.currentName : ""
            color: root.foreground
            font.family: root.face
            font.pixelSize: root.fontSize
            font.weight: root.fontWeight
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: NiriActions.switchLayoutNext()
        }
    }
}
