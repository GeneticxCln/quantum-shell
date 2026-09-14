import QtQuick

// The time, from the system clock.
//
// This is the shell's one scheduled wake-up that is not a retry or an animation frame, and it is worth
// stating why rather than leaving it to look like the polling QUANTUM_SHELL.md forbids. The passage of
// time has no event source: nothing notifies a listener that the minute changed, so a clock either asks
// or it is wrong. What is forbidden is a timer re-reading niri's state, and this reads none.
//
// It asks as little as it can. The timer is single-shot and re-armed for the exact moment the displayed
// text would differ, so the shell wakes once a minute instead of sixty times, and the minute shown is
// never stale by a second either.
Text {
    id: clock

    property string face

    color: "#c8cad8"
    font.family: face
    font.pixelSize: 13
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

    Component.onCompleted: {
        refresh()
        armNextChange()
    }
}
