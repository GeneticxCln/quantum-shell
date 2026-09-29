#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QQmlComponent;
class QQmlEngine;
class QScreen;
class QWindow;

namespace quantum::dbus {
class NotificationService;
}

namespace quantum::app {

// The notification history panel: one surface, on the primary output, that exists while the service says the
// panel is open.
//
// It is the counterpart of `ToastHost` for a surface a person asks for rather than one a notification causes,
// and it is a class rather than QML for the same reason: the lifetime is the business. The open/closed state
// is the service's (`notificationHistoryOpen`), because the readout that toggles it and this host that draws it
// are separate objects and the service is the one thing both see. The host follows that property, and it also
// writes it: when the compositor closes the surface, or the output it is on goes away, the panel is not open
// any more and the service is told so, so a later toggle opens it instead of doing nothing.
class HistoryHost : public QObject
{
    Q_OBJECT

public:
    // Both must outlive this object. The panel is created against `engine` from `historyUrl`.
    explicit HistoryHost(quantum::dbus::NotificationService& service, QQmlEngine& engine, const QUrl& historyUrl,
                         QObject* parent = nullptr);

    // Whether the component was readable, and the reason when it was not, for the composition root's decision
    // about a shell whose panel cannot be drawn.
    bool ready() const { return componentError_.isEmpty(); }
    QString componentError() const { return componentError_; }

    // The panel, or null while it is not open.
    QWindow* window() const;

private:
    void handleOpenChanged();
    void handleScreenRemoved(QScreen* screen);
    void open();
    void close();

    quantum::dbus::NotificationService* service_ = nullptr;
    QQmlComponent* component_ = nullptr;
    QString componentError_;
    QPointer<QWindow> window_;
    QPointer<QScreen> screen_;
};

}  // namespace quantum::app
