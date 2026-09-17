import QtQuick
import QuantumShell 1.0

// The network connection the machine is on, as NetworkManager reports it.
//
// Every value here comes from `NetworkService`, which owns the bus connection and follows the daemon's own
// signals — nothing in this file opens a socket, asks a question or decides when a reading happens. That split
// is the boundary rule the other two readouts follow as well: the reading is the service's, and what a readout
// looks like is this file's.
//
// *Whether* each part is drawn is the configuration's: `[bar.network]`'s `show_status`, `show_name` and
// `show_strength`, reaching this file as `Config.bar.network`. All three are read once, and none of them is
// invented here — the schema refuses a key it does not read, so the three names below are its names.
//
// **What the readout draws, and the one state that is not a reading.** `available` is false while
// NetworkManager has not answered and while it is not on the bus, and that is a dash — never `offline`, which
// would be claiming a fact about the network from the daemon's silence. A daemon that says *nothing is
// connected* has answered, and that is drawn as `offline`; a daemon that says *this is being worked on* is drawn
// as `connecting`, which is the state a bar must not fold into `connected` — a machine that has a link and no
// routing is not on the network yet.
//
// **The signal quality is a number only for wireless connections.** `hasStrength` is a real flag, so a wired
// connection draws its name and nothing else and a wireless one at the edge of range draws `0%` rather than a
// dash: zero is a reading, and the flag is what tells the two apart. It is the same rule the volume readout
// applies to a muted sink and the system status applies to a CPU percentage that does not exist yet.
//
// **A connection the daemon is unhappy about is drawn in the urgent colour.** NetworkManager reports
// `connectivity`: `full` is a network that works, `portal` is one behind a captive portal and `limited` is one
// that cannot reach everything. Those two are drawn as a word after the rest — the daemon's verdict, spelled
// rather than coloured alone, because a colour is not readable by everyone and the word is the fact. Absent and
// `full` draw the same nothing, and that is deliberate: a marker's job is to flag trouble, and "I have not
// heard" is not trouble.
//
// The widget is an `Item` rather than a `Row` for the reason its two neighbours are: the group it is declared
// in gives it the bar's full height, and centring its own content in that height is the widget's job. Nothing
// here carries a position — `Bar.qml` decides which group it is in.
Item {
    id: root

    // Named the way the other widgets are, so the group this widget landed in can be read without counting the
    // bar's children.
    objectName: "network"

    property color foreground
    property color muted
    property color urgent
    property string face

    // The three configuration flags, read once each so that what is drawn and what the file says cannot
    // disagree, and named so that a value which never reached this widget is a failure rather than a readout
    // that happens to look right: `bar-interaction-test` reads them back after writing each into a real
    // configuration file. `visible` is what stops the readout being drawn *and* takes its room out of the bar,
    // and the width below is the widget's own, so a hidden readout reports the room it takes, which is none.
    readonly property bool shown: Config.bar.network.showStatus
    readonly property bool naming: Config.bar.network.showName
    readonly property bool strengthShown: Config.bar.network.showStrength

    width: shown ? content.width : 0
    visible: shown

    // What the readout draws, as a function of the reading rather than of the service singleton.
    //
    // Stated in its inputs so the rule can be exercised with the tokens and numbers the daemon sends —
    // `bar-interaction-test` compares its answers with literals for every state, for a wireless and a wired
    // connection, for a signal of zero and for the state where there is no reading at all. Everything on the far
    // side of the bus needs a daemon, and a rendering rule that could only be tested with one would be a rule
    // nothing tests; the *binding* below is still what says which values it is given, and that is read back off
    // the loaded widget.
    function textFor(available, state, kind, name, interfaceName, hasStrength, strength, connectivity,
                     showName, showStrength) {
        if (!available)
            return "—"
        // The daemon's own state, one branch each: `disconnected` is a machine that is deliberately offline and
        // `connecting` is one whose link is up and whose routing is not, which is the difference between a bar
        // that tells the truth and one that says "connected" the moment anything at all is happening.
        if (state === "disconnected")
            return "offline"
        if (state === "connecting")
            return "connecting"
        // Connected. The pieces the configuration asks for, in the order a person reads them: what the connection
        // is called, then how good the signal carrying it is, then the daemon's verdict on it if there is one to
        // give.
        const parts = []
        if (showName && name !== "")
            parts.push(name)
        if (kind === "wifi" && showStrength && hasStrength)
            parts.push(strength + "%")
        if (parts.length === 0)
            // Nothing configured is drawable — no name, or a name the daemon has not published, and no signal
            // quality — so the readout falls back to what is left of the connection rather than to an empty bar:
            // its interface when the daemon named one, and the kind of device otherwise. Both are facts the
            // daemon gave, where a stand-in would be a word this file invented.
            parts.push(interfaceName !== "" ? interfaceName : kind)
        if (connectivity === "portal" || connectivity === "limited")
            // The daemon's own word for it, drawn rather than only coloured: the colour below repeats this fact
            // instead of carrying it alone.
            parts.push(connectivity)
        return parts.join(" ")
    }

    // Whether the connection is one the daemon has flagged. A network behind a captive portal and one that
    // cannot reach everything are both "connected" and both not working, which is the case the colour exists for.
    function troubled(connectivity) {
        return connectivity === "portal" || connectivity === "limited"
    }

    // The service's readings, read in one expression so that everything handed to the rule above describes one
    // reading rather than several: where the values came from is this line and nowhere else in the file.
    function reading() {
        return textFor(NetworkService.available, NetworkService.state, NetworkService.deviceKind,
                       NetworkService.connectionName, NetworkService.interfaceName, NetworkService.hasStrength,
                       NetworkService.strength, NetworkService.connectivity, naming, strengthShown)
    }

    Row {
        id: content
        anchors.verticalCenter: parent.verticalCenter
        spacing: 5

        Text {
            text: "NET"
            color: root.muted
            font.family: root.face
            font.pixelSize: 12
        }

        Text {
            objectName: "networkValue"
            text: root.reading()
            // The colour repeats what the text says rather than saying something on its own: a connection the
            // daemon flagged as not working is drawn in the urgent colour, and everything else is drawn normally.
            // There is no third colour, because there is no third state — an unread daemon is a dash and a
            // disconnected one is a word, and neither is urgent.
            color: NetworkService.available && root.troubled(NetworkService.connectivity)
                       ? root.urgent : root.foreground
            font.family: root.face
            font.pixelSize: 12
        }
    }

    // No gesture, deliberately. The volume readout mutes on a click and steps on the wheel because those are
    // requests the shell can make of the daemon that owns the value; what a click on this readout would do is
    // open a control centre, and this shell has none. A click that did nothing would be a gesture that exists
    // only to look like one, so there is not one. It arrives with the control centre, and it will be a
    // `Q_INVOKABLE` on the service when it does.
}
