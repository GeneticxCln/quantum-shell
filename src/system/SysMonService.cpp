#include "system/SysMonService.h"

#include "QmlModule.h"
#include "app/Logging.h"

#include <QFile>
#include <QTimer>
#include <QQmlEngine>
#include <QtGlobal>

namespace quantum::system {

namespace {

// The most any line of these files can be. `/proc/stat` on a 256-core machine is a few hundred lines and
// `/proc/meminfo` is under 2 KiB; a cap exists so that a file replaced by something enormous — a test
// fixture pointed at the wrong path, or a mount that is not procfs at all — is a refusal rather than an
// allocation. It is far above anything either file ever contains, so no real reading is refused by it.
constexpr qint64 maxFileBytes = 1024 * 1024;

std::optional<QByteArray> readFile(const QString& path, const char* what)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(quantum::app::systemLog) << "cannot read" << what << "at" << path << ":" << file.errorString();
        return std::nullopt;
    }
    if (file.size() > maxFileBytes) {
        qCWarning(quantum::app::systemLog)
            << "refusing" << what << "at" << path << ":" << file.size() << "bytes is larger than"
            << maxFileBytes << ", so this is not the file it is meant to be";
        return std::nullopt;
    }
    const QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        qCWarning(quantum::app::systemLog) << "cannot read" << what << "at" << path << ":" << file.errorString();
        return std::nullopt;
    }
    return contents;
}

std::optional<quint64> asCounter(const QByteArray& token)
{
    bool ok = false;
    const quint64 value = token.toULongLong(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::optional<CpuTimes> parseAggregateCpuTimes(const QByteArray& statText)
{
    // The aggregate line and not a per-core one: `cpu ` with the space is the whole-CPU line, `cpu0` is one
    // core, and they are different readings that must not be confused for each other.
    for (const QByteArray& line : statText.split('\n')) {
        if (!line.startsWith("cpu "))
            continue;

        const QList<QByteArray> fields = line.simplified().split(' ');
        // label + user + nice + system + idle + iowait + irq + softirq + steal. The kernel's documented line
        // has two more — guest and guest_nice — which are deliberately not read: they are already included in
        // user and nice, so counting them would count that time twice.
        constexpr int fieldsRead = 8;
        if (fields.size() < fieldsRead + 1) {
            qCWarning(quantum::app::systemLog)
                << "refusing /proc/stat's cpu line: expected at least" << fieldsRead << "counters, found"
                << fields.size() - 1 << "in" << line;
            return std::nullopt;
        }

        quint64 counters[fieldsRead] = {};
        for (int i = 0; i < fieldsRead; ++i) {
            const auto value = asCounter(fields.at(i + 1));
            if (!value) {
                qCWarning(quantum::app::systemLog)
                    << "refusing /proc/stat's cpu line: field" << i << "is not a number:" << fields.at(i + 1);
                return std::nullopt;
            }
            counters[i] = *value;
        }

        CpuTimes times;
        for (quint64 counter : counters)
            times.total += counter;
        // idle + iowait: no task wanted the CPUs. iowait is idle time with outstanding I/O, which the kernel's
        // own accounting tools count as idle rather than as work.
        times.idle = counters[3] + counters[4];
        return times;
    }

    qCWarning(quantum::app::systemLog) << "refusing /proc/stat: no aggregate cpu line";
    return std::nullopt;
}

std::optional<MemoryReading> parseMemInfo(const QByteArray& memInfoText)
{
    std::optional<quint64> total;
    std::optional<quint64> available;

    for (const QByteArray& line : memInfoText.split('\n')) {
        const bool isTotal = line.startsWith("MemTotal:");
        const bool isAvailable = line.startsWith("MemAvailable:");
        if (!isTotal && !isAvailable)
            continue;

        const QList<QByteArray> parts = line.mid(line.indexOf(':') + 1).simplified().split(' ');
        // The value and its unit. A number whose unit is not the one this file documents is not a value in
        // KiB, and naming it as one because the digits happened to parse is how a wrong reading gets shown.
        if (parts.size() != 2 || parts.at(1) != "kB") {
            qCWarning(quantum::app::systemLog)
                << "refusing /proc/meminfo's" << (isTotal ? "MemTotal" : "MemAvailable") << "line:" << line
                << "- expected a value in kB";
            return std::nullopt;
        }
        const auto value = asCounter(parts.at(0));
        if (!value) {
            qCWarning(quantum::app::systemLog)
                << "refusing /proc/meminfo's" << (isTotal ? "MemTotal" : "MemAvailable") << "line:" << line
                << "- not a number";
            return std::nullopt;
        }
        (isTotal ? total : available) = value;
    }

    if (!total || !available) {
        // Not substituted with `MemFree + Buffers + Cached`, which is a different estimate of a different
        // thing: showing it under this field's name would be a reading nobody computed.
        qCWarning(quantum::app::systemLog) << "refusing /proc/meminfo: MemTotal is"
                                           << (total ? "present" : "missing") << "and MemAvailable is"
                                           << (available ? "present" : "missing");
        return std::nullopt;
    }

    MemoryReading reading;
    reading.totalKb = *total;
    reading.availableKb = *available;
    return reading;
}

std::optional<double> busyPercent(const CpuTimes& before, const CpuTimes& after)
{
    // A backwards counter cannot happen on one boot. It is refused rather than subtracted, because the
    // subtraction is unsigned and would wrap into a percentage that looks like a number.
    if (after.total <= before.total || after.idle < before.idle) {
        return std::nullopt;
    }
    const quint64 deltaTotal = after.total - before.total;
    const quint64 deltaIdle = after.idle - before.idle;
    if (deltaIdle > deltaTotal) {
        return std::nullopt;
    }
    return 100.0 * static_cast<double>(deltaTotal - deltaIdle) / static_cast<double>(deltaTotal);
}

QString SysMonService::defaultProcRoot()
{
    return QStringLiteral("/proc");
}

SysMonService::SysMonService(const QString& procRoot, QObject* parent)
    : QObject(parent), procRoot_(procRoot), timer_(new QTimer(this))
{
    // Single-shot and re-armed after each reading, rather than a repeating timer: the next reading is
    // `sampleIntervalMs` after the last one *finished*, so a reading that took a while cannot make the next
    // one late and then queue behind it. It is also what makes stopping it a single call.
    timer_->setSingleShot(true);
    QObject::connect(timer_, &QTimer::timeout, this, [this] {
        if (!active_)
            return;
        sampleNow();
        armTimer();
    });
}

SysMonService::~SysMonService() = default;

void SysMonService::setActive(bool active)
{
    if (active == active_) {
        return;
    }
    active_ = active;

    if (active_) {
        // The first reading is taken now rather than one interval from now, so the memory half is on screen
        // immediately. The CPU half needs the reading after this one — one sample is a baseline, not a
        // percentage — so `cpuAvailable` stays false until it exists.
        sampleNow();
        armTimer();
    } else {
        timer_->stop();
        // Nothing is claimed about a moment nobody is watching. `cpuPercent` is an average over an interval,
        // so keeping it across a gap would publish a number that is real but is not the one it says it is.
        const bool hadReading = cpuAvailable_ || memoryAvailable_;
        previousCpu_.reset();
        cpuAvailable_ = false;
        memoryAvailable_ = false;
        memory_ = MemoryReading{};
        cpuPercent_ = 0.0;
        if (hadReading)
            emit readingChanged();
    }

    emit activeChanged();
}

void SysMonService::setSampleIntervalMs(int milliseconds)
{
    if (milliseconds < MinimumSampleIntervalMs) {
        // Refused rather than clamped, so the caller keeps the value it had and the record says what was
        // wrong with the new one. The floor is the kernel's: `USER_HZ` is 100, so two readings less than a
        // tick apart can find no tick between them, and a percentage over no interval is not a reading.
        qCWarning(quantum::app::systemLog)
            << "refusing a sampling interval of" << milliseconds << "ms: the kernel counts CPU time in "
            << "USER_HZ ticks, so an interval below" << MinimumSampleIntervalMs
            << "ms can span no tick at all; keeping" << sampleIntervalMs_ << "ms";
        return;
    }
    if (milliseconds == sampleIntervalMs_) {
        return;
    }
    sampleIntervalMs_ = milliseconds;
    // Said once here rather than where the value came from, because this is the object the cadence belongs
    // to and the only one that knows every way it can change: the composition root sets it from
    // `bar.system.sample_interval_ms` at startup, and a later edit to that key arrives through the same
    // setter. A person asking "why is the CPU readout so slow, or so busy?" has this record as the answer,
    // and the live test reads it back out of a running shell to prove the configured value got here.
    qCInfo(quantum::app::systemLog) << "sampling /proc/stat and /proc/meminfo every" << sampleIntervalMs_
                                    << "ms while the bar is visible";
    // Re-armed so a new interval takes effect now rather than after the old one has elapsed.
    if (active_)
        armTimer();
    emit sampleIntervalMsChanged();
}

void SysMonService::armTimer()
{
    timer_->start(sampleIntervalMs_);
}

void SysMonService::sampleNow()
{
    // Memory is a value the kernel holds: one read of it is a reading.
    const std::optional<MemoryReading> memory =
        readFile(procRoot_ + QStringLiteral("/meminfo"), "/proc/meminfo")
            .and_then([](const QByteArray& text) { return parseMemInfo(text); });

    // CPU is not: the aggregate counters are cumulative since boot, so a percentage is the difference between
    // this reading and the one before it.
    const std::optional<CpuTimes> cpu =
        readFile(procRoot_ + QStringLiteral("/stat"), "/proc/stat")
            .and_then([](const QByteArray& text) { return parseAggregateCpuTimes(text); });

    const std::optional<double> percent =
        (cpu && previousCpu_) ? busyPercent(*previousCpu_, *cpu) : std::nullopt;
    if (cpu && previousCpu_ && !percent) {
        qCWarning(quantum::app::systemLog)
            << "no CPU reading: the aggregate counters did not advance between two readings";
    }
    if (cpu)
        previousCpu_ = cpu;
    else
        previousCpu_.reset();  // no baseline either: the next reading must not be a difference from a gap

    // Every value the widget can read, compared before anything is emitted: a reading that changed nothing
    // must not wake a binding, which is the same rule `NiriService` and `Config` keep.
    bool changed = false;
    const bool newCpuAvailable = percent.has_value();
    if (newCpuAvailable != cpuAvailable_) {
        cpuAvailable_ = newCpuAvailable;
        changed = true;
    }
    if (percent && *percent != cpuPercent_) {
        cpuPercent_ = *percent;
        changed = true;
    }
    if (memory) {
        if (memory->totalKb != memory_.totalKb || memory->availableKb != memory_.availableKb) {
            memory_ = *memory;
            changed = true;
        }
        if (!memoryAvailable_) {
            memoryAvailable_ = true;
            changed = true;
        }
    } else {
        // A reading that could not be taken is not left standing as if it had been.
        if (memoryAvailable_ || memory_.totalKb != 0) {
            memoryAvailable_ = false;
            memory_ = MemoryReading{};
            changed = true;
        }
    }

    if (changed)
        emit readingChanged();
}

void SysMonService::registerQmlSingleton(SysMonService& service)
{
    qmlRegisterSingletonInstance(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion,
                                 quantum::qml::ModuleMinorVersion, QmlTypeName, &service);
}

}  // namespace quantum::system
