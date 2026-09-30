// Tells the clock when the time it is showing has been moved from under it.
//
// `qml/Clock.qml` re-arms a single-shot timer for the next minute boundary, and that timer runs on the monotonic
// clock. Two things make the boundary it computed wrong, and neither is visible to a timer:
//
//   * **The wall clock was set.** After a suspend the kernel steps the real-time clock forward by the time
//     spent asleep, and `settimeofday`, `date -s` and an NTP step do the same — the monotonic clock, which does
//     not count suspended time, has not moved, so the timer fires late by however far the clock jumped: up to a
//     minute of a stale time after a resume, and more after a large step.
//   * **The zone changed.** Switching the timezone moves local time without touching the clock at all.
//
// Both have an event source, so this service uses it rather than asking:
//
//   * a `timerfd` on `CLOCK_REALTIME`, armed at an absolute time no clock reaches, with
//     `TFD_TIMER_CANCEL_ON_SET`. It never expires; the kernel instead completes a `read` with `ECANCELED`
//     whenever the real-time clock is set discontinuously, which is the documented use of that flag — and it
//     covers a resume from suspend, since the kernel treats the step taken on resume as the clock being set
//     (verified here with a `clock_settime`, below and in `clock-test`);
//   * a watch on `/etc/localtime` and its directory, which is what a zone switch rewrites (`timedatectl
//     set-timezone` replaces the symlink, so the file's own watch dies and the directory is what reports it).
//
// `clockChanged` is the one signal, and what a listener does about it is its own: `Clock.qml` reads the time and
// re-arms its boundary. The service holds no time and formats none, because a string for a person to read is the
// widget's, and it adds no timer: an idle shell is woken by neither of these sources until the clock or the zone
// actually moves.
#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>

class QSocketNotifier;

namespace quantum::system {

class ClockService : public QObject
{
    Q_OBJECT

public:
    // `localtimePath` is the file whose replacement means the zone changed, injectable for the reason every path
    // in this shell is: a test needs a file of its own rather than the machine's.
    explicit ClockService(const QString& localtimePath = QStringLiteral("/etc/localtime"), QObject* parent = nullptr);
    ~ClockService() override;

    // Whether the real-time clock is being watched. False when the kernel refused the timerfd, which is logged
    // once with the reason; the zone watch works either way.
    bool watchingClock() const { return clockFd_ >= 0; }

    static void registerQmlSingleton(ClockService& service);

    inline static constexpr auto QmlTypeName = "ClockService";

Q_SIGNALS:
    // The wall clock was set (including by a resume from suspend) or the timezone changed.
    void clockChanged();

private:
    void armClockWatch();
    void handleClockFd();
    void handleZoneChanged();
    // Points the watcher at the zone file and the deepest directory that exists above it.
    // `force` re-adds the paths even when the set is unchanged, which a zone switch needs: see the caller.
    void rewatchZone(bool force);
    // What names the zone right now: where the link points, what that resolves to and when that file last changed.
    // The directory watch reports every change to its neighbours too (`/etc` is busy), so an event is only a zone
    // change when this differs from the last one seen.
    QString zoneIdentity() const;

    int clockFd_ = -1;
    QSocketNotifier* clockNotifier_ = nullptr;
    QString localtimePath_;
    QString zoneIdentity_;
    QFileSystemWatcher zoneWatcher_;
};

}  // namespace quantum::system
