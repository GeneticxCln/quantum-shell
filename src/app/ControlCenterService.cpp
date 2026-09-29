#include "app/ControlCenterService.h"

#include "QmlModule.h"

#include <QQmlEngine>

namespace quantum::app {

ControlCenterService::ControlCenterService(QObject* parent) : QObject(parent) {}

void ControlCenterService::setOpen(bool open)
{
    if (open_ == open)
        return;
    open_ = open;
    emit openChanged();
}

bool ControlCenterService::toggle()
{
    setOpen(!open_);
    return open_;
}

void ControlCenterService::registerQmlSingleton(ControlCenterService& service)
{
    qmlRegisterSingletonInstance(quantum::qml::ModuleUri, quantum::qml::ModuleMajorVersion,
                                 quantum::qml::ModuleMinorVersion, QmlTypeName, &service);
}

}  // namespace quantum::app
