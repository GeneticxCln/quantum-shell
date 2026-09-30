import QtQuick
import QuantumShell 1.0

// The calendar: a month, as a grid, with today marked, in a surface of its own on the overlay layer at the
// top-right corner below the bar — where the clock that opens it is.
//
// It is created by `src/app/CalendarHost.cpp` when `CalendarService.open` becomes true and destroyed when it becomes
// false, so it keeps no open/closed state: Escape and a click on the clock write the service's property and the host
// follows it. What it draws is the system's date and nothing else — there are no events, because nothing in this shell
// reads a calendar of events, and a grid that pretended to would be showing things that are not there. "Today" is the date when the panel was
// opened: the panel lives as long as a person looks at it, so it needs no timer to stay right, and the next opening
// reads the date again.
//
// The month shown is this component's own UI state (the previous and next buttons move it), which is presentation
// and belongs in QML. Which weekday a week starts on is the locale's (`Qt.locale().firstDayOfWeek`), so there is no
// configuration key for it; the month and day names are the locale's too.
//
// Its own namespace, `quantum-shell-calendar`, part of the frozen public interface like the bar's. Keyboard
// interactivity is on-demand: the panel takes keys when it is clicked and not before, and Escape closes it once it has
// them; a click outside closes it through the backdrop, and so does a second click on the clock.
LayerShellWindow {
    id: panel

    layer: LayerShellWindow.Overlay
    anchors: LayerShellWindow.TopEdge | LayerShellWindow.RightEdge
    keyboardInteractivity: LayerShellWindow.OnDemandKeyboard
    layerNamespace: "quantum-shell-calendar"
    exclusiveZone: 0

    readonly property color foreground: Config.bar.colors.foreground
    readonly property color muted: Config.bar.colors.muted
    readonly property color accent: Config.bar.colors.accent
    readonly property string face: Config.bar.font.family
    readonly property int fontSize: Config.bar.font.size
    readonly property int fontWeight: Config.bar.font.weight

    // The date the panel was opened on, and the first day of the month it is showing.
    property date today: new Date()
    property date shown: new Date(today.getFullYear(), today.getMonth(), 1)

    readonly property int cell: 36
    readonly property int chrome: 10 * 2 + 12 * 2
    width: cell * 7 + chrome
    height: chrome + header.height + 8 + weekdays.height + 4 + grid.height

    color: "transparent"
    visible: false

    // The cells of `month`'s grid: six weeks of dates starting on the locale's first weekday, each with the day
    // number and whether it belongs to the month. Six rows always, so the panel does not change height as months do.
    function cells(month, firstDayOfWeek) {
        const first = new Date(month.getFullYear(), month.getMonth(), 1)
        const back = (first.getDay() - firstDayOfWeek + 7) % 7
        const result = []
        for (let i = 0; i < 42; ++i) {
            const day = new Date(first.getFullYear(), first.getMonth(), 1 - back + i)
            result.push({
                day: day.getDate(),
                inMonth: day.getMonth() === first.getMonth(),
                year: day.getFullYear(), month: day.getMonth()
            })
        }
        return result
    }
    function isToday(cellData, now) {
        return cellData.day === now.getDate() && cellData.month === now.getMonth() && cellData.year === now.getFullYear()
    }
    function shiftedMonth(month, delta) {
        return new Date(month.getFullYear(), month.getMonth() + delta, 1)
    }
    function title(month) {
        return Qt.locale().monthName(month.getMonth(), Locale.LongFormat) + " " + month.getFullYear()
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 10
        radius: 10
        color: "#1a1b26"

        Item {
            id: keys
            anchors.fill: parent
            focus: true
            Keys.onEscapePressed: CalendarService.open = false

            Item {
                id: header
                objectName: "calendarHeader"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                height: 32

                // The buttons are worded with plain characters rather than arrow glyphs, for the reason the
                // control centre's are: no bundled font is promised to have them.
                Text {
                    id: previous
                    objectName: "calendarPrevious"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 32
                    horizontalAlignment: Text.AlignHCenter
                    textFormat: Text.PlainText
                    text: "<"
                    color: panel.accent
                    font.family: panel.face
                    font.pixelSize: panel.fontSize + 4
                    font.weight: panel.fontWeight
                    MouseArea {
                        anchors.fill: parent
                        onClicked: panel.shown = panel.shiftedMonth(panel.shown, -1)
                    }
                }
                Text {
                    objectName: "calendarTitle"
                    anchors.centerIn: parent
                    textFormat: Text.PlainText
                    text: panel.title(panel.shown)
                    color: panel.foreground
                    font.family: panel.face
                    font.pixelSize: panel.fontSize + 2
                    font.weight: panel.fontWeight
                }
                Text {
                    id: next
                    objectName: "calendarNext"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 32
                    horizontalAlignment: Text.AlignHCenter
                    textFormat: Text.PlainText
                    text: ">"
                    color: panel.accent
                    font.family: panel.face
                    font.pixelSize: panel.fontSize + 4
                    font.weight: panel.fontWeight
                    MouseArea {
                        anchors.fill: parent
                        onClicked: panel.shown = panel.shiftedMonth(panel.shown, 1)
                    }
                }
            }

            Row {
                id: weekdays
                anchors.left: parent.left
                anchors.top: header.bottom
                anchors.leftMargin: 12
                anchors.topMargin: 8
                height: 20
                Repeater {
                    model: 7
                    delegate: Text {
                        required property int index
                        width: panel.cell
                        height: weekdays.height
                        horizontalAlignment: Text.AlignHCenter
                        textFormat: Text.PlainText
                        text: Qt.locale().dayName((Qt.locale().firstDayOfWeek + index) % 7, Locale.ShortFormat)
                        color: panel.muted
                        font.family: panel.face
                        font.pixelSize: panel.fontSize - 1
                        font.weight: panel.fontWeight
                    }
                }
            }

            Grid {
                id: grid
                objectName: "calendarGrid"
                anchors.left: parent.left
                anchors.top: weekdays.bottom
                anchors.leftMargin: 12
                anchors.topMargin: 4
                columns: 7
                Repeater {
                    model: panel.cells(panel.shown, Qt.locale().firstDayOfWeek)
                    delegate: Item {
                        required property var modelData
                        objectName: "calendarDay"
                        readonly property bool today: panel.isToday(modelData, panel.today)
                        property alias dayText: label.text
                        width: panel.cell
                        height: panel.cell - 6
                        Rectangle {
                            anchors.centerIn: parent
                            width: 28
                            height: 28
                            radius: 14
                            color: parent.today ? panel.accent : "transparent"
                        }
                        Text {
                            id: label
                            anchors.centerIn: parent
                            textFormat: Text.PlainText
                            text: modelData.day
                            color: parent.today ? "#1a1b26" : modelData.inMonth ? panel.foreground : panel.muted
                            font.family: panel.face
                            font.pixelSize: panel.fontSize
                            font.weight: parent.today ? Font.Bold : panel.fontWeight
                        }
                    }
                }
            }
        }
    }
}
