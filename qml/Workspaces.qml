import QtQuick
import QuantumShell 1.0

// The workspace strip: one capsule per workspace, in the order the model reports them, and the two
// gestures that act on them.
//
// The model is `NiriService.workspaces`, which is niri's own workspace list — so the strip reorders and
// marks itself focused because the compositor said so, from an event, with nothing here deciding what
// is on screen. Acting on a capsule is `NiriActions.focusWorkspaceById(id)`, the id exactly as the model
// reports it, and the wheel is `focusWorkspaceUp()`/`focusWorkspaceDown()` — niri's own actions, so the
// order the wheel moves through is the compositor's and not one computed from the strip. Nothing here
// talks to a socket or holds a workspace id of its own.
Item {
    id: root

    property color foreground
    property color muted
    property color accent
    property color urgent
    property string face

    // The strip is the part of the bar a pointer can land on, so the root takes its width from it: an
    // Item with no width of its own has none, and a pointer event is hit-tested against the bounds of the
    // items it walks. The bar gives the height (Main.qml anchors this to fill it); nothing gives the
    // width but this.
    width: Math.max(strip.width, noCompositor.width)

    Row {
        id: strip
        objectName: "strip"
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        // The wheel over the strip, mapped the way niri maps the same gesture on the desktop: its default
        // config binds `WheelScrollDown` to `focus-workspace-down` and `WheelScrollUp` to
        // `focus-workspace-up`, which is what makes scrolling over the bar behave like scrolling over a
        // window. `TouchPad` is named explicitly because it is not the default, and a touchpad that sends
        // scroll phases rather than wheel ticks would otherwise be ignored.
        //
        // One gap, stated rather than hidden: niri's own bind carries `cooldown-ms=150`, and this handler
        // acts on every wheel event it is given. A mouse wheel sends one event per notch, so the two agree
        // there; a continuous touchpad scroll can send several, and this is where a cooldown belongs when
        // the bar has one.
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (event) => {
                if (event.angleDelta.y < 0)
                    NiriActions.focusWorkspaceDown()
                else if (event.angleDelta.y > 0)
                    NiriActions.focusWorkspaceUp()
            }
        }

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

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    // Not on the focused capsule. niri resolves the reference and then switches to it,
                    // and with `workspace-auto-back-and-forth` — which this project's own session sets —
                    // switching to the workspace already focused lands on the previously focused one, so
                    // asking again would move the person somewhere else rather than nowhere.
                    onClicked: {
                        if (!capsule.focused)
                            NiriActions.focusWorkspaceById(capsule.modelData.id)
                    }
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
