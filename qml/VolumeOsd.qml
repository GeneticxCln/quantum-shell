import QtQuick
import QuantumShell 1.0

// The on-screen display for a volume change: a small panel on the overlay layer, centred above the bottom
// edge, drawn from the same reading the bar's volume readout is.
//
// There is one of these per output, created by `src/app/OsdHost.cpp` when the volume or the mute of the sink
// being followed is adjusted and destroyed when `[bar.osd] timeout_ms` has passed. Nothing here is passed in:
// every value is bound to `PipeWireService`, so a further adjustment while the panel is up redraws it from the
// daemon's own answer and the host only has to restart its clock — which is why this file has no signal and
// no property a host would set.
//
// It is an overlay rather than the top layer, because it appears over whatever is on screen, full-screen
// windows included, and goes without a trace; the namespace is its own (`quantum-shell-osd`, part of the
// frozen public interface like the bar's). It reserves nothing and takes no keyboard focus: a display that
// moved a window or stole the keys of the person who is adjusting the volume would be the opposite of one.
//
// The unit is the configuration's `[bar.audio] volume_scale` — the same token the bar's readout is drawn in
// and the wheel's step is measured in, so one number means one unit everywhere. The bar below is the
// percentage in both cases: it is a position within the sink's range, and the percentage is the desktop's
// own convention for where in the range a volume is.
LayerShellWindow {
    id: osdWindow

    layer: LayerShellWindow.Overlay
    // Anchored to the bottom edge alone: the compositor centres a surface along an axis it is not anchored
    // to, and the client names both sizes, so the panel is neither stretched nor left to be proposed the
    // whole output.
    anchors: LayerShellWindow.BottomEdge
    keyboardInteractivity: LayerShellWindow.NoKeyboard
    layerNamespace: "quantum-shell-osd"
    exclusiveZone: 0

    // The panel is 72 high and sits above a transparent strip, because the surface is pinned to the bottom edge
    // and the gap between the panel and the edge is the surface's own bottom: 60 px of nothing, so the panel is
    // clear of a dock or of the edge without a margin, which QML has no value type for.
    width: 300
    height: 132
    // Only the panel takes the pointer: the strip beneath it is empty, and a click on empty space should reach
    // the window under it rather than being swallowed by a display that is about to go.
    inputRect: Qt.rect(0, 0, 300, 72)

    readonly property color foreground: Config.bar.colors.foreground
    readonly property color muted: Config.bar.colors.muted
    readonly property color accent: Config.bar.colors.accent
    readonly property string face: Config.bar.font.family
    readonly property int fontSize: Config.bar.font.size
    readonly property int fontWeight: Config.bar.font.weight

    // What the panel says for a reading, as a function of the reading so the rule can be checked with the
    // numbers a daemon sends. The same three states the bar's readout has, for the same reason: a muted sink is
    // not playing at its stored level, and silence in decibels is negative infinity rather than a number.
    function valueText(scale, available, muted, percent, decibels) {
        if (!available)
            return "—"
        if (muted)
            return "MUTE"
        if (scale === "decibel")
            return Number.isFinite(decibels) ? decibels.toFixed(1) + " dB" : "-∞ dB"
        return percent + "%"
    }

    // The bar's fill: the percentage while there is a level to show, nothing when muted or unread.
    function fillFor(available, muted, percent) {
        return available && !muted ? Math.max(0, Math.min(100, percent)) / 100 : 0
    }

    readonly property string valueLabel: valueText(Config.bar.audio.volumeScale, PipeWireService.available,
                                                    PipeWireService.muted, PipeWireService.volumePercent,
                                                    PipeWireService.volumeDecibels)
    readonly property real fill: fillFor(PipeWireService.available, PipeWireService.muted,
                                         PipeWireService.volumePercent)

    color: "transparent"
    visible: false

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 6
        height: 72
        radius: 8
        color: "#1a1b26"

        Column {
            anchors.centerIn: parent
            width: parent.width - 32
            spacing: 8

            Row {
                width: parent.width
                Text {
                    id: label
                    textFormat: Text.PlainText
                    text: "Volume"
                    color: osdWindow.muted
                    font.family: osdWindow.face
                    font.pixelSize: osdWindow.fontSize
                    font.weight: osdWindow.fontWeight
                    width: parent.width / 2
                }
                Text {
                    id: value
                    objectName: "osdValue"
                    textFormat: Text.PlainText
                    text: osdWindow.valueLabel
                    horizontalAlignment: Text.AlignRight
                    color: osdWindow.foreground
                    font.family: osdWindow.face
                    font.pixelSize: osdWindow.fontSize
                    font.weight: osdWindow.fontWeight
                    width: parent.width / 2
                }
            }

            Item {
                width: parent.width
                height: 6

                Rectangle {
                    anchors.fill: parent
                    radius: 3
                    color: osdWindow.muted
                    opacity: 0.35
                }

                Rectangle {
                    objectName: "osdFill"
                    width: parent.width * osdWindow.fill
                    height: parent.height
                    radius: 3
                    color: osdWindow.accent
                }
            }
        }
    }
}
