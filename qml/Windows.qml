import QtQuick
import QuantumShell 1.0

// The window list: one entry per window on this output's workspaces, in the order the model reports them, and
// the click that acts on one.
//
// The model is `NiriService.windows`, niri's own window list, filtered to the windows whose workspace is one of
// this output's — the workspace a window names is niri's field, and the output a workspace names is too, so a bar
// on one monitor lists that monitor's windows and not the other's. A window niri reports on no workspace is on
// no output and so on none of the bars; the widget loaded on its own (`outputName` empty) lists the whole model,
// for the reason `Workspaces.qml` does. Which entry is marked and which is urgent are niri's flags, read rather
// than inferred, and a click asks niri to focus the window the entry names by the id text the model reported
// (`NiriActions.focusWindowById`), so nothing here holds a window of its own.
//
// An empty list draws nothing and takes no room: a bar with no windows is not a bar with a blank capsule.
Item {
    id: root

    objectName: "windows"

    // Which output this list belongs to, named as niri names it, assigned by the bar from its own surface.
    property string outputName

    readonly property var outputWorkspaceIds: NiriService.workspaces
        .filter(workspace => outputName === "" || workspace.output === outputName)
        .map(workspace => workspace.id)

    readonly property var windows: outputName === ""
                                   ? NiriService.windows
                                   : NiriService.windows.filter(window => window.workspaceId !== undefined
                                                                          && outputWorkspaceIds.indexOf(window.workspaceId) >= 0)

    property color foreground
    property color muted
    property color accent
    property color urgent
    property string face
    property int fontSize: 12
    property int fontWeight: 400

    // The widest one entry gets. Longer titles are elided rather than pushing the rest of the bar around, and
    // the whole is bounded by the room the bar's own layout gives the left group.
    readonly property int entryMaxWidth: 180

    width: strip.width

    Row {
        id: strip
        objectName: "windowStrip"
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        Repeater {
            model: root.windows

            delegate: Rectangle {
                id: entry

                required property var modelData

                readonly property bool focused: modelData.isFocused === true
                readonly property bool urgentWindow: modelData.isUrgent === true
                // The title is what the window says it is; an application that sets none is named by its app id,
                // which is what niri reports for it, and a window with neither is drawn as nothing rather than
                // as a made-up name.
                readonly property string label: modelData.title !== "" ? modelData.title : modelData.appId

                width: Math.min(root.entryMaxWidth, caption.implicitWidth + 16)
                height: 22
                radius: height / 2
                color: focused ? root.accent : "transparent"
                border.width: urgentWindow || !focused ? 1 : 0
                border.color: urgentWindow ? root.urgent : root.muted

                Text {
                    id: caption
                    // Outside text is drawn as the characters it is: a title can look like markup.
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent.verticalCenter
                    x: 8
                    width: Math.min(implicitWidth, root.entryMaxWidth - 16)
                    elide: Text.ElideRight
                    text: entry.label
                    color: entry.focused ? "#12131a" : root.foreground
                    font.family: root.face
                    font.pixelSize: root.fontSize
                    font.weight: root.fontWeight
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    // Not on the focused window: asking niri to focus what is focused changes nothing and is a
                    // request for nothing.
                    onClicked: {
                        if (!entry.focused)
                            NiriActions.focusWindowById(entry.modelData.id)
                    }
                }
            }
        }
    }
}
