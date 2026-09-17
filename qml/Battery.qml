import QtQuick
import QuantumShell 1.0

// The charge UPower reports: what is left, whether it is going in or out, how long the daemon says that will
// take, and whether the daemon is worried.
//
// Every value here comes from `BatteryService`, which owns the bus connection and follows the daemon's own
// signals — nothing in this file opens a socket, asks a question or decides when a reading happens. That split
// is the boundary rule the other three readouts follow as well: the reading is the service's, and what a readout
// looks like is this file's.
//
// *Whether* each part is drawn is the configuration's: `[bar.battery]`'s `show_status`, `show_percentage` and
// `show_time`, reaching this file as `Config.bar.battery`. All three are read once, and none of them is invented
// here — the schema refuses a key it does not read, so the three names below are its names.
//
// **The one state that draws nothing is the daemon answering that there is no battery.** Three cases, and they
// are three different statements:
//
//   * `!available` — UPower has not answered, or is not on the bus — is a dash. "I have not heard" is a fact
//     about the shell, and every other readout on this bar says it the same way.
//   * `available && !present` is *nothing at all*, not a dash and not a zero: the daemon has answered, and the
//     answer is that this machine has no battery to report on. Every desktop is this case, and drawing `0%` or
//     a dash there would be inventing a battery — one flat, or one that cannot be read.
//   * `available && present` is the reading, and an absent *part* of it is still an absence: a level the daemon
//     did not publish leaves the number out rather than drawing zero.
//
// That is why `drawn` is a binding on the daemon's answer rather than on the reading alone, and why it is
// expressed through a function: the rule is exercised with literals by `bar-interaction-test`, which is the only
// way a case that needs a laptop, a desktop and a daemon whose battery is missing can be a case at all.
//
// **The state is spelled, and the colour repeats it.** UPower reports a `State` — charging, discharging, full,
// empty, two pending states, or unknown — and a `WarningLevel` of its own: `low`, `critical` and `action` are the
// daemon's *judgement* about the battery, computed from thresholds in its own configuration. The readout draws
// the word for the states a person needs told apart — `charging`, `fully-charged`, `empty` and the two pending
// ones — and says nothing extra for `discharging`, which is what a laptop spends the day doing, and nothing at
// all for `unknown`, which is the daemon not having decided. A warning supersedes that word, because it is the
// more urgent fact and the daemon's own spelling of it; the colour then repeats the word rather than carrying it
// alone, which is the same rule the network readout applies to a captive portal.
//
// The widget is an `Item` rather than a `Row` for the reason its three neighbours are: the group it is declared
// in gives it the bar's full height, and centring its own content in that height is the widget's job. Nothing
// here carries a position — `Bar.qml` decides which group it is in.
Item {
    id: root

    // Named the way the other widgets are, so the group this widget landed in can be read without counting the
    // bar's children.
    objectName: "battery"

    property color foreground
    property color muted
    property color urgent
    property string face

    // The three configuration flags, read once each so that what is drawn and what the file says cannot
    // disagree, and named so that a value which never reached this widget is a failure rather than a readout
    // that happens to look right: `bar-interaction-test` reads them back after writing each into a real
    // configuration file.
    readonly property bool shown: Config.bar.battery.showStatus
    readonly property bool levelShown: Config.bar.battery.showPercentage
    readonly property bool timeShown: Config.bar.battery.showTime

    // Whether the readout is drawn, as a function of the daemon's answer. `visible` is what stops it being drawn
    // *and* hit-tested, and the width below is the widget's own, so a readout that is not drawn reports the room
    // it takes, which is none.
    readonly property bool drawn: shown && shouldDraw(BatteryService.available, BatteryService.present)

    width: drawn ? content.width : 0
    visible: drawn

    // The daemon's answer about whether there is a battery here, as the widget's rule rather than as an
    // expression: a machine with no battery draws nothing at all, while a daemon that has not answered draws a
    // dash. Stated in its inputs so the four combinations can be compared with literals — the state a laptop
    // makes visible, the state this desktop is in, and the two that need a daemon to be stopped.
    function shouldDraw(available, present) {
        return !available || present
    }

    // The word for a state, or nothing when the state is the ordinary one. `discharging` is what a laptop does
    // all day and `unknown` is the daemon not having decided, so neither is spelled; the rest are the facts a
    // person is looking at the readout to learn, in the daemon's own words.
    function stateWord(state) {
        switch (state) {
        case "charging":
            return "charging"
        case "fully-charged":
            return "fully-charged"
        case "empty":
            return "empty"
        case "pending-charge":
            return "pending-charge"
        case "pending-discharge":
            return "pending-discharge"
        default:
            return ""
        }
    }

    // The daemon's warning level as the word for it, or nothing. These three are the levels that mean the daemon
    // is telling a person something about the battery rather than reporting where its charge is going; `none`
    // and `discharging` are not trouble, and `unknown` is the daemon not having decided, which is not trouble
    // either.
    function warningWord(warning) {
        switch (warning) {
        case "low":
            return "low"
        case "critical":
            return "critical"
        case "action":
            return "action"
        default:
            return ""
        }
    }

    // Whether the daemon has flagged the battery. The colour below repeats the word this returns true for.
    function troubled(warning) {
        return warningWord(warning) !== ""
    }

    // What the readout draws, as a function of the reading rather than of the service singleton. Stated in its
    // inputs so the rule can be exercised with the tokens and numbers UPower sends — `bar-interaction-test`
    // compares its answers with literals for every state, for a warning, for both ends of the charge, for a clock
    // the daemon published as zero and for the two ways there is no reading at all.
    function textFor(available, present, hasPercentage, percentage, state, warning, hasTime, timeText,
                     showPercentage, showTime) {
        if (!available)
            return "—"
        if (!present)
            return ""
        const parts = []
        if (showPercentage && hasPercentage)
            parts.push(percentage + "%")
        if (showTime && hasTime)
            parts.push(timeText)
        // One status word and not two: the daemon's warning when it has one, and the state otherwise. Drawing
        // both would say `9% 0:20 low discharging`, where `low` is the daemon's judgement and `discharging` is
        // the direction it was already implied by — a time to empty rather than to full.
        const status = warningWord(warning) !== "" ? warningWord(warning) : stateWord(state)
        if (status !== "")
            parts.push(status)
        if (parts.length === 0)
            // Nothing configured is drawable and the state is the ordinary one, so the readout falls back to the
            // state itself rather than to an empty value beside a label: `discharging` and `unknown` are facts the
            // daemon gave, where an empty string would be a readout that looks broken.
            parts.push(state)
        return parts.join(" ")
    }

    // The service's readings, read in one expression so that everything handed to the rule above describes one
    // reading rather than several: where the values came from is this line and nowhere else in the file.
    function reading() {
        return textFor(BatteryService.available, BatteryService.present, BatteryService.hasPercentage,
                       BatteryService.percentage, BatteryService.state, BatteryService.warning,
                       BatteryService.hasTimeRemaining, BatteryService.timeRemaining, levelShown, timeShown)
    }

    Row {
        id: content
        anchors.verticalCenter: parent.verticalCenter
        spacing: 5

        Text {
            text: "BAT"
            color: root.muted
            font.family: root.face
            font.pixelSize: 12
        }

        Text {
            objectName: "batteryValue"
            text: root.reading()
            // The colour repeats what the text says rather than saying something on its own: a battery the daemon
            // has flagged is drawn in the urgent colour beside the word for that flag. There is no third colour,
            // because there is no third state — a daemon that has not answered is a dash and one that reports no
            // battery draws nothing, and neither is urgent.
            color: BatteryService.available && root.troubled(BatteryService.warning)
                       ? root.urgent : root.foreground
            font.family: root.face
            font.pixelSize: 12
        }
    }

    // No gesture, deliberately. The volume readout mutes on a click because that is a request the shell can make
    // of the daemon that owns the value; what a click on this readout would do is open a power panel, and this
    // shell has none. A click that did nothing would be a gesture that exists only to look like one, so there is
    // not one. It arrives with the control centre, and it will be a `Q_INVOKABLE` on the service when it does.
}
