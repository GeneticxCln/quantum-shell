#include "system/ClockService.h"

#include "QmlModule.h"
#include "app/Logging.h"

#include <QDir>
#include <QFileInfo>
#include <QQmlEngine>
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>
#include <limits>

#include <sys/timerfd.h>
#include <unistd.h>

namespace quantum::system {

ClockService::ClockService(const QString& localtimePath, QObject* parent)
    : QObject(parent)
    , localtimePath_(localtimePath)
{
    clockFd_ = ::timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    if (clockFd_ < 0) {
        qCWarning(quantum::app::systemLog) << "the real-time clock cannot be watched for being set, so a resume"
                                            << "from suspend will not be noticed:" << std::strerror(errno);
    } else {
        clockNotifier_ = new QSocketNotifier(clockFd_, QSocketNotifier::Read, this);
        connect(clockNotifier_, &QSocketNotifier::activated, this, &ClockService::handleClockFd);
        armClockWatch();
    }

    connect(&zoneWatcher_, &QFileSystemWatcher::fileChanged, this, &ClockService::handleZoneChanged);
    connect(&zoneWatcher_, &QFileSystemWatcher::directoryChanged, this, &ClockService::handleZoneChanged);
    zoneIdentity_ = zoneIdentity();
    rewatchZone(false);
}

ClockService::~ClockService()
{
    if (clockFd_ >= 0)
        ::close(clockFd_);
}

void ClockService::armClockWatch()
{
    // An absolute expiry at the end of time: the timer is never meant to fire, only to be cancelled. The flag
    // is what makes a `read` fail with ECANCELED when the clock is set; it is only valid together with
    // TFD_TIMER_ABSTIME, which is why both are here.
    itimerspec spec{};
    spec.it_value.tv_sec = std::numeric_limits<time_t>::max();
    if (::timerfd_settime(clockFd_, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &spec, nullptr) < 0) {
        qCWarning(quantum::app::systemLog) << "the real-time clock cannot be watched for being set:"
                                            << std::strerror(errno);
        clockNotifier_->setEnabled(false);
        ::close(clockFd_);
        clockFd_ = -1;
    }
}

void ClockService::handleClockFd()
{
    uint64_t expirations = 0;
    const ssize_t got = ::read(clockFd_, &expirations, sizeof(expirations));
    if (got < 0 && errno == ECANCELED) {
        // The clock was set. The kernel counts clock-set events against each read, so the next one after this
        // is reported without the timer being armed again (measured: two `clock_settime` calls in a row are
        // both reported, in `clock-test`).
        qCInfo(quantum::app::systemLog) << "the real-time clock was set (a resume from suspend, or the time was changed)";
        emit clockChanged();
        return;
    }
    if (got < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    // An expiry, or any other error: neither is a thing this timer is armed to produce. It is reported rather
    // than swallowed.
    qCWarning(quantum::app::systemLog) << "the real-time clock watch answered something unexpected"
                                        << (got < 0 ? std::strerror(errno) : "an expiry");
}

void ClockService::handleZoneChanged()
{
    const QString identity = zoneIdentity();
    if (identity == zoneIdentity_) {
        // Not a zone change, but the event may still have removed what was being watched, so the set is
        // re-derived as the configuration watcher's is.
        rewatchZone(false);
        return;
    }
    zoneIdentity_ = identity;
    // Forced: the path is the same string as before, but a watch on a symlink is on what it pointed at when it was
    // added, so after a switch the old target is still the one being watched and the zone now in force is not.
    rewatchZone(true);
    qCInfo(quantum::app::systemLog) << "the timezone file changed:" << localtimePath_;
    emit clockChanged();
}

QString ClockService::zoneIdentity() const
{
    const QFileInfo link(localtimePath_);
    return link.symLinkTarget() + QLatin1Char('|') + link.canonicalFilePath() + QLatin1Char('|')
           + QString::number(QFileInfo(link.canonicalFilePath()).lastModified().toMSecsSinceEpoch());
}

void ClockService::rewatchZone(bool force)
{
    QStringList wanted;
    if (QFileInfo::exists(localtimePath_))
        wanted.append(localtimePath_);
    QDir directory = QFileInfo(localtimePath_).absoluteDir();
    while (!directory.exists()) {
        const QString before = directory.absolutePath();
        if (!directory.cdUp() || directory.absolutePath() == before)
            break;
    }
    if (directory.exists())
        wanted.append(directory.absolutePath());
    wanted.removeDuplicates();

    QStringList current = zoneWatcher_.files();
    current.append(zoneWatcher_.directories());
    current.sort();
    wanted.sort();
    if (!force && current == wanted)
        return;
    if (!current.isEmpty())
        zoneWatcher_.removePaths(current);
    for (const QString& path : wanted)
        zoneWatcher_.addPath(path);
}

void ClockService::registerQmlSingleton(ClockService& service)
{
    qmlRegisterSingletonInstance(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion,
                                 quantum::qml::ModuleMinorVersion, QmlTypeName, &service);
}

}  // namespace quantum::system
