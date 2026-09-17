import QtQuick
import QuantumShell 1.0

// The system's CPU and memory, as the shell reads them.
//
// Every number here comes from `SysMonService`, which reads /proc and takes the readings itself — nothing in
// this file reads a file, opens a socket or decides when a reading happens. That split is the boundary rule:
// the reading and its cadence are the service's, and what a readout looks like is this file's.
//
// *Which* readouts and *what form* the memory one takes are the configuration's: `[bar.system]`'s `show_cpu`,
// `show_memory` and `memory_format`, reaching this file as `Config.bar.system`. The schema decides that
// (it refuses a token it does not know by name), and this file is where a choice becomes text — a string for
// a person to read is presentation, and `src/` has no business formatting one. So the tokens compared below
// are the schema's list, and not a second spelling this file is free to invent: `bar-interaction-test` drives
// every one of them through this file, so a token renamed on one side fails there rather than falling back
// to the default branch below on screen.
//
// Hiding a readout is not switching off its reading. The service takes both halves in one pass whatever is
// drawn, so a hidden readout costs no wake-up and a shown one is never a number waiting for a flag to change.
//
// The two readouts are named, so that "the CPU readout is not drawn" can be read back as the row's own
// visibility — the label and the number together, because it is the row that the flag hides and not the
// value inside it.
//
// The two readouts are separate because they become available at different moments, and saying so is the
// point of the empty state rather than an accident of it. Memory is a value the kernel holds, so one reading
// of /proc/meminfo is a memory reading. A CPU percentage is the difference between two readings of
// /proc/stat's cumulative counters, so for the first `sampleIntervalMs` of the shell's life — and after every
// moment the bar was hidden — there is no CPU reading to show. `cpuAvailable` and `memoryAvailable` are those
// two facts, each a real flag from the service, and each readout draws a dash exactly while its flag is
// false: never a zero, never the last number seen, which would be a reading that is real but is not current.
//
// The widget is an Item rather than a Row so the group it is declared in can give it the bar's full height
// while its own content stays centred — the same shape `Workspaces.qml` has, and the reason `Clock.qml` has
// to align its own text. Nothing here carries a position: `Bar.qml` decides which group it is in.
Item {
    id: root

    // Named the way the strip's row is, so the group this widget landed in can be read without counting the
    // bar's children.
    objectName: "systemMonitor"

    property color foreground
    property color muted
    property string face

    // The bar's group gives the height; the width belongs to the content, like the strip's. Hiding both
    // readouts therefore leaves this widget no width at all rather than a gap where something used to be.
    width: content.width

    // The memory reading, in the form the configuration names. The tokens are `MemoryFormats` in
    // `ConfigSchema.h`, which refuses anything outside that list by name — so the last branch is the form the
    // file defaults to rather than somewhere an unknown token lands.
    function memoryText() {
        if (!SysMonService.memoryAvailable) {
            return "—";
        }
        const used = SysMonService.memoryUsedKb / 1048576;
        const total = SysMonService.memoryTotalKb / 1048576;
        switch (Config.bar.system.memoryFormat) {
        case "used":
            return used.toFixed(1) + "G";
        case "available":
            // `MemAvailable` itself, which is the kernel's own answer to what can still be handed out — the
            // field `memoryUsedKb` is the complement of, read here rather than recomputed by subtraction.
            return (SysMonService.memoryAvailableKb / 1048576).toFixed(0) + "G";
        case "percent":
            return total > 0 ? Math.round(100 * used / total) + "%" : "—";
        default:
            return used.toFixed(1) + "/" + total.toFixed(0) + "G";
        }
    }

    Row {
        id: content
        anchors.verticalCenter: parent.verticalCenter
        spacing: 14

        // CPU, and the interval it is averaged over is the service's sampling interval rather than anything
        // chosen here: it is the difference between two readings, not an instantaneous value. Shown or not by
        // `show_cpu`; its one honest form is a percentage, which is why there is no `cpu_format` key — a key
        // whose every legal value draws the same thing is a key that does nothing.
        Row {
            objectName: "cpuReadout"
            spacing: 5
            visible: Config.bar.system.showCpu

            Text {
                text: "CPU"
                color: root.muted
                font.family: root.face
                font.pixelSize: 12
            }

            Text {
                objectName: "cpuValue"
                text: SysMonService.cpuAvailable ? Math.round(SysMonService.cpuPercent) + "%" : "—"
                color: root.foreground
                font.family: root.face
                font.pixelSize: 12
            }
        }

        // Memory, in the form `memory_format` names. Its "used" is the shell's own `MemTotal - MemAvailable`
        // because /proc/meminfo has no used field, and every quantity below is shown in GiB from the KiB the
        // kernel reports.
        Row {
            objectName: "memoryReadout"
            spacing: 5
            visible: Config.bar.system.showMemory

            Text {
                text: "MEM"
                color: root.muted
                font.family: root.face
                font.pixelSize: 12
            }

            Text {
                objectName: "memoryValue"
                text: root.memoryText()
                color: root.foreground
                font.family: root.face
                font.pixelSize: 12
            }
        }
    }
}
