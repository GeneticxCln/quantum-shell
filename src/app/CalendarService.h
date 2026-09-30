// The calendar panel's open/closed state, as QML sees it.
//
// The same shape as `ControlCenterService`, for the same reason: what the calendar *shows* is the date, which the
// system owns and QML reads, so there is nothing for a service to hold but whether the panel is up — and the clock that
// toggles it, the panel's own Escape key and the host that draws the surface all have to agree about that.
#pragma once

#include <QObject>

namespace quantum::app {

class CalendarService : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool open READ isOpen WRITE setOpen NOTIFY openChanged)

public:
    explicit CalendarService(QObject* parent = nullptr);

    bool isOpen() const { return open_; }
    void setOpen(bool open);

    // Opens the panel if it is closed and closes it if it is open, and reports whether it is open now.
    Q_INVOKABLE bool toggle();

    static void registerQmlSingleton(CalendarService& service);
    inline static constexpr auto QmlTypeName = "CalendarService";

Q_SIGNALS:
    void openChanged();

private:
    bool open_ = false;
};

}  // namespace quantum::app
