// The system's own CPU and memory, read from /proc, as QML sees it.
//
// This is the bar's second reading that has **no event source**, and the first one that needs a clock of
// its own. The clock is the shell's other one and it is written down in `qml/Clock.qml`: nothing notifies a
// listener that the minute changed, so a clock either asks or it is wrong. The same is true here, for a
// sharper reason — `/proc/stat`'s aggregate line is *cumulative jiffies since boot*, so a CPU percentage is
// not a value the kernel holds at all, it is the difference between two readings, and a difference needs two
// moments. There is no subscription to have: the kernel's pressure files report stall time rather than
// occupancy, and this kernel has no memory-pressure file at all, which was read rather than assumed. So the
// reading is taken on a single-shot timer that re-arms itself, and the reason is here rather than left to
// look like the polling SYSTEM_PROMPT.md forbids.
//
// Three rules keep that from becoming a permissive exception:
//
//   * **Nothing is sampled while the bar is not on screen.** `active` is the bar's own visibility, wired in
//     `main.cpp`; inactive means no timer is armed at all and no reading is claimed. The idle cost of this
//     service on a hidden bar is exactly zero wake-ups.
//   * **A reading that stopped being taken stops being shown.** Deactivating clears the values and sets
//     `available` false, rather than leaving a number that was true two minutes ago on screen looking
//     current. The CPU baseline is dropped with it, so every percentage published is the difference between
//     two readings taken `sampleIntervalMs` apart and not across a gap nobody was watching.
//   * **A reading that could not be taken is not guessed at.** A file that cannot be opened, a line that is
//     not the shape the kernel documents, a value that is not a number — each is a refusal with the reason
//     logged, and `available` goes false. Nothing falls back to a different quantity, because
//     `MemFree + Buffers + Cached` is a different estimate wearing the same label.
//
// Names, and where they were verified: the aggregate `cpu` line of `/proc/stat` is
// `cpu user nice system idle iowait irq softirq steal guest guest_nice`, documented in proc(5), and the
// guest values are *already counted* in user and nice — so the total is the first eight fields and summing
// all ten would double-count them. `/proc/meminfo` documents `MemTotal` and `MemAvailable`; the kernel's own
// "how much can be allocated without swapping" is `MemAvailable`, and there is no `MemUsed`, which is why
// `used` here is `MemTotal - MemAvailable` and is named for that subtraction rather than for a kernel field
// that does not exist.
//
// What QML gets, each property because a binding reads it:
//
//   cpuPercent        the busy share over the interval between the last two readings, 0 while unavailable
//   cpuAvailable      false until two readings exist — one reading is a baseline, not a percentage
//   memoryUsedKb      MemTotal - MemAvailable, in KiB
//   memoryTotalKb     MemTotal, in KiB
//   memoryAvailableKb MemAvailable itself, in KiB — the kernel's own answer to what can still be handed
//                     out, published as the field it is rather than left for a widget to recompute by
//                     subtracting `memoryUsedKb` from `memoryTotalKb`
//   memoryAvailable   false until a reading has been taken and parsed
//   active            whether the reading is being kept current at all
//   sampleIntervalMs  how far apart two readings are, which is also what `cpuPercent` is an average over
//
// `cpuPercent` is 0 rather than absent while `cpuAvailable` is false, because a double has no empty value;
// it is the flag that is the honest empty state, and `qml/SystemMonitor.qml` is written to read the flag and
// never the number. That is the one place this differs from `NiriService`, where an absent value is an empty
// map — a map has an empty, a percentage does not.
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <optional>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace quantum::system {

// The aggregate CPU counters from `/proc/stat`'s `cpu` line, in the kernel's own units (USER_HZ, which is
// 100 Hz on every architecture this shell builds on and is not assumed to be anything else: the values are
// only ever used as differences of each other, so the unit cancels).
struct CpuTimes {
    // idle + iowait: the time no task wanted the CPUs. iowait is idle time that had outstanding I/O, which
    // is why the kernel's own accounting tools add it here rather than to busy time.
    quint64 idle = 0;
    // user + nice + system + idle + iowait + irq + softirq + steal. guest and guest_nice are excluded
    // because the kernel already includes them in user and nice.
    quint64 total = 0;
};

// `MemTotal` and `MemAvailable` from `/proc/meminfo`, in KiB as the kernel reports them.
struct MemoryReading {
    quint64 totalKb = 0;
    quint64 availableKb = 0;

    // What the widget shows as "used": the rest of what can be handed out. Named for the subtraction
    // because no `MemUsed` field exists to read.
    quint64 usedKb() const { return totalKb - availableKb; }
};

// The parsers, as pure functions of the text — which is what lets every shape of input, including the ones
// the kernel does not produce, be tested without a /proc to hand. `nullopt` is a refusal, never a default.

// Reads the aggregate `cpu` line (not a per-core `cpuN` line, which would be a different reading).
std::optional<CpuTimes> parseAggregateCpuTimes(const QByteArray& statText);

// Reads `MemTotal` and `MemAvailable`. Refuses the text when either is missing, rather than substituting a
// second-best quantity under the same name.
std::optional<MemoryReading> parseMemInfo(const QByteArray& memInfoText);

// The busy share between two readings, as a percentage. Refuses a pair whose counters did not advance
// (nothing to divide by) and a pair that went backwards, which cannot happen on one boot and would otherwise
// wrap around an unsigned subtraction into a nonsensical percentage.
std::optional<double> busyPercent(const CpuTimes& before, const CpuTimes& after);

class SysMonService : public QObject {
    Q_OBJECT

public:
    // The cadence the shell ships with, and the shortest interval it will accept. Both are stated here
    // because this is the code that has a reason for each: 2000 ms is policy — often enough that a person
    // watching the readout sees it move, rare enough that a bar nobody is looking at is not the thing that
    // wakes a laptop up — and 10 ms is a fact about the kernel, whose `USER_HZ` is 100, so an interval
    // shorter than one tick can span no tick at all and a percentage over no interval does not exist.
    //
    // `ConfigSchema.h` declares the same two numbers as the configuration file's default and its bound,
    // because a schema is a pure function of the file's text and does not include this header — and
    // `config-test` compares both pairs at compile time, so the two spellings cannot drift apart. That is
    // the same rule the key names and the QML module names are held to.
    inline static constexpr int DefaultSampleIntervalMs = 2000;
    inline static constexpr int MinimumSampleIntervalMs = 10;

    // The path the readings come from. A constructor argument with a default rather than a constant inside
    // the reads, for the reason `ConfigWatcher` takes its path: a test can point this at a directory of its
    // own and drive every shape of input through the same code the shell runs.
    static QString defaultProcRoot();

    explicit SysMonService(const QString& procRoot = defaultProcRoot(), QObject* parent = nullptr);
    ~SysMonService() override;

    Q_PROPERTY(double cpuPercent READ cpuPercent NOTIFY readingChanged)
    Q_PROPERTY(bool cpuAvailable READ cpuAvailable NOTIFY readingChanged)
    Q_PROPERTY(qulonglong memoryUsedKb READ memoryUsedKb NOTIFY readingChanged)
    Q_PROPERTY(qulonglong memoryTotalKb READ memoryTotalKb NOTIFY readingChanged)
    Q_PROPERTY(qulonglong memoryAvailableKb READ memoryAvailableKb NOTIFY readingChanged)
    Q_PROPERTY(bool memoryAvailable READ memoryAvailable NOTIFY readingChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(int sampleIntervalMs READ sampleIntervalMs WRITE setSampleIntervalMs NOTIFY sampleIntervalMsChanged)

    double cpuPercent() const { return cpuPercent_; }
    bool cpuAvailable() const { return cpuAvailable_; }
    qulonglong memoryUsedKb() const { return memory_.usedKb(); }
    qulonglong memoryTotalKb() const { return memory_.totalKb; }
    qulonglong memoryAvailableKb() const { return memory_.availableKb; }
    bool memoryAvailable() const { return memoryAvailable_; }
    bool active() const { return active_; }
    int sampleIntervalMs() const { return sampleIntervalMs_; }

    // Whether the reading is kept current. Called with the bar's visibility, so a hidden bar costs nothing;
    // false forgets the reading, so nothing stale is ever on screen. Not a `Q_INVOKABLE`: the shell's QML
    // reads the properties and never decides when the shell samples.
    void setActive(bool active);

    // How far apart two readings are, and therefore the interval `cpuPercent` averages over. Settable
    // because the cadence is policy the shell owns: `bar.system.sample_interval_ms` in the configuration
    // file reaches it through the composition root, a change to that key applies to a running shell, and a
    // test drives it with a few milliseconds. An interval below `MinimumSampleIntervalMs` is refused with
    // the reason rather than accepted and sampled pointlessly.
    void setSampleIntervalMs(int milliseconds);

    // Takes one reading now: what the timer calls, and what makes the sampling policy testable without
    // waiting on a timer. Not a `Q_INVOKABLE` for the same reason `setActive` is not.
    void sampleNow();

    // Registers `service` so a QML file can bind to it:
    //
    //     import QuantumShell 1.0
    //     Text { text: SysMonService.cpuAvailable ? Math.round(SysMonService.cpuPercent) + "%" : "—" }
    //
    // The instance stays owned by C++; QML only reads it. The module URI and version come from
    // `QmlModule.h`, which is the one place they are written down, because they are the same module
    // `NiriService`, `NiriActions` and `Config` are registered into.
    static void registerQmlSingleton(SysMonService& service);

    // The type name above, declared once. It is interface of the same kind as `NiriService`'s: a QML file is
    // written against it, so a rename that is not a rename everywhere fails at load time rather than at build
    // time. `sysmon_test.cpp` mirrors it and compares its copy at compile time.
    inline static constexpr auto QmlTypeName = "SysMonService";

signals:
    void readingChanged();
    void activeChanged();
    void sampleIntervalMsChanged();

private:
    void armTimer();

    QString procRoot_;
    QTimer* timer_ = nullptr;

    bool active_ = false;
    int sampleIntervalMs_ = DefaultSampleIntervalMs;

    // The last reading taken, and whether each half of it exists. The CPU half needs a previous reading to be
    // a percentage at all, which is why there are two of these rather than one.
    std::optional<CpuTimes> previousCpu_;
    std::optional<CpuTimes> lastCpu_;
    MemoryReading memory_;
    double cpuPercent_ = 0.0;
    bool cpuAvailable_ = false;
    bool memoryAvailable_ = false;
};

}  // namespace quantum::system
