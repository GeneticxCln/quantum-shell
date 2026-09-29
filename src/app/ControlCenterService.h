// The control centre's open/closed state, as QML and the IPC both see it.
//
// It is deliberately this small. What the control centre *shows* and *does* belongs to the services it is drawn
// from — the volume and mute are `PipeWireService`'s, Do Not Disturb is `NotificationService`'s, transport is
// `MediaService`'s — and duplicating any of that here would be a second copy of a fact that already has an owner.
// What has no owner yet is whether the panel is up, and three things need to agree about it: the surface (which
// follows it), the panel's own Escape key (which writes it) and the IPC verb a compositor key binding runs
// (which toggles it). So this is the one place that answer lives, the way `LauncherService.open` is the
// launcher's.
#pragma once

#include <QObject>

QT_BEGIN_NAMESPACE
class QJSEngine;
class QQmlEngine;
QT_END_NAMESPACE

namespace quantum::app {

class ControlCenterService : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool open READ isOpen WRITE setOpen NOTIFY openChanged)

public:
    explicit ControlCenterService(QObject* parent = nullptr);

    bool isOpen() const { return open_; }
    void setOpen(bool open);

    // Opens the panel if it is closed and closes it if it is open, and reports whether it is open now.
    Q_INVOKABLE bool toggle();

    static void registerQmlSingleton(ControlCenterService& service);
    inline static constexpr auto QmlTypeName = "ControlCenterService";

Q_SIGNALS:
    void openChanged();

private:
    bool open_ = false;
};

}  // namespace quantum::app
