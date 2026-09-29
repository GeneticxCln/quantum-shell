#pragma once

#include <QObject>
#include <QPointer>
#include <QUrl>

class QQmlComponent;
class QQmlEngine;
class QScreen;
class QTimer;
class QWindow;

namespace quantum::config {
class Config;
}
namespace quantum::audio {
class PipeWireService;
}

namespace quantum::app {

// The on-screen display for the volume: one surface per output, created when the volume or the mute of the
// sink being followed is adjusted and withdrawn when `[bar.osd] timeout_ms` has passed since the last change.
//
// What it says is not this class's and not stored anywhere in it: the surface binds to `PipeWireService`,
// so a second adjustment while it is up redraws it from the daemon's own reading, and the class's job is the
// lifetime — which outputs have one, when the clock starts again and when they are destroyed. That is also
// why it is C++ and not QML: a surface that comes and goes, per output, with a clock, is lifetime.
//
// It is driven by `volumeAdjusted` and by nothing else. The service's readings without an adjustment — the
// first one after a start, a returning daemon, a switch of the default sink — are not announced, because a
// display appearing for something nobody did would be a display that lies about what happened.
class OsdHost : public QObject
{
    Q_OBJECT

public:
    // All three must outlive this object and none is owned.
    explicit OsdHost(quantum::audio::PipeWireService& audio, quantum::config::Config& config,
                     QQmlEngine& engine, const QUrl& osdUrl, QObject* parent = nullptr);

    // Whether the component was readable, turned into a failed process by the composition root for the reason
    // the toast's is: a shell that reports volume changes it cannot draw is not the one that was asked for.
    bool ready() const { return componentError_.isEmpty(); }
    QString componentError() const { return componentError_; }

    // The surfaces that are up, in the order their outputs arrived.
    QList<QWindow*> windows() const;

private:
    struct Osd
    {
        QPointer<QScreen> screen;
        QPointer<QWindow> window;
    };

    void handleAdjusted();
    void handleShowChanged();
    void show();
    void withdraw();
    void handleScreenRemoved(QScreen* screen);

    QQmlComponent* component_ = nullptr;
    QString componentError_;
    QList<Osd> osds_;
    QTimer* timer_ = nullptr;
    quantum::config::Config* config_ = nullptr;
};

}  // namespace quantum::app
