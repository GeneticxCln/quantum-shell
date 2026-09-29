// The media player readout: track title, artist, and playback status from the active MPRIS player.
//
// Reads from MediaService, which follows whichever player last sent a PropertiesChanged. Shows nothing
// when no player exists or when the active player has no track loaded — not a dash, because "no media"
// is a complete reading rather than a failure.
import QtQuick 2.15
import QuantumShell 1.0

Item {
    id: root
    objectName: "media"

    // Palette and typography from the bar
    required property color foreground
    required property color muted
    required property color accent
    required property string face
    // From `[bar.font]` through `Bar.qml`, two up from the body size because this readout draws prose. The
    // default is what this component drew before the table existed.
    property int fontSize: 14
    property int fontWeight: 400

    // Configured visibility
    property bool showMedia: Config.bar.media.showMedia

    // Only visible when both: configured to show AND there's a track to display
    visible: showMedia && MediaService.available
    width: visible ? content.implicitWidth : 0
    implicitHeight: content.implicitHeight

    Row {
        id: content
        spacing: 6
        anchors.centerIn: parent

        // Playback status indicator
        Text {
            // Outside text is drawn as the characters it is: Qt reads a string as rich text when it looks like
            // markup, which would let a sender restyle the bar or reference an image from a track title.
            textFormat: Text.PlainText
            id: statusIcon
            text: {
                switch (MediaService.playbackStatus) {
                case "playing":
                    return "▶";
                case "paused":
                    return "⏸";
                default:
                    return "⏹";
                }
            }
            color: root.muted
            font.family: root.face
            font.pixelSize: root.fontSize
            font.weight: root.fontWeight
            anchors.verticalCenter: parent.verticalCenter
        }

        // Track info: "Title - Artist" or just title if no artist
        Text {
            // Outside text is drawn as the characters it is: Qt reads a string as rich text when it looks like
            // markup, which would let a sender restyle the bar or reference an image from a track title.
            textFormat: Text.PlainText
            id: trackText
            text: {
                const title = MediaService.title || "";
                const artist = MediaService.artist || "";
                if (title && artist) {
                    return title + " - " + artist;
                }
                return title || artist || "—";
            }
            color: root.foreground
            font.family: root.face
            font.pixelSize: root.fontSize
            font.weight: root.fontWeight
            anchors.verticalCenter: parent.verticalCenter

            // Truncate long track names
            elide: Text.ElideRight
            maximumLineCount: 1
            width: Math.min(implicitWidth, 300)
        }
    }
}
