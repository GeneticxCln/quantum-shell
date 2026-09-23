// The notification readout: the most recent notification the shell received, from the daemon the shell
// is.
//
// Reads from NotificationService, which owns `org.freedesktop.Notifications` on the session bus — or
// joins the queue for it, which is not the same thing. A summary drawn while another daemon holds the
// name would be a claim about a life of the daemon that is not this one, so the readout is drawn only
// while the service reports that the shell is the daemon, is a dash when it is the daemon and has been
// sent nothing yet, and is not drawn at all when the shell is not the daemon or the configuration turns
// it off.
import QtQuick 2.15
import QuantumShell 1.0

Item {
    id: root
    objectName: "notifications"

    // Palette and typography from the bar
    required property color foreground
    required property color muted
    required property string face

    // Configured visibility
    property bool showNotifications: Config.bar.notifications.showNotifications

    // Visible only when configured to show AND the shell is the notification daemon. The second half is
    // not the same question as the first: the name is taken either way, so hiding the readout does not
    // stand the daemon down, and being the daemon does not force the readout on a person who turned it
    // off. The flag is also what the reading is read against, because it is the bus's own answer about who
    // owns the name: a queued shell holds no summary, and a shell the bus has just handed the name publishes
    // it, so the text and the flag move together.
    visible: showNotifications && NotificationService.notificationAvailable
    width: visible ? content.implicitWidth : 0
    implicitHeight: content.implicitHeight

    Row {
        id: content
        spacing: 6
        anchors.centerIn: parent

        // The sender's application name, which is its own statement about who sent this. Drawn in the
        // muted colour because it is attribution rather than the message.
        //
        // Named the way the other readouts' texts are, so `bar-interaction-test` can read what this widget
        // draws without knowing which child it is — the same reason `volumeValue` and `networkValue` are named.
        Text {
            id: applicationText
            objectName: "notificationApplication"
            text: NotificationService.notificationApplication
            color: root.muted
            font.family: root.face
            font.pixelSize: 14
            anchors.verticalCenter: parent.verticalCenter
            visible: text.length > 0
        }

        // The summary, which is the subject line a person wrote. The dash is the service holding no
        // summary while the shell *is* the daemon, which is a desktop that has sent it nothing yet. The
        // other empty state — the shell not being the daemon — draws nothing at all, so this text is
        // never the one making that claim.
        Text {
            id: summaryText
            objectName: "notificationSummary"
            text: NotificationService.notificationSummary.length > 0 ? NotificationService.notificationSummary : "—"
            color: root.foreground
            font.family: root.face
            font.pixelSize: 14
            anchors.verticalCenter: parent.verticalCenter

            // A long subject is truncated rather than allowed to push the clock off the bar.
            elide: Text.ElideRight
            maximumLineCount: 1
            width: Math.min(implicitWidth, 240)
        }
    }
}
