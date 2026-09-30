import QtQuick
import QuantumShell 1.0

// The time, from the system clock.
//
// This is one of the shell's two scheduled wake-ups that are neither a retry nor an animation frame, and it
// is worth stating why rather than leaving it to look like the polling QUANTUM_SHELL.md forbids. The
// passage of time has no event source: nothing notifies a listener that the minute changed, so a clock
// either asks or it is wrong. What is forbidden is a timer re-reading state the shell has a subscription
// for, and this reads none. The other one is the system status, which reads /proc and cannot avoid asking
// either — see `src/system/SysMonService.h`, where that reading's reason and its limits are written down.
//
// It asks as little as it can. The timer is single-shot and re-armed for the exact moment the displayed
// text would differ, so the shell wakes once a minute instead of sixty times, and the minute shown is
// never stale by a second either.
//
// The boundary it computes is on the monotonic clock's terms, which stops counting during a suspend and knows
// nothing of the clock being set or the zone being changed, so a resume or a change would leave the time wrong
// for up to a minute. `ClockService` is the event source for exactly those two cases (a real-time clock that was
// set, a timezone that changed) and this reads the time and re-arms when it says so.
Text {
    id: clock
    // Outside text is drawn as the characters it is: Qt reads a string as rich text when it looks like
    // markup, which would let a sender restyle the bar or reference an image from a track title.
    textFormat: Text.PlainText

    property string face
    // From `[bar.font]` through `Bar.qml`, one up from the body size because the time is the readout a person
    // reads most and the only one on the bar without a label beside it. The default is what this component
    // drew before the table existed.
    property int fontSize: 13
    property int fontWeight: 400

    // Named the way the strip's row is, so the group a widget landed in can be read without counting the
    // bar's children.
    objectName: "clock"

    // The clock is given its height by the group it is declared in (`Bar.qml`), the way every widget is, and
    // it is the widget's job to put its own content in the middle of the height it was given: a `Text` taller
    // than its glyphs renders from the top, so without this the time would ride up against the top edge of a
    // 32-pixel bar while the capsules beside it sat centred.
    verticalAlignment: Text.AlignVCenter

    color: "#c8cad8"
    font.family: face
    font.pixelSize: fontSize
    font.weight: fontWeight
    font.letterSpacing: 0.5

    function refresh() {
        text = Qt.formatDateTime(new Date(), "HH:mm")
    }

    function armNextChange() {
        const now = new Date()
        const intoMinute = now.getSeconds() * 1000 + now.getMilliseconds()
        timer.interval = Math.max(1, 60000 - intoMinute)
        timer.restart()
    }

    Timer {
        id: timer
        repeat: false
        onTriggered: {
            clock.refresh()
            clock.armNextChange()
        }
    }

    // A click on the time opens the calendar, and another closes it: the panel's state is the service's.
    MouseArea {
        anchors.fill: parent
        onClicked: CalendarService.toggle()
    }

    Connections {
        target: ClockService
        function onClockChanged() {
            clock.refresh()
            clock.armNextChange()
        }
    }

    Component.onCompleted: {
        refresh()
        armNextChange()
    }
}
