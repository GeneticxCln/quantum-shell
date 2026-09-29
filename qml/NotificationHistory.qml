import QtQuick
import QuantumShell 1.0

// The notification history: what the shell has been sent, newest first, in a surface of its own on the top
// layer, anchored to the top-right corner and reserving nothing.
//
// It is created by `src/app/HistoryHost.cpp` when the service says the panel is open and destroyed when it
// says it is not, so this component holds no open/closed state of its own — the readout in the bar toggles
// `NotificationService.notificationHistoryOpen` and the host follows it. Everything drawn is the service's:
// the list is `notificationHistory`, the mode is `notificationDoNotDisturb`, and the two buttons call the
// service's own `clearNotificationHistory()` and write its own property.
//
// Its own namespace, `quantum-shell-notification-history`, part of the frozen public interface like the bar's
// and the toast's. Like the toast it names its own size: anchored to two edges only, a surface that declared
// nothing would be proposed the whole output.
LayerShellWindow {
    id: panel

    layer: LayerShellWindow.Top
    anchors: LayerShellWindow.TopEdge | LayerShellWindow.RightEdge
    keyboardInteractivity: LayerShellWindow.NoKeyboard
    layerNamespace: "quantum-shell-notification-history"
    exclusiveZone: 0

    readonly property color foreground: Config.bar.colors.foreground
    readonly property color muted: Config.bar.colors.muted
    readonly property color accent: Config.bar.colors.accent
    readonly property string face: Config.bar.font.family
    readonly property int fontSize: Config.bar.font.size
    readonly property int fontWeight: Config.bar.font.weight

    // A width, and a height that is what the panel holds up to a ceiling: past it the list scrolls, and the
    // ceiling is well inside any output a bar can be on. The margins are written out rather than folded into
    // one number: the panel sits 10 in from its window, the header 12 in from the panel, the list 12 below the
    // header and 12 above the panel's bottom, and an empty list still needs the line that says so.
    readonly property int maxHeight: 520
    readonly property int chrome: 10 + 12 + 12 + 12 + 10
    width: 400
    height: Math.min(maxHeight, chrome + header.height + Math.max(list.contentHeight, 24))

    color: "transparent"
    visible: false

    Rectangle {
        anchors.fill: parent
        anchors.margins: 10
        radius: 8
        color: "#1a1b26"

        Row {
            id: header
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            height: 26
            spacing: 12

            Text {
                textFormat: Text.PlainText
                objectName: "historyTitle"
                text: NotificationService.notificationHistoryCount + " received"
                color: panel.muted
                font.family: panel.face
                font.pixelSize: panel.fontSize
                font.weight: panel.fontWeight
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - dndButton.width - clearButton.width - 2 * parent.spacing
                elide: Text.ElideRight
            }

            Text {
                id: dndButton
                textFormat: Text.PlainText
                objectName: "historyDoNotDisturb"
                text: NotificationService.notificationDoNotDisturb ? "Do Not Disturb: on" : "Do Not Disturb: off"
                color: NotificationService.notificationDoNotDisturb ? panel.accent : panel.foreground
                font.family: panel.face
                font.pixelSize: panel.fontSize
                font.weight: panel.fontWeight
                anchors.verticalCenter: parent.verticalCenter
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: NotificationService.notificationDoNotDisturb = !NotificationService.notificationDoNotDisturb
                }
            }

            Text {
                id: clearButton
                textFormat: Text.PlainText
                objectName: "historyClear"
                text: "Clear"
                color: panel.foreground
                font.family: panel.face
                font.pixelSize: panel.fontSize
                font.weight: panel.fontWeight
                anchors.verticalCenter: parent.verticalCenter
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: NotificationService.clearNotificationHistory()
                }
            }
        }

        ListView {
            id: list
            objectName: "historyList"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: header.bottom
            anchors.bottom: parent.bottom
            anchors.margins: 12
            clip: true
            spacing: 8
            model: NotificationService.notificationHistory
            interactive: contentHeight > height

            delegate: Item {
                id: entry
                required property var modelData
                width: list.width
                height: column.implicitHeight

                Column {
                    id: column
                    width: parent.width
                    spacing: 2

                    Row {
                        width: parent.width
                        spacing: 8
                        Text {
                            textFormat: Text.PlainText
                            text: entry.modelData.application
                            color: panel.muted
                            font.family: panel.face
                            font.pixelSize: panel.fontSize
                            font.weight: panel.fontWeight
                            elide: Text.ElideRight
                            width: parent.width - stamp.width - parent.spacing
                        }
                        Text {
                            id: stamp
                            textFormat: Text.PlainText
                            text: Qt.formatDateTime(new Date(entry.modelData.received), "HH:mm")
                            color: panel.muted
                            font.family: panel.face
                            font.pixelSize: panel.fontSize
                            font.weight: panel.fontWeight
                        }
                    }
                    Text {
                        textFormat: Text.PlainText
                        text: entry.modelData.summary
                        color: panel.foreground
                        font.family: panel.face
                        font.pixelSize: panel.fontSize
                        font.weight: panel.fontWeight
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        width: parent.width
                    }
                    Text {
                        textFormat: Text.PlainText
                        visible: entry.modelData.body !== ""
                        text: entry.modelData.body
                        color: panel.foreground
                        font.family: panel.face
                        font.pixelSize: panel.fontSize
                        font.weight: panel.fontWeight
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        width: parent.width
                    }
                }

                // A click on an entry removes it from the history.
                MouseArea {
                    anchors.fill: parent
                    onClicked: NotificationService.removeFromNotificationHistory(entry.modelData.id)
                }
            }
        }

        // The honest empty state, driven by the count rather than by a label that is always there.
        Text {
            textFormat: Text.PlainText
            objectName: "historyEmpty"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: header.bottom
            anchors.topMargin: 18
            visible: NotificationService.notificationHistoryCount === 0
            text: "Nothing received yet"
            color: panel.muted
            font.family: panel.face
            font.pixelSize: panel.fontSize
            font.weight: panel.fontWeight
        }
    }
}
