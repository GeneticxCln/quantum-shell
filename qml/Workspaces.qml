import QtQuick
import QuantumShell 1.0

// The workspace strip: one capsule per workspace on this output, in the order the model reports them, and
// the two gestures that act on them.
//
// The model is `NiriService.workspaces`, which is niri's own workspace list, filtered to the output this
// bar is on — so the strip reorders and marks itself focused because the compositor said so, from an event,
// with nothing here deciding what is on screen. Acting on a capsule is `NiriActions.focusWorkspaceById(id)`,
// the id exactly as the model reports it, and the wheel is `focusWorkspaceUp()`/`focusWorkspaceDown()` —
// niri's own actions, so the order the wheel moves through is the compositor's and not one computed from
// the strip. Nothing here talks to a socket or holds a workspace id of its own.
Item {
    id: root

    // Named the way the row of capsules below is, so the group this widget landed in can be read without
    // counting the bar's children.
    objectName: "workspaces"

    // Which output this strip belongs to, named as niri names it. The bar assigns it from the output its own
    // surface is on.
    property string outputName

    // The strip draws this output's workspaces, and the filter is niri's own field rather than a deduction
    // from anything here: a workspace names the output it is on, so a bar on one monitor draws the
    // workspaces of that monitor and not the ones the person is looking at on the other.
    //
    // A workspace niri reports with no output — one that is on no monitor at all — therefore appears on none
    // of the bars, which is the same statement rather than a gap: the strip on an output shows that output's
    // workspaces, and a workspace nothing is showing is not on it. An empty `outputName` is the component
    // loaded on its own rather than placed on an output (`qml/Bar.qml`), and then the whole model is the
    // honest reading — there is no output whose set could be meant.
    readonly property var workspaces: outputName === ""
                                      ? NiriService.workspaces
                                      : NiriService.workspaces.filter(workspace => workspace.output === outputName)

    property color foreground
    property color muted
    property color accent
    property color urgent
    property string face
    // From `[bar.font]` through `Bar.qml`; the defaults are what this component drew before the table existed.
    property int fontSize: 12
    property int fontWeight: 400

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
        // One step per notch of *distance*, not per event. A mouse wheel sends 120 units a notch, so it steps once
        // a notch; a touchpad or a high-resolution wheel sends the same distance as many small deltas, and acting
        // on each of them made one swipe over the bar walk through every workspace (niri's own bind carries
        // `cooldown-ms=150` for the same reason). The distance is accumulated and a step is taken for every whole
        // 120 of it, the remainder kept for the next event and dropped when the gesture ends, so it needs no
        // timer and a swipe that covers two notches of distance steps twice.
        WheelHandler {
            id: wheel
            property real travelled: 0
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onActiveChanged: if (!active) travelled = 0
            onWheel: (event) => {
                travelled += event.angleDelta.y
                while (travelled >= 120) {
                    NiriActions.focusWorkspaceUp()
                    travelled -= 120
                }
                while (travelled <= -120) {
                    NiriActions.focusWorkspaceDown()
                    travelled += 120
                }
            }
        }

        Repeater {
            model: root.workspaces

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
                    // Outside text is drawn as the characters it is: Qt reads a string as rich text when it looks like
                    // markup, which would let a sender restyle the bar or reference an image from a track title.
                    textFormat: Text.PlainText
                    anchors.centerIn: parent
                    // A named workspace shows its name; an unnamed one shows the index niri reports for
                    // it, which is what niri itself calls it.
                    text: capsule.modelData.name !== "" ? capsule.modelData.name
                                                        : String(capsule.modelData.idx)
                    color: capsule.focused ? "#12131a" : root.foreground
                    font.family: root.face
                    font.pixelSize: root.fontSize
                    font.weight: root.fontWeight
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
            // Outside text is drawn as the characters it is: Qt reads a string as rich text when it looks like
            // markup, which would let a sender restyle the bar or reference an image from a track title.
            textFormat: Text.PlainText
            anchors.verticalCenter: parent.verticalCenter
            text: "niri"
            color: root.muted
            font.family: root.face
            font.pixelSize: root.fontSize
            font.weight: root.fontWeight
        }
    }
}
