import QtQuick
import QuantumShell 1.0

// The launcher: a text field and the applications that match it, in a surface of its own on the overlay layer.
//
// It is created by `src/app/LauncherHost.cpp` when `LauncherService.open` becomes true and destroyed when it
// becomes false, so this component keeps no open/closed state: Escape and Enter write the service's property and
// the host follows it. Everything drawn is the service's — the results are `LauncherService.results`, the highlight
// is `selectedIndex`, and what a keystroke does is `query` being written and `moveSelection` / `launchSelected`
// being called. Nothing here reads a directory or starts a process; that is `src/apps/`.
//
// Its own namespace, `quantum-shell-launcher`, part of the frozen public interface like the bar's. It takes the
// keyboard exclusively while it is up (a launcher that lost the keys to the window behind it would type into
// that window), and anchors to nothing, which is how a layer surface is centred on its output: the compositor
// centres a surface along every axis it is not anchored to, and the client names both sizes.
LayerShellWindow {
    id: launcher

    layer: LayerShellWindow.Overlay
    // No `anchors`: the default is none, which is what centres the surface.
    keyboardInteractivity: LayerShellWindow.ExclusiveKeyboard
    layerNamespace: "quantum-shell-launcher"
    exclusiveZone: 0

    readonly property color foreground: Config.bar.colors.foreground
    readonly property color muted: Config.bar.colors.muted
    readonly property color accent: Config.bar.colors.accent
    readonly property string face: Config.bar.font.family
    readonly property int fontSize: Config.bar.font.size
    readonly property int fontWeight: Config.bar.font.weight

    // The rows one result takes, and the surface's height as a function of how many there are. A launcher with no
    // match still has a row, because it says so.
    readonly property int rowHeight: 44
    readonly property int inputHeight: 52
    readonly property int rows: Math.max(1, LauncherService.results.length)
    readonly property int chrome: 10 * 2 + 12 * 2 + inputHeight + 8
    width: 600
    height: chrome + rows * rowHeight

    color: "transparent"
    visible: false

    Rectangle {
        anchors.fill: parent
        anchors.margins: 10
        radius: 10
        color: "#1a1b26"

        // The keys. The keyboard focus is the text field's — typing has to reach it — and a key the field does not
        // use travels up to this item, which is where Escape closes and the arrows move the highlight. Enter is the
        // field's own `accepted`, once: handling it here as well would start the application twice.
        Item {
            id: keys
            anchors.fill: parent

            Keys.onEscapePressed: LauncherService.open = false
            Keys.onUpPressed: LauncherService.moveSelection(-1)
            Keys.onDownPressed: LauncherService.moveSelection(1)

            Rectangle {
                id: field
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                height: launcher.inputHeight
                radius: 6
                color: "transparent"
                border.width: 1
                border.color: launcher.accent

                TextInput {
                    id: input
                    objectName: "launcherInput"
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    verticalAlignment: TextInput.AlignVCenter
                    color: launcher.foreground
                    font.family: launcher.face
                    font.pixelSize: launcher.fontSize + 4
                    font.weight: launcher.fontWeight
                    clip: true
                    focus: true
                    // The service's query is the truth: what is typed is written to it, and it is written
                    // back so a query the service cleared (opening starts empty) empties the field.
                    text: LauncherService.query
                    onTextEdited: LauncherService.query = text
                    onAccepted: LauncherService.launchSelected()
                }
            }

            ListView {
                id: list
                objectName: "launcherList"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: field.bottom
                anchors.bottom: parent.bottom
                anchors.topMargin: 8
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                anchors.bottomMargin: 12
                interactive: false
                model: LauncherService.results
                currentIndex: LauncherService.selectedIndex

                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: list.width
                    height: launcher.rowHeight
                    radius: 6
                    color: index === LauncherService.selectedIndex ? Qt.alpha(launcher.accent, 0.25) : "transparent"

                    MouseArea {
                        anchors.fill: parent
                        onClicked: LauncherService.launch(index)
                    }

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12

                        Text {
                            textFormat: Text.PlainText
                            text: modelData.name
                            width: parent.width
                            elide: Text.ElideRight
                            color: launcher.foreground
                            font.family: launcher.face
                            font.pixelSize: launcher.fontSize + 1
                            font.weight: launcher.fontWeight
                        }
                        Text {
                            textFormat: Text.PlainText
                            text: modelData.comment
                            width: parent.width
                            elide: Text.ElideRight
                            visible: modelData.comment !== ""
                            color: launcher.muted
                            font.family: launcher.face
                            font.pixelSize: launcher.fontSize - 1
                            font.weight: launcher.fontWeight
                        }
                    }
                }

                // The empty state, driven by the service: no application matches (or none was found).
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.top
                    anchors.verticalCenterOffset: launcher.rowHeight / 2
                    visible: LauncherService.results.length === 0
                    textFormat: Text.PlainText
                    text: LauncherService.scanning ? "Reading applications…"
                          : LauncherService.applicationCount === 0 ? "No applications found"
                          : "No match"
                    color: launcher.muted
                    font.family: launcher.face
                    font.pixelSize: launcher.fontSize
                    font.weight: launcher.fontWeight
                }
            }
        }
    }
}
