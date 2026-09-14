// Keeps the shell attached to niri across a compositor restart.
//
// niri is a session process, not a daemon. When it restarts, every connection to it is gone at once,
// its IPC socket is recreated under a new name (niri's socket name carries the process id — see
// `niriSocketFiles`), and everything the shell had learned about the session describes a compositor
// that no longer exists. This class owns the two things that recovery needs: the retry policy for both
// connections, and finding the socket again on every attempt.
//
// The retry policy is exponential backoff with jitter, which is the one kind of timer the project
// allows besides debounce and animation: nothing is polled while a connection is up, and the only work
// is an attempt after a connection was actually lost.
#pragma once

#include <QObject>
#include <QString>

#include <functional>

class QTimer;

namespace quantum::niri {

class NiriEventStream;
class NiriIPC;

class NiriReconnect : public QObject {
    Q_OBJECT

public:
    struct Policy {
        // The delay before the first retry.
        int firstDelayMs = 250;
        int factor = 2;
        // The delay never grows past this: a compositor that is down for a long time is retried
        // patiently rather than in a tight loop.
        int maximumDelayMs = 8000;
        // Random extra delay, 0..jitterMs, added to each retry so several shell processes starting at
        // the same moment do not all come back in the same instant. Zero makes the schedule exact,
        // which is what a test asserting the sequence wants.
        int jitterMs = 100;
        // One attempt may take this long before it is written off. A Unix socket refuses a connection
        // promptly, so this guards against a compositor that accepts a connection and never answers
        // rather than being the normal path.
        int attemptTimeoutMs = 5000;
    };

    explicit NiriReconnect(QObject* parent = nullptr);

    // The connections to keep attached. Both endpoints are attached when the request connection is
    // connected and the event stream is acknowledged and streaming, which is what `attached()` reports.
    void keepAttached(NiriIPC& request);
    void keepAttached(NiriEventStream& stream);

    void setPolicy(const Policy& policy);
    Policy policy() const;

    // How an attempt finds the compositor. The default is `compositorSocket()`: `$NIRI_SOCKET` while
    // that path exists, rediscovered from niri's naming rule otherwise. A test that runs a compositor
    // of its own replaces it with one that finds that compositor.
    void setSocketLocator(std::function<QString()> locator);

    // Attaches now, then keeps attaching until `stop()`.
    void start();
    // Stops retrying. The connections are left as they are; this stops the supervising, not the sockets.
    void stop();

    bool isAttached() const;
    bool isWaitingForRetry() const;
    // Attempts made since the last successful attach, and 0 while attached.
    int attempt() const;
    // The delay of the retry that is waiting, or the last one that was tried.
    int lastDelayMs() const;
    // The socket the last attempt used, empty when there was none to try.
    QString lastSocket() const;

signals:
    // Every watched connection is up, and the compositor has been (re)detected.
    void attached();
    // A connection that was attached is gone. Emitted once per outage, not once per failed attempt.
    void lost(const QString& reason);
    void retryScheduled(int attempt, int delayMs, const QString& reason);

private:
    void attemptAttach();
    void noteLoss(const QString& reason);
    void scheduleRetry(const QString& reason);
    void checkAttached();
    int delayForRetry(int retry) const;

    NiriIPC* request_ = nullptr;
    NiriEventStream* stream_ = nullptr;
    QTimer* retry_ = nullptr;     // single-shot backoff timer
    QTimer* deadline_ = nullptr;  // single-shot per-attempt deadline
    std::function<QString()> locator_;

    Policy policy_;
    bool running_ = false;
    bool waiting_ = false;
    bool attachedBefore_ = false;
    bool lossReported_ = false;
    int attempt_ = 0;
    // Retries scheduled since the last successful attach. The backoff is computed from this rather than
    // from `attempt_`, because a loss and the attempt that follows it are separate steps.
    int retryCount_ = 0;
    int lastDelayMs_ = 0;
    QString lastSocket_;
};

}  // namespace quantum::niri
