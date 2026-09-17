// The system's CPU and memory readings: the /proc lines they come from, what a refusal looks like, and the
// cadence the service owns.
//
// Nothing here needs a compositor, a display or even a real /proc: the service takes the directory it reads
// from as a constructor argument, for the reason `ConfigWatcher` takes its path, so a directory this test
// writes can drive every shape of input through the same code the shell runs — including the shapes the
// kernel does not produce and a real machine therefore cannot show. Two slots read the real /proc as well,
// because a parser that is only ever handed this test's own strings agrees with this test and nothing else.
//
// The cadence is the part worth stating. `/proc/stat`'s aggregate line is cumulative jiffies since boot, so a
// CPU percentage is not a value the kernel holds: it is the difference between two readings. What that means
// for a test is that the first reading can never produce one, and that is asserted rather than waited out —
// `sampleNow()` is public for exactly this, so the numbers are driven without a timer, while the timer itself
// is checked separately with an interval in milliseconds.
#include "system/SysMonService.h"

#include "app/Logging.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <string_view>

using quantum::system::CpuTimes;
using quantum::system::MemoryReading;
using quantum::system::SysMonService;

namespace {

// The QML name this service is registered under, mirrored here rather than read out of the header. A rename
// in `src/system/` does not build until this file agrees, which is where the question "who else writes
// `SysMonService`" gets asked: `qml/SystemMonitor.qml` is the answer, and its own loading is asserted in
// `bar-interaction-test`.
constexpr auto coveredServiceTypeName = "SysMonService";
static_assert(std::string_view(SysMonService::QmlTypeName) == std::string_view(coveredServiceTypeName),
              "the QML type name changed: update the mirror above, and every QML file that binds to it");

// The aggregate line as the kernel writes it: `cpu` and then user, nice, system, idle, iowait, irq, softirq,
// steal, and optionally the two guest counters.
QByteArray cpuLine(quint64 user, quint64 nice, quint64 system, quint64 idle, quint64 iowait = 0,
                   quint64 irq = 0, quint64 softirq = 0, quint64 steal = 0, const QByteArray& guests = {})
{
    return QStringLiteral("cpu  %1 %2 %3 %4 %5 %6 %7 %8 %9\n")
        .arg(user)
        .arg(nice)
        .arg(system)
        .arg(idle)
        .arg(iowait)
        .arg(irq)
        .arg(softirq)
        .arg(steal)
        .arg(QString::fromLatin1(guests))
        .toUtf8();
}

// A /proc/stat with one per-core line under the aggregate one, which is what a real file looks like and what
// makes "read the aggregate, not a core" a claim the parser can actually get wrong.
QByteArray statFile(quint64 user, quint64 idle)
{
    return cpuLine(user, 0, 0, idle) + QByteArrayLiteral("cpu0 1 0 0 1 0 0 0 0\ncpu1 1 0 0 1 0 0 0 0\n");
}

QByteArray memInfoFile(quint64 totalKb, quint64 availableKb)
{
    return QStringLiteral("MemTotal:       %1 kB\nMemFree:        1024 kB\nMemAvailable:   %2 kB\n"
                          "Buffers:        100 kB\nCached:         200 kB\n")
        .arg(totalKb)
        .arg(availableKb)
        .toUtf8();
}

// Writes one of the two files the service reads. Returns whether it landed, so a slot can fail on the write
// rather than on the reading that came from a file that was never there.
bool writeFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(contents) == contents.size();
}

// The shell's own records, captured rather than described, for the reason `niri-actions-test` captures them:
// a refusal that is only visible to whoever happened to connect to a signal is not a report.
struct Record {
    QtMsgType type = QtDebugMsg;
    QString category;
    QString message;
};

QList<Record> records;

void captureRecords(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    records.append(Record{type, QString::fromUtf8(context.category ? context.category : ""), message});
}

}  // namespace

class SysMonTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void parsesTheAggregateCpuLineAndNotAPerCoreOne();
    void sumsTheEightCountersTheKernelDocumentsSoGuestIsNotCountedTwice();
    void refusesACpuLineItCannotRead();

    void readsMemTotalAndMemAvailableAndNothingElse();
    void refusesAMemoryReadingItCannotName();

    void computesTheBusyShareBetweenTwoReadings();
    void refusesAPairOfReadingsItCannotSubtract();

    void takesTheFirstReadingFromAProcOfItsOwn();
    void publishesNothingWhenTheReadingDidNotChange();
    void dropsAReadingItCouldNotTake();
    void recordsEveryRefusalWithItsReason();

    void samplesOnlyWhileItIsActive();
    void samplesOnTheIntervalItWasGiven();
    void recordsTheCadenceItAcceptedAndOnlyWhenItChanged();

    void readsTheRealProc();
};

void SysMonTest::initTestCase()
{
    // The default the shell runs with, and the two files it reads. A machine without them is a machine this
    // service has nothing to say about, which is worth knowing before the slots that assert what it says.
    QCOMPARE(SysMonService::defaultProcRoot(), QStringLiteral("/proc"));
    QVERIFY2(QFile::exists(QStringLiteral("/proc/stat")), "/proc/stat is not readable here");
    QVERIFY2(QFile::exists(QStringLiteral("/proc/meminfo")), "/proc/meminfo is not readable here");
}

void SysMonTest::parsesTheAggregateCpuLineAndNotAPerCoreOne()
{
    const auto times = quantum::system::parseAggregateCpuTimes(statFile(10, 90));
    QVERIFY2(times.has_value(), "the aggregate cpu line was refused");
    // The aggregate line only: the per-core lines below it carry their own smaller counters, and reading one
    // of those instead would describe one core while the widget says "CPU".
    QCOMPARE(times->total, quint64(100));
    QCOMPARE(times->idle, quint64(90));
}

void SysMonTest::sumsTheEightCountersTheKernelDocumentsSoGuestIsNotCountedTwice()
{
    // user nice system idle iowait irq softirq steal, then guest and guest_nice. The kernel counts guest time
    // inside user and nice already, so a total that included them would be counting that time twice — which is
    // the mistake this slot exists to catch.
    const auto times = quantum::system::parseAggregateCpuTimes(statFile(10, 90).replace(
        QByteArrayLiteral("cpu  10 0 0 90 0 0 0 0"),
        QByteArrayLiteral("cpu  10 0 0 90 0 0 0 0 7 3")));
    QVERIFY2(times.has_value(), "a line carrying the guest counters was refused");
    QCOMPARE(times->total, quint64(100));
    QCOMPARE(times->idle, quint64(90));

    // iowait is idle time that had outstanding I/O, which is why it is counted as idle rather than as work.
    const auto withIowait = quantum::system::parseAggregateCpuTimes(cpuLine(10, 0, 0, 50, 40));
    QVERIFY2(withIowait.has_value(), "a line with iowait was refused");
    QCOMPARE(withIowait->idle, quint64(90));
}

void SysMonTest::refusesACpuLineItCannotRead()
{
    // Each of these is a different way of not understanding the file, and every one of them is a refusal
    // rather than a reading built out of the parts that happened to parse.
    QVERIFY(!quantum::system::parseAggregateCpuTimes(QByteArrayLiteral("cpu0 1 2 3 4 5 6 7 8\n")).has_value());
    QVERIFY(!quantum::system::parseAggregateCpuTimes(QByteArrayLiteral("cpu  1 2 3 4\n")).has_value());
    QVERIFY(!quantum::system::parseAggregateCpuTimes(QByteArrayLiteral("cpu  1 2 3 4 5 6 7 x\n")).has_value());
    QVERIFY(!quantum::system::parseAggregateCpuTimes(QByteArrayLiteral("ctxt 12345\nbtime 1\n")).has_value());
    QVERIFY(!quantum::system::parseAggregateCpuTimes(QByteArrayLiteral("")).has_value());
}

void SysMonTest::readsMemTotalAndMemAvailableAndNothingElse()
{
    const auto memory = quantum::system::parseMemInfo(memInfoFile(32779356, 17827500));
    QVERIFY2(memory.has_value(), "a meminfo in the kernel's own format was refused");
    QCOMPARE(memory->totalKb, quint64(32779356));
    QCOMPARE(memory->availableKb, quint64(17827500));
    // What the widget shows as used, and it is this subtraction rather than a field: /proc/meminfo has no
    // "used", and MemFree or MemFree+Buffers+Cached are different quantities wearing a similar name.
    QCOMPARE(memory->usedKb(), quint64(32779356 - 17827500));
}

void SysMonTest::refusesAMemoryReadingItCannotName()
{
    // No MemAvailable at all. The old approximation is deliberately not substituted: it is a different
    // estimate, and showing it under this name would be a reading nobody computed.
    QVERIFY(!quantum::system::parseMemInfo(QByteArrayLiteral("MemTotal:  1000 kB\nMemFree: 500 kB\n")).has_value());
    QVERIFY(!quantum::system::parseMemInfo(QByteArrayLiteral("MemAvailable: 500 kB\n")).has_value());
    // A number whose unit is not the one the file documents is not a value in KiB.
    QVERIFY(!quantum::system::parseMemInfo(QByteArrayLiteral("MemTotal: 1000 MB\nMemAvailable: 500 kB\n"))
                 .has_value());
    QVERIFY(!quantum::system::parseMemInfo(QByteArrayLiteral("MemTotal: lots kB\nMemAvailable: 500 kB\n"))
                 .has_value());
    QVERIFY(!quantum::system::parseMemInfo(QByteArray()).has_value());
}

void SysMonTest::computesTheBusyShareBetweenTwoReadings()
{
    const CpuTimes before{50, 100};

    const auto half = quantum::system::busyPercent(before, CpuTimes{100, 200});
    QVERIFY2(half.has_value(), "a pair that advanced by two hundred jiffies was refused");
    QCOMPARE(*half, 50.0);

    // Nothing at all was done: every jiffy of the interval was idle.
    const auto idle = quantum::system::busyPercent(before, CpuTimes{150, 200});
    QVERIFY2(idle.has_value(), "an idle interval was refused");
    QCOMPARE(*idle, 0.0);

    // No idle jiffies at all: the whole interval was work.
    const auto busy = quantum::system::busyPercent(before, CpuTimes{50, 200});
    QVERIFY2(busy.has_value(), "a fully busy interval was refused");
    QCOMPARE(*busy, 100.0);
}

void SysMonTest::refusesAPairOfReadingsItCannotSubtract()
{
    const CpuTimes before{50, 100};

    // The same counters twice: the interval has no length, so there is nothing to divide by. This is the case
    // a timer faster than the kernel's own accounting produces.
    QVERIFY(!quantum::system::busyPercent(before, before).has_value());
    // Counters that went backwards. They cannot on one boot, and subtracting them as unsigned would wrap into
    // a percentage that looks like a number.
    QVERIFY(!quantum::system::busyPercent(before, CpuTimes{40, 90}).has_value());
    // More idle time than total time is not a state the kernel reports.
    QVERIFY(!quantum::system::busyPercent(before, CpuTimes{200, 150}).has_value());
}

void SysMonTest::takesTheFirstReadingFromAProcOfItsOwn()
{
    QTemporaryDir proc;
    QVERIFY2(proc.isValid(), qPrintable(proc.errorString()));
    const QString stat = proc.filePath(QStringLiteral("stat"));
    const QString meminfo = proc.filePath(QStringLiteral("meminfo"));
    QVERIFY(writeFile(stat, statFile(0, 0)));
    QVERIFY(writeFile(meminfo, memInfoFile(1000, 400)));

    SysMonService service(proc.path());
    QVERIFY2(!service.active(), "the service samples before anything asks it to");

    // The first reading. Memory is a value the kernel holds, so one read of it is a reading; the CPU counters
    // are cumulative, so this reading is a baseline and there is no percentage in it yet.
    service.sampleNow();
    QVERIFY2(service.memoryAvailable(), "the first reading published no memory");
    QCOMPARE(service.memoryTotalKb(), qulonglong(1000));
    QCOMPARE(service.memoryUsedKb(), qulonglong(600));
    QVERIFY2(!service.cpuAvailable(), "a CPU percentage was published from a single reading");

    // The second, an interval later: 100 jiffies passed and 30 of them were idle, so the interval was 70% busy.
    QVERIFY(writeFile(stat, statFile(70, 30)));
    service.sampleNow();
    QVERIFY2(service.cpuAvailable(), "two readings did not produce a CPU percentage");
    QCOMPARE(service.cpuPercent(), 70.0);
    // The memory file did not change, and neither did the value published from it.
    QCOMPARE(service.memoryUsedKb(), qulonglong(600));
}

void SysMonTest::publishesNothingWhenTheReadingDidNotChange()
{
    QTemporaryDir proc;
    QVERIFY2(proc.isValid(), qPrintable(proc.errorString()));
    const QString stat = proc.filePath(QStringLiteral("stat"));
    const QString meminfo = proc.filePath(QStringLiteral("meminfo"));
    QVERIFY(writeFile(meminfo, memInfoFile(1000, 400)));

    SysMonService service(proc.path());
    QSignalSpy readings(&service, &SysMonService::readingChanged);

    // Two readings that establish a percentage of 50, then a third whose interval is *also* 50% — an
    // idle machine's counters advance by the same amount every few seconds, so this is what a quiet desktop
    // looks like rather than a contrivance.
    QVERIFY(writeFile(stat, statFile(0, 0)));
    service.sampleNow();
    const int afterFirst = readings.count();
    QVERIFY2(afterFirst > 0, "the first reading published nothing at all");

    QVERIFY(writeFile(stat, statFile(50, 50)));
    service.sampleNow();
    const int afterSecond = readings.count();
    QCOMPARE(service.cpuPercent(), 50.0);
    QVERIFY2(afterSecond > afterFirst, "the CPU percentage that appeared was not published");

    QVERIFY(writeFile(stat, statFile(100, 100)));
    service.sampleNow();
    // Same percentage, same memory: a binding has nothing to re-evaluate, so nothing is emitted.
    QCOMPARE(service.cpuPercent(), 50.0);
    QCOMPARE(readings.count(), afterSecond);
}

void SysMonTest::dropsAReadingItCouldNotTake()
{
    QTemporaryDir proc;
    QVERIFY2(proc.isValid(), qPrintable(proc.errorString()));
    const QString stat = proc.filePath(QStringLiteral("stat"));
    const QString meminfo = proc.filePath(QStringLiteral("meminfo"));
    QVERIFY(writeFile(meminfo, memInfoFile(1000, 400)));

    SysMonService service(proc.path());
    QVERIFY(writeFile(stat, statFile(0, 0)));
    service.sampleNow();
    QVERIFY(writeFile(stat, statFile(70, 30)));
    service.sampleNow();
    QVERIFY2(service.cpuAvailable() && service.memoryAvailable(), "the reading never became complete");

    // The file a reading comes from is replaced by text that is not that file. A reading that could not be
    // taken must not be left standing as if it had been: the widget's empty state is driven by these flags, so
    // a stale number would be indistinguishable from a current one.
    QVERIFY(writeFile(meminfo, QByteArrayLiteral("this is not /proc/meminfo\n")));
    service.sampleNow();
    QVERIFY2(!service.memoryAvailable(), "memory is still claimed after a reading that could not be taken");
    QCOMPARE(service.memoryTotalKb(), qulonglong(0));

    // The CPU counters are dropped with it — a difference across a gap is not a percentage over an interval.
    QVERIFY(writeFile(stat, QByteArrayLiteral("garbage\n")));
    service.sampleNow();
    QVERIFY2(!service.cpuAvailable(), "a CPU percentage is still claimed after a reading that failed");
}

void SysMonTest::recordsEveryRefusalWithItsReason()
{
    QTemporaryDir proc;
    QVERIFY2(proc.isValid(), qPrintable(proc.errorString()));
    QVERIFY(writeFile(proc.filePath(QStringLiteral("stat")), QByteArrayLiteral("cpu  1 2 3\n")));
    QVERIFY(writeFile(proc.filePath(QStringLiteral("meminfo")), QByteArrayLiteral("MemTotal: 1000 kB\n")));

    SysMonService service(proc.path());
    const QtMessageHandler previous = qInstallMessageHandler(&captureRecords);
    records.clear();
    service.sampleNow();
    qInstallMessageHandler(previous);

    // Two refusals, each naming what it could not read: the widget shows its empty state either way, so the
    // record is the only place the reason exists.
    QCOMPARE(records.size(), 2);
    for (const Record& record : records) {
        QCOMPARE(record.category, quantum::app::systemLog().categoryName());
        QCOMPARE(record.type, QtWarningMsg);
    }
    QVERIFY2(records.at(0).message.contains(QStringLiteral("MemAvailable")),
             qPrintable(records.at(0).message));
    QVERIFY2(records.at(1).message.contains(QStringLiteral("cpu line")), qPrintable(records.at(1).message));
    QVERIFY(!service.memoryAvailable());
    QVERIFY(!service.cpuAvailable());
}

void SysMonTest::recordsTheCadenceItAcceptedAndOnlyWhenItChanged()
{
    // The record is how the configured cadence is observable at all: nothing else about a sampling interval
    // is visible from outside the process, and the live test reads this same record out of a running shell to
    // prove `bar.system.sample_interval_ms` reached this service. So what it says, which category it is on
    // and — just as important — when it is *not* written are pinned here.
    SysMonService service;

    const QtMessageHandler previous = qInstallMessageHandler(&captureRecords);
    records.clear();
    service.setSampleIntervalMs(50);
    service.setSampleIntervalMs(50);  // unchanged: nothing to say, and nothing for a listener to re-arm for
    service.setSampleIntervalMs(0);   // refused: a warning, not an acceptance
    qInstallMessageHandler(previous);

    QCOMPARE(records.size(), 2);

    const Record accepted = records.at(0);
    QCOMPARE(accepted.category, quantum::app::systemLog().categoryName());
    QCOMPARE(accepted.type, QtInfoMsg);
    QVERIFY2(accepted.message.contains(QStringLiteral("every 50 ms")), qPrintable(accepted.message));
    // The files it reads are named, because "sampling what, every 50 ms?" is the question the record answers.
    QVERIFY2(accepted.message.contains(QStringLiteral("/proc/stat"))
                 && accepted.message.contains(QStringLiteral("/proc/meminfo")),
             qPrintable(accepted.message));

    const Record refused = records.at(1);
    QCOMPARE(refused.category, quantum::app::systemLog().categoryName());
    QCOMPARE(refused.type, QtWarningMsg);
    QVERIFY2(refused.message.contains(QStringLiteral("refusing")), qPrintable(refused.message));
    QCOMPARE(service.sampleIntervalMs(), 50);
}

void SysMonTest::samplesOnlyWhileItIsActive()
{
    // The real /proc, because this slot is about the cadence and not about the parsing: a directory of this
    // test's own holds a file that does not change, and a file that does not change publishes nothing — which
    // is correct, and is asserted in its own slot above. Only a real machine's counters advance on their own.
    SysMonService service;
    service.setSampleIntervalMs(30);
    QSignalSpy readings(&service, &SysMonService::readingChanged);

    // Nothing is sampled while no bar is on screen, and the claim is about a wake-up rather than about a
    // window in time: what is read here is that no timer is scheduled at all, which is the difference between
    // a hidden bar that costs nothing and one that samples on a cadence nobody is looking at. Asserting only
    // "no reading arrived in the last 150 ms" would pass for a service still armed on a two-second interval,
    // which is what this slot did before the timer was read — the falsifier for it is in the report.
    auto* timer = service.findChild<QTimer*>();
    QVERIFY2(timer != nullptr, "the service has no timer to be armed or not");
    QVERIFY2(!timer->isActive(), "a timer is scheduled while nothing is on screen");
    QTest::qWait(150);
    QCOMPARE(readings.count(), 0);
    QVERIFY(!service.memoryAvailable());

    // Activating reads immediately — memory is on screen at once — and the CPU percentage follows one
    // interval later, because a percentage needs the reading after this one.
    service.setActive(true);
    QVERIFY2(timer->isActive(), "activating scheduled no next reading");
    QVERIFY2(service.memoryAvailable(), "activating did not take a reading");
    QVERIFY2(!service.cpuAvailable(), "a CPU percentage was published from a single reading");
    QTRY_VERIFY_WITH_TIMEOUT(service.cpuAvailable(), 5000);

    // Deactivating stops the timer and forgets the reading, so a bar that is hidden does not leave numbers on
    // screen that were true when it was last shown.
    service.setActive(false);
    QVERIFY2(!timer->isActive(), "deactivating left a timer scheduled");
    QVERIFY(!service.memoryAvailable());
    QVERIFY(!service.cpuAvailable());
    QCOMPARE(service.memoryTotalKb(), qulonglong(0));
    const int afterStopping = readings.count();
    QTest::qWait(150);
    QCOMPARE(readings.count(), afterStopping);
}

void SysMonTest::samplesOnTheIntervalItWasGiven()
{
    // The real /proc, for the reason the slot above gives: what is asserted here is that readings keep
    // arriving while active and stop when it is not, and that needs counters that move.
    SysMonService service;
    service.setSampleIntervalMs(20);
    QCOMPARE(service.sampleIntervalMs(), 20);

    // An interval of zero is refused rather than accepted and never fired: a reading needs a moment to happen
    // in, and a timer that fires continuously is the busy loop this is not.
    service.setSampleIntervalMs(0);
    QCOMPARE(service.sampleIntervalMs(), 20);

    QSignalSpy intervalSpy(&service, &SysMonService::sampleIntervalMsChanged);
    service.setSampleIntervalMs(25);
    QCOMPARE(service.sampleIntervalMs(), 25);
    QCOMPARE(intervalSpy.count(), 1);

    // The interval is a cadence, not a promise about any single reading: what is asserted is that readings
    // keep arriving while active and that they stop when it is not.
    service.setActive(true);
    QSignalSpy readings(&service, &SysMonService::readingChanged);
    QTRY_VERIFY_WITH_TIMEOUT(readings.count() >= 2, 5000);
    service.setActive(false);
    const int atStop = readings.count();
    QTest::qWait(120);
    QCOMPARE(readings.count(), atStop);
}

void SysMonTest::readsTheRealProc()
{
    // The parsers above are only ever handed this test's own strings, and a parser that agrees with its own
    // test agrees with nothing else. So the shell's own configuration is read too — the real /proc, through
    // the real code path — and the reading is checked against the arithmetic that must hold for any machine.
    SysMonService service;
    QCOMPARE(service.sampleIntervalMs(), 2000);
    service.sampleNow();

    QVERIFY2(service.memoryAvailable(), "the machine this test runs on published no memory reading");
    QVERIFY2(service.memoryTotalKb() > 0, "a machine with no memory at all");
    QVERIFY2(service.memoryUsedKb() <= service.memoryTotalKb(), "used memory exceeds total memory");

    QVERIFY2(!service.cpuAvailable(), "a CPU percentage was published from a single reading");
    // Waited, and not because the sampling interval needs a test: `/proc/stat` counts in USER_HZ, which is
    // 100 Hz on every architecture, so two readings taken microseconds apart can legitimately find no jiffy
    // in between — and a percentage of no interval is refused rather than published as zero.
    QTest::qWait(200);
    service.sampleNow();
    QVERIFY2(service.cpuAvailable(), "two readings of the real /proc produced no CPU percentage");
    QVERIFY2(service.cpuPercent() >= 0.0 && service.cpuPercent() <= 100.0,
             qPrintable(QStringLiteral("a share of %1% is not a share of anything")
                            .arg(service.cpuPercent())));
}

QTEST_GUILESS_MAIN(SysMonTest)

#include "sysmon_test.moc"
