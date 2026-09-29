import QtQuick
import QuantumShell 1.0

// The control centre: the controls this shell has a real service behind, in a surface of their own on the
// overlay layer, anchored to the top-right corner below the bar.
//
// It is created by `src/app/ControlCenterHost.cpp` when `ControlCenterService.open` becomes true and destroyed
// when it becomes false, so it keeps no open/closed state: Escape writes the service's property and the host
// follows it. Everything here is a binding onto a service that owns the fact and a call to a method that
// performs it —
//
//   * the volume is `PipeWireService.volumePercent`, set with `setVolumePercent`, and the mute is
//     `PipeWireService.muted`, flipped with `toggleMute`;
//   * Do Not Disturb is `NotificationService.notificationDoNotDisturb`, written directly, as the notification
//     readout's own click does;
//   * the player is `MediaService`, and the three buttons are its `previous`, `playPause` and `next`, which
//     ask the player and let the property change that comes back redraw the state.
//
// The transport buttons are worded (Previous, Play / Pause, Next) rather than drawn with media-symbol glyphs: no
// bundled font has them, so a symbol is a fallback-font lookup at best and a missing-glyph box at worst, and the
// words are what every font can draw.
//
// Nothing is drawn that the shell cannot do: there is no brightness or Bluetooth control because there is no
// service behind either, and a slider that moved nothing would be a dead control. A section with nothing to act
// on says so — the volume reads a dash until there is a sink, the player buttons are dimmed and inert while no
// player exists.
//
// Its own namespace, `quantum-shell-control-center`, part of the frozen public interface like the bar's. Keyboard
// interactivity is on-demand: the panel takes keys when it is clicked and not before, so opening it from a key
// binding does not steal the keyboard from the window being worked in; Escape closes it once it has them, and
// the same `qsctl control-center toggle` that opened it closes it.
LayerShellWindow {
    id: panel

    layer: LayerShellWindow.Overlay
    anchors: LayerShellWindow.TopEdge | LayerShellWindow.RightEdge
    keyboardInteractivity: LayerShellWindow.OnDemandKeyboard
    layerNamespace: "quantum-shell-control-center"
    exclusiveZone: 0

    readonly property color foreground: Config.bar.colors.foreground
    readonly property color muted: Config.bar.colors.muted
    readonly property color accent: Config.bar.colors.accent
    readonly property string face: Config.bar.font.family
    readonly property int fontSize: Config.bar.font.size
    readonly property int fontWeight: Config.bar.font.weight

    // Sizes: a fixed width, and a height that is what the three sections hold. Anchored to two edges only, so the
    // client names both.
    readonly property int rowHeight: 40
    readonly property int chrome: 10 * 2 + 14 * 2
    width: 360
    height: chrome + dnd.height + 12 + volume.height + 12 + media.height

    color: "transparent"
    visible: false

    // What the volume slider's fill and the mute label say, as functions of the reading so the rules can be
    // exercised with the numbers a daemon sends.
    function volumeText(available, muted, percent) {
        if (!available)
            return "—"
        return muted ? "MUTE" : percent + "%"
    }
    function fillFor(available, percent) {
        return available ? Math.max(0, Math.min(100, percent)) / 100 : 0
    }
    // The percentage a press at `x` in a track `width` wide asks for.
    function percentAt(x, width) {
        return width <= 0 ? 0 : Math.round(Math.max(0, Math.min(1, x / width)) * 100)
    }
    function mediaText(available, title, artist, status) {
        if (!available)
            return "Nothing playing"
        return (status === "playing" ? "Playing: " : status === "paused" ? "Paused: " : "") + title
            + (artist !== "" ? " — " + artist : "")
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 10
        radius: 10
        color: "#1a1b26"

        Keys.onEscapePressed: ControlCenterService.open = false
        focus: true

        // Do Not Disturb: the switch the notification readout's left click also flips.
        Rectangle {
            id: dnd
            objectName: "controlDoNotDisturb"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 14
            height: panel.rowHeight
            radius: 6
            color: NotificationService.notificationDoNotDisturb ? Qt.alpha(panel.accent, 0.25) : "transparent"
            border.width: 1
            border.color: NotificationService.notificationDoNotDisturb ? panel.accent : panel.muted

            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 12
                textFormat: Text.PlainText
                text: NotificationService.notificationDoNotDisturb ? "Do Not Disturb: on" : "Do Not Disturb: off"
                color: panel.foreground
                font.family: panel.face
                font.pixelSize: panel.fontSize
                font.weight: panel.fontWeight
            }
            MouseArea {
                anchors.fill: parent
                onClicked: NotificationService.notificationDoNotDisturb = !NotificationService.notificationDoNotDisturb
            }
        }

        // Volume: a track that is clicked or dragged to a level, and the mute beside it.
        Item {
            id: volume
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: dnd.bottom
            anchors.topMargin: 12
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            height: 64

            Text {
                id: volumeLabel
                anchors.left: parent.left
                anchors.top: parent.top
                textFormat: Text.PlainText
                text: "Volume"
                color: panel.muted
                font.family: panel.face
                font.pixelSize: panel.fontSize
                font.weight: panel.fontWeight
            }
            Text {
                id: volumeValue
                objectName: "controlVolumeValue"
                anchors.right: muteButton.left
                anchors.rightMargin: 12
                anchors.top: parent.top
                textFormat: Text.PlainText
                text: panel.volumeText(PipeWireService.available, PipeWireService.muted, PipeWireService.volumePercent)
                color: panel.foreground
                font.family: panel.face
                font.pixelSize: panel.fontSize
                font.weight: panel.fontWeight
            }
            Rectangle {
                id: muteButton
                objectName: "controlMute"
                anchors.right: parent.right
                anchors.top: parent.top
                width: 64
                height: 24
                radius: 4
                opacity: PipeWireService.available ? 1 : 0.4
                color: PipeWireService.muted ? Qt.alpha(panel.accent, 0.25) : "transparent"
                border.width: 1
                border.color: PipeWireService.muted ? panel.accent : panel.muted
                Text {
                    anchors.centerIn: parent
                    textFormat: Text.PlainText
                    text: PipeWireService.muted ? "Unmute" : "Mute"
                    color: panel.foreground
                    font.family: panel.face
                    font.pixelSize: panel.fontSize - 1
                    font.weight: panel.fontWeight
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: PipeWireService.available
                    onClicked: PipeWireService.toggleMute()
                }
            }
            Item {
                id: track
                objectName: "controlVolumeTrack"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 24
                opacity: PipeWireService.available ? 1 : 0.4

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 6
                    radius: 3
                    color: panel.muted
                    opacity: 0.35
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * panel.fillFor(PipeWireService.available, PipeWireService.volumePercent)
                    height: 6
                    radius: 3
                    color: PipeWireService.muted ? panel.muted : panel.accent
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: PipeWireService.available
                    onPressed: mouse => PipeWireService.setVolumePercent(panel.percentAt(mouse.x, width))
                    onPositionChanged: mouse => {
                        if (pressed)
                            PipeWireService.setVolumePercent(panel.percentAt(mouse.x, width))
                    }
                }
            }
        }

        // The player being followed, and transport for it.
        Item {
            id: media
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: volume.bottom
            anchors.topMargin: 12
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            height: 24 + 8 + 32

            Text {
                id: mediaLabel
                objectName: "controlMediaText"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                textFormat: Text.PlainText
                elide: Text.ElideRight
                text: panel.mediaText(MediaService.available, MediaService.title, MediaService.artist,
                                      MediaService.playbackStatus)
                color: MediaService.available ? panel.foreground : panel.muted
                font.family: panel.face
                font.pixelSize: panel.fontSize
                font.weight: panel.fontWeight
            }
            Row {
                objectName: "controlTransport"
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                spacing: 12
                opacity: MediaService.available ? 1 : 0.4

                Repeater {
                    model: [
                        { name: "controlPrevious", label: "Previous", act: 0, wide: 84 },
                        { name: "controlPlayPause", label: "Play / Pause", act: 1, wide: 104 },
                        { name: "controlNext", label: "Next", act: 2, wide: 64 }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        objectName: modelData.name
                        width: modelData.wide
                        height: 32
                        radius: 4
                        color: "transparent"
                        border.width: 1
                        border.color: panel.muted
                        Text {
                            anchors.centerIn: parent
                            textFormat: Text.PlainText
                            text: modelData.label
                            color: panel.foreground
                            font.family: panel.face
                            font.pixelSize: panel.fontSize
                            font.weight: panel.fontWeight
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: MediaService.available
                            onClicked: {
                                if (modelData.act === 0)
                                    MediaService.previous()
                                else if (modelData.act === 1)
                                    MediaService.playPause()
                                else
                                    MediaService.next()
                            }
                        }
                    }
                }
            }
        }
    }
}
