// The service that tells the clock when it has been moved from under it: the real-time clock being set, and the
// timezone changing. Both are real events on real files and a real timerfd — nothing here is a stand-in for the
// kernel — because the whole claim is that an event arrives.
//
// The timezone half runs against a zone file of the test's own, in a directory it owns, so it can replace it the
// way `timedatectl` does and can do it under any user. The clock half needs `CAP_SYS_TIME`, because the only way to
// make the kernel report the clock as set is to set it: it sets the clock to the value it just read, so the step is
// the few microseconds between the two calls and no more. Where the process may not do that (an unprivileged
// developer session, most CI) that one slot skips with the reason and the rest of the binary still runs; on a
// machine where it can, the slot is the proof that a resume from suspend, which the kernel reports the same way,
// reaches the shell.
#include "system/ClockService.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <cstring>
#include <filesystem>

#include <time.h>

using quantum::system::ClockService;

namespace {

constexpr int settleMs = 5000;
// How long a slot waits for an event that must not come. Long enough that a late inotify or timerfd wake would
// have landed, short enough not to dominate the binary.
constexpr int quietMs = 300;

bool write(const QString& path, const QByteArray& text)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(text) == text.size();
}

}  // namespace

class ClockTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void aQuietServiceSaysNothing();
    void aZoneSwitchedTheWayTimedatectlDoesItIsNoticed();
    void aZoneFileEditedInPlaceIsNoticed();
    void aNeighbouringFileChangingIsNotAZoneChange();
    void aZoneThatDidNotExistYetIsNoticedWhenItAppears();
    void theRealTimeClockBeingSetIsNoticedEveryTime();

private:
    QString zonePath() const { return dir_.filePath(QStringLiteral("localtime")); }
    QString zoneFile(const QString& name) const { return dir_.filePath(name); }
    // `localtime` as a symlink to `target`, replaced atomically, which is what a zone switch does.
    bool pointZoneAt(const QString& target);

    QTemporaryDir dir_;
};

void ClockTest::init()
{
    QVERIFY(dir_.isValid());
    // A fresh directory each slot's worth of content: the files are removed rather than the directory replaced,
    // so the path the service was built with stays valid.
    for (const QString& name : {QStringLiteral("localtime"), QStringLiteral("localtime.new"), QStringLiteral("zone-a"),
                                QStringLiteral("zone-b"), QStringLiteral("neighbour")})
        QFile::remove(dir_.filePath(name));
    QVERIFY(write(zoneFile(QStringLiteral("zone-a")), "TZif-a"));
    QVERIFY(write(zoneFile(QStringLiteral("zone-b")), "TZif-b"));
    QVERIFY(pointZoneAt(zoneFile(QStringLiteral("zone-a"))));
}

bool ClockTest::pointZoneAt(const QString& target)
{
    std::error_code error;
    const std::filesystem::path staged = zonePath().toStdString() + ".new";
    std::filesystem::remove(staged, error);
    std::filesystem::create_symlink(target.toStdString(), staged, error);
    if (error)
        return false;
    std::filesystem::rename(staged, zonePath().toStdString(), error);
    return !error;
}

void ClockTest::aQuietServiceSaysNothing()
{
    ClockService service(zonePath());
    QSignalSpy spy(&service, &ClockService::clockChanged);
    QTest::qWait(quietMs);
    QCOMPARE(spy.count(), 0);
}

void ClockTest::aZoneSwitchedTheWayTimedatectlDoesItIsNoticed()
{
    ClockService service(zonePath());
    QSignalSpy spy(&service, &ClockService::clockChanged);

    // The symlink is replaced by a rename, so the zone the service first watched is no longer the one in force:
    // the directory watch is what reports the switch.
    QVERIFY(pointZoneAt(zoneFile(QStringLiteral("zone-b"))));
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, settleMs);

    // And the watch was moved to the zone now in force, or a change to *that* file would go unseen: editing the
    // new target in place is what shows it. The pause is for the modification time the change is recognised by.
    QTest::qWait(1100);
    QVERIFY(write(zoneFile(QStringLiteral("zone-b")), "TZif-b-updated"));
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 2, settleMs);
}

void ClockTest::aZoneFileEditedInPlaceIsNoticed()
{
    ClockService service(zonePath());
    QSignalSpy spy(&service, &ClockService::clockChanged);

    // The zone a link resolves to being rewritten (a tzdata update replaces the file's contents): the link is the
    // same and the file is not, and the file's modification time is what says so.
    QTest::qWait(1100);  // a modification time is compared at a resolution that a same-instant rewrite would miss
    QVERIFY(write(zoneFile(QStringLiteral("zone-a")), "TZif-a-updated"));
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, settleMs);
}

void ClockTest::aNeighbouringFileChangingIsNotAZoneChange()
{
    ClockService service(zonePath());
    QSignalSpy spy(&service, &ClockService::clockChanged);

    // The directory watch reports every entry that appears beside the zone, and `/etc` is a directory things
    // appear in all day. None of that is a zone change, and reporting it would refresh the clock for nothing.
    QVERIFY(write(zoneFile(QStringLiteral("neighbour")), "hello"));
    QTest::qWait(quietMs);
    QFile::remove(zoneFile(QStringLiteral("neighbour")));
    QTest::qWait(quietMs);
    QCOMPARE(spy.count(), 0);
}

void ClockTest::aZoneThatDidNotExistYetIsNoticedWhenItAppears()
{
    QFile::remove(zonePath());
    ClockService service(zonePath());
    QSignalSpy spy(&service, &ClockService::clockChanged);

    QVERIFY(pointZoneAt(zoneFile(QStringLiteral("zone-b"))));
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, settleMs);
}

void ClockTest::theRealTimeClockBeingSetIsNoticedEveryTime()
{
    ClockService service(zonePath());
    QVERIFY2(service.watchingClock(), "the kernel refused the timerfd, so the clock cannot be watched at all");
    QSignalSpy spy(&service, &ClockService::clockChanged);

    const auto setClockToNow = [] {
        timespec now{};
        if (::clock_gettime(CLOCK_REALTIME, &now) != 0)
            return errno;
        return ::clock_settime(CLOCK_REALTIME, &now) == 0 ? 0 : errno;
    };
    const int first = setClockToNow();
    if (first == EPERM)
        QSKIP("setting the real-time clock needs CAP_SYS_TIME, which this process does not have: the one thing "
              "this slot proves (that a clock being set reaches the service) cannot be exercised here");
    QCOMPARE(first, 0);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, settleMs);

    // A second step is noticed as well as the first: a watch that reported once and then went deaf would be
    // wrong for every resume after the first, which is the failure this second half exists to catch.
    QCOMPARE(setClockToNow(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 2, settleMs);
}

QTEST_GUILESS_MAIN(ClockTest)

#include "clock_test.moc"
