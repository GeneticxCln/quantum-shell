#include "app/Logging.h"

#include <QtGlobal>

namespace quantum::app {

Q_LOGGING_CATEGORY(shellLog, "quantum.shell")
Q_LOGGING_CATEGORY(niriLog, "quantum.shell.niri")
Q_LOGGING_CATEGORY(configLog, "quantum.shell.config")
Q_LOGGING_CATEGORY(ipcLog, "quantum.shell.ipc")
Q_LOGGING_CATEGORY(waylandLog, "quantum.shell.wayland")
Q_LOGGING_CATEGORY(systemLog, "quantum.shell.system")
Q_LOGGING_CATEGORY(audioLog, "quantum.shell.audio")
Q_LOGGING_CATEGORY(networkLog, "quantum.shell.network")
Q_LOGGING_CATEGORY(batteryLog, "quantum.shell.battery")
Q_LOGGING_CATEGORY(mediaLog, "quantum.shell.media")

namespace Logging {

QString pattern()
{
    // The user's own channel for the same decision is the environment variable. Reading it here rather than
    // setting the pattern unconditionally is what makes it a fallback instead of an override: the shell says
    // what its records look like only when nobody else has.
    if (qEnvironmentVariableIsSet("QT_MESSAGE_PATTERN"))
        return qEnvironmentVariable("QT_MESSAGE_PATTERN");
    return QString::fromLatin1(DefaultPattern);
}

void install()
{
    qSetMessagePattern(pattern());
}

QString format(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    return qFormatLogMessage(type, context, message);
}

}  // namespace Logging
}  // namespace quantum::app
