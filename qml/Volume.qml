import QtQuick
import QuantumShell 1.0

// The output volume, and the two gestures that change it.
//
// Every value here comes from `PipeWireService`, which owns the daemon connection and does the writing —
// nothing in this file reads a socket, knows a dB, or decides what the new volume is. That split is the
// boundary rule: the reading, the step and the write are the service's, and what a readout looks like is
// this file's.
//
// **The percentage is not the linear factor the daemon reports.** `percentFromLinear` in
// `src/audio/AudioVolume.cpp` carries the measurement that establishes the convention — `wpctl
// get-volume` prints 0.35 for a sink whose Props say 0.042872, which is 0.35 cubed — and the service
// hands QML the same number the rest of the desktop would show. Nothing is converted here, which is why
// this widget has no arithmetic in it at all.
//
// **A click toggles mute; the wheel steps.** Both are requests rather than local changes: what is drawn
// afterwards is the daemon's own answer, so the bar can never disagree with the mixer, and a volume
// changed by any other tool on the desktop appears here through the same path. A click asks for the mute
// state to be flipped and does not assume the flip happened — it is the daemon that decides, and the
// reading that comes back is what is drawn.
//
// **A wheel step does not unmute.** `wpctl set-volume` on a muted sink leaves it muted; a wheel that also
// lifted the mute would be a second effect nobody asked for, and it would make "quieter" mean "louder
// enough to hear". The service carries the mute state through the write for the same reason, and the step
// itself is the configuration's rather than a number written here — how far a notch moves the volume is the
// person's setting, and the widget is the last place it should be decided. There are two of them,
// `[bar.audio].step_percent` and `step_decibels`, and which applies is the unit below: a notch is a distance
// in the thing being read. This file sends the gesture and nothing else — it does not say how far it goes,
// and it could not, because the value a notch moves is the daemon's.
//
// **Which unit the number is drawn in is the configuration's.** `[bar.audio].volume_scale` names one of two,
// and both are the *same* reading the daemon owns, converted from the same linear factor in the service:
// `percent` is the desktop's convention — the cube root of that factor, which is what `wpctl get-volume`
// prints — and `decibel` is the physical gain, `20 * log10`, which is what the desktop's other volume tool
// prints beside the percentage. The conversion is not done here; this file chooses which of the two published
// numbers to draw and how to spell it. Because the same token is also what the service measures a notch in,
// a person reading this readout is moved by a distance in the unit they are reading: one point of the
// percentage, or one decibel — which, at the bottom of the range, is the difference between a notch they can
// see and one that moves the volume 18 dB, because a percentage point is 18.06 dB at 1%.
//
// The empty state is a dash and it is driven by a real flag. `available` is false while no Props param
// has arrived for the sink the metadata names as the default — before the daemon answers, while the
// daemon is away, and when the metadata names no sink at all. There is no fallback reading: a volume the
// shell could not read is a volume it does not show, and the reason it could not is a record on the
// `quantum.shell.audio` category.
//
// The widget is an `Item` rather than a `Row` for the reason `SystemMonitor.qml` is: the group it is
// declared in gives it the bar's full height, and it is the widget's job to centre its own content in
// that height. Nothing here carries a position — `Bar.qml` decides which group it is in.
Item {
    id: root

    // Named the way the other widgets are, so the group this widget landed in can be read without counting
    // the bar's children.
    objectName: "volume"

    property color foreground
    property color muted
    property color accent
    property string face

    // The configuration flag, read once so that the arrangement and the renderer cannot disagree about
    // whether this widget is there. `visible` is what stops it being drawn *and* hit-tested, and it is also
    // what takes its room out of the bar: the group is a `Row`, and a positioner lays out no space for an
    // invisible child (see `CapsuleGroup.qml`, where that is stated as its own behaviour). The width below is
    // the widget's own, which the widget is the one to declare — hidden, it should report the room it takes,
    // which is none, rather than a width nothing occupies. `bar-interaction-test` asserts both the group
    // reclaiming the room and this widget reporting zero.
    readonly property bool shown: Config.bar.audio.showVolume

    // The unit the readout is drawn in, read once for the reason `shown` is — nothing else in this file
    // consults the configuration, so what is drawn and what the file says cannot disagree — and named so that
    // a token which never reached this widget is a failure rather than a readout that happens to look right:
    // `bar-interaction-test` reads this property back after writing each token into a real configuration file.
    readonly property string scale: Config.bar.audio.volumeScale

    width: shown ? content.width : 0
    visible: shown

    // What the readout draws, as a function of the readings rather than of the service singleton.
    //
    // Stated in its inputs so the rule can be exercised with the numbers a daemon sends: `bar-interaction-test`
    // compares its answers with literals for both units, for silence, for a mute and for the state where there
    // is no reading at all. Everything on the far side of the socket needs a daemon, and a rendering rule that
    // could only be tested with one would be a rule nothing tests — while the *binding* below is still what
    // says which numbers it is given, and that is read back off the loaded widget.
    function textFor(scale, available, muted, percent, decibels) {
        if (!available)
            return "—"
        // Three states, and the middle one is not a volume: a muted sink's decibels are still the daemon's
        // number, but drawing it would say the sink is playing at that level, which is the one thing a mute
        // means it is not.
        if (muted)
            return "MUTE"
        switch (scale) {
        case "decibel":
            // One decimal place, and that is the unit's own step read back: a notch moves `step_decibels`,
            // whose floor is a tenth of a dB because a shorter step would be one this readout could not show.
            // Silence is the daemon's zero factor, whose decibel value is negative infinity, and it is drawn as
            // that symbol rather than as a number — a floor would be a volume the sink is not playing at.
            return Number.isFinite(decibels) ? decibels.toFixed(1) + " dB" : "-∞ dB"
        default:
            return percent + "%"
        }
    }

    // The service's readings, read in one expression so the two numbers handed to the rule above describe one
    // reading rather than two: which unit is drawn is the configuration's, and where the numbers came from is
    // this line.
    function reading() {
        return textFor(scale, PipeWireService.available, PipeWireService.muted,
                       PipeWireService.volumePercent, PipeWireService.volumeDecibels)
    }

    Row {
        id: content
        anchors.verticalCenter: parent.verticalCenter
        spacing: 5

        Text {
            text: "VOL"
            color: root.muted
            font.family: root.face
            font.pixelSize: 12
        }

        Text {
            objectName: "volumeValue"
            text: root.reading()
            // Muted is drawn in the accent colour, so the state is legible at a glance rather than only
            // readable by parsing the word: the colour and the word are the same fact, not two.
            color: PipeWireService.available && PipeWireService.muted ? root.accent : root.foreground
            font.family: root.face
            font.pixelSize: 12
        }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton
        // Nothing is decided here: the service asks the daemon to flip the mute state and the new reading
        // arrives as an event. Clicking while there is no reading would be a write with no state to write
        // from, which the service refuses and logs.
        onClicked: PipeWireService.toggleMute()
    }

    // The wheel over the readout. Unlike the workspace strip there is no compositor binding to mirror here
    // — this is the shell's own gesture — so the convention is the one every desktop uses: up is louder.
    // `TouchPad` is named explicitly because it is not the default device set, and a touchpad that sends
    // scroll phases rather than wheel ticks would otherwise be ignored.
    //
    // One gap, stated rather than hidden: a continuous touchpad scroll can send several events per gesture
    // and this handler acts on every one of them, so a flick moves several steps where a mouse wheel moves
    // one. A cooldown is where that is fixed, and it belongs with the other gestures when the shell has one.
    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (event) => {
            if (event.angleDelta.y < 0)
                PipeWireService.stepVolume(-1)
            else if (event.angleDelta.y > 0)
                PipeWireService.stepVolume(1)
        }
    }
}
