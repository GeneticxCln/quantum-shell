#include "app/CalendarService.h"

#include "QmlModule.h"

#include <QQmlEngine>

namespace quantum::app {

CalendarService::CalendarService(QObject* parent) : QObject(parent) {}

void CalendarService::setOpen(bool open)
{
    if (open_ == open)
        return;
    open_ = open;
    emit openChanged();
}

bool CalendarService::toggle()
{
    setOpen(!open_);
    return open_;
}

void CalendarService::registerQmlSingleton(CalendarService& service)
{
    qmlRegisterSingletonInstance(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion,
                                 quantum::qml::ModuleMinorVersion, QmlTypeName, &service);
}

}  // namespace quantum::app
