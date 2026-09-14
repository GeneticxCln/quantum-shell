#include "niri/NiriReconnect.h"

#include "app/Logging.h"
#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"
#include "niri/NiriProtocol.h"

#include <QDebug>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace quantum::niri {

NiriReconnect::NiriReconnect(QObject* parent)
    : QObject(parent), retry_(new QTimer(this)), deadline_(new QTimer(this)) {
    retry_->setSingleShot(true);
    deadline_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, &NiriReconnect::attemptAttach);
    connect(deadline_, &QTimer::timeout, this, [this] {
        // The attempt is not making progress. That is not one of the ends the connection would have
        // reported on its own, so it is treated as a failed attempt and retried with backoff.
        scheduleRetry(QStringLiteral("the compositor did not answer within %1 ms")
                          .arg(policy_.attemptTimeoutMs));
    });
}

void NiriReconnect::keepAttached(NiriIPC& request) {
    request_ = &request;

    connect(request_, &NiriIPC::connected, this, [this] {
        // A compositor can be a different build after a restart, so what it supports is asked again
        // every time it is attached rather than carried over from the one before.
        request_->detect();
        checkAttached();
    });
    connect(request_, &NiriIPC::disconnected, this,
            [this] { noteLoss(QStringLiteral("niri closed the request connection")); });
    // This is also how a connection attempt to a socket that is not there is reported.
    connect(request_, &NiriIPC::transportError, this, &NiriReconnect::noteLoss);
}

void NiriReconnect::keepAttached(NiriEventStream& stream) {
    stream_ = &stream;

    connect(stream_, &NiriEventStream::streaming, this, &NiriReconnect::checkAttached);
    connect(stream_, &NiriEventStream::disconnected, this,
            [this] { noteLoss(QStringLiteral("niri closed the event stream")); });
    connect(stream_, &NiriEventStream::streamFailed, this, &NiriReconnect::noteLoss);
    connect(stream_, &NiriEventStream::transportError, this, &NiriReconnect::noteLoss);
}

void NiriReconnect::setPolicy(const Policy& policy) {
    policy_ = policy;
}

NiriReconnect::Policy NiriReconnect::policy() const {
    return policy_;
}

void NiriReconnect::setSocketLocator(std::function<QString()> locator) {
    locator_ = std::move(locator);
}

void NiriReconnect::start() {
    running_ = true;
    attachedBefore_ = false;
    lossReported_ = false;
    attempt_ = 0;
    retryCount_ = 0;
    lastDelayMs_ = 0;
    attemptAttach();
}

void NiriReconnect::stop() {
    running_ = false;
    waiting_ = false;
    retry_->stop();
    deadline_->stop();
}

bool NiriReconnect::isAttached() const {
    if (request_ != nullptr && !request_->isConnected()) {
        return false;
    }
    if (stream_ != nullptr && !stream_->isStreaming()) {
        return false;
    }
    // Nothing watched is nothing to be attached to.
    return request_ != nullptr || stream_ != nullptr;
}

bool NiriReconnect::isWaitingForRetry() const {
    return waiting_;
}

int NiriReconnect::attempt() const {
    return attempt_;
}

int NiriReconnect::lastDelayMs() const {
    return lastDelayMs_;
}

QString NiriReconnect::lastSocket() const {
    return lastSocket_;
}

void NiriReconnect::attemptAttach() {
    if (!running_) {
        return;
    }

    retry_->stop();
    waiting_ = false;

    if (isAttached()) {
        // Already up, so there is nothing to attach: this happens when something else connected before
        // the supervisor was started. Reported through the same path as an attempt that succeeded.
        checkAttached();
        return;
    }

    ++attempt_;
    lastSocket_ = locator_ ? locator_() : compositorSocket();
    if (lastSocket_.isEmpty()) {
        // There is no compositor socket to attach to. That is what a restart looks like from here, and
        // also what a shell started outside a niri session looks like, so the retry keeps going and the
        // reason is reported rather than a capability being concluded.
        scheduleRetry(QStringLiteral("no niri IPC socket was found"));
        return;
    }

    if (request_ != nullptr) {
        request_->connectToCompositor(lastSocket_);
    }
    if (stream_ != nullptr) {
        stream_->connectToCompositor(lastSocket_);
    }
    deadline_->start(policy_.attemptTimeoutMs);
}

void NiriReconnect::noteLoss(const QString& reason) {
    if (!running_) {
        return;
    }
    // A live connection also reports protocol complaints through `transportError`. Reconnecting over
    // one of those would turn a report into an outage, so only a connection that is actually down
    // counts as a loss.
    if (isAttached()) {
        return;
    }

    if (attachedBefore_ && !lossReported_) {
        lossReported_ = true;
        // Logged here rather than by whoever connects to the signal: the signal is for the shell to react
        // to, and this record is the compositor restart as it happened, which is the one thing a person
        // reading a log after a restart is looking for.
        qCWarning(quantum::app::niriLog) << "lost the connection to niri:" << reason;
        emit lost(reason);
    }
    scheduleRetry(reason);
}

void NiriReconnect::scheduleRetry(const QString& reason) {
    if (!running_ || waiting_) {
        return;
    }
    deadline_->stop();
    waiting_ = true;
    // The delay is counted in retries, not in attempts. Counting attempts would charge two delays for
    // one step of the backoff: a connection that drops before any attempt has been made, and the
    // attempt that then fails, are both "the first" — so the schedule would start at the first delay
    // twice over instead of doubling from it.
    lastDelayMs_ = delayForRetry(retryCount_);
    ++retryCount_;
    retry_->start(lastDelayMs_);
    qCInfo(quantum::app::niriLog) << "retrying in" << lastDelayMs_ << "ms:" << reason;
    emit retryScheduled(attempt_, lastDelayMs_, reason);
}

void NiriReconnect::checkAttached() {
    if (!running_ || !isAttached()) {
        return;
    }
    deadline_->stop();
    retry_->stop();
    waiting_ = false;
    attempt_ = 0;
    retryCount_ = 0;
    lastDelayMs_ = 0;
    attachedBefore_ = true;
    lossReported_ = false;
    qCInfo(quantum::app::niriLog) << "attached to niri";
    emit attached();
}

int NiriReconnect::delayForRetry(int retry) const {
    // The delay before retry number `retry` (the first is 0): firstDelay * factor^retry, capped.
    // Computed in 64 bits so a long outage cannot overflow its way back to a short delay.
    qint64 delay = policy_.firstDelayMs;
    for (int step = 0; step < retry; ++step) {
        if (delay >= policy_.maximumDelayMs) {
            break;
        }
        delay *= std::max(1, policy_.factor);
    }
    delay = std::min<qint64>(delay, std::max(0, policy_.maximumDelayMs));

    const int jitter = policy_.jitterMs > 0 ? QRandomGenerator::global()->bounded(policy_.jitterMs + 1) : 0;
    return static_cast<int>(std::max<qint64>(0, delay + jitter));
}

}  // namespace quantum::niri
