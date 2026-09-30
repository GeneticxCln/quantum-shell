#include "app/ShellCapabilities.h"

#include "app/BarHost.h"
#include "app/ControlCenterService.h"
#include "apps/LauncherService.h"
#include "config/Config.h"
#include "niri/NiriService.h"
#include <QWindow>

namespace quantum::app {

ShellCapabilities::ShellCapabilities(quantum::niri::NiriService& service,
                                     quantum::config::Config& config,
                                     BarHost* bars, quantum::apps::LauncherService* launcher,
                                     ControlCenterService* controlCenter)
    : service_(service)
    , config_(config)
    , bars_(bars)
    , launcher_(launcher)
    , controlCenter_(controlCenter)
{
}

QJsonObject ShellCapabilities::state() const
{
    // One key per `NiriService` property, under the property's own name. The QML API and the IPC surface are
    // the same capabilities, so they are the same names: a widget that reads
    // `NiriService.keyboardLayout.currentName` and a script that reads `keyboardLayout.currentName` out of
    // `qsctl state` are reading one value, and the pairing is checked rather than asserted —
    // `ipc-capabilities-test` compares these keys against the service's meta-object, so a property renamed
    // there fails to build until it is renamed here too.
    //
    // Nothing is filtered or reshaped on the way through. An empty list here is the same empty list QML
    // sees, and `connected` is what says whether that means "no niri" or "no workspaces".
    QJsonObject state;
    state.insert(QStringLiteral("workspaces"), QJsonValue::fromVariant(service_.workspaces()));
    state.insert(QStringLiteral("focusedWindow"), QJsonValue::fromVariant(service_.focusedWindow()));
    state.insert(QStringLiteral("windows"), QJsonValue::fromVariant(service_.windows()));
    state.insert(QStringLiteral("outputs"), QJsonValue::fromVariant(service_.outputs()));
    state.insert(QStringLiteral("keyboardLayout"), QJsonValue::fromVariant(service_.keyboardLayout()));
    state.insert(QStringLiteral("overviewOpen"), service_.overviewOpen());
    state.insert(QStringLiteral("connected"), service_.connected());
    return state;
}

std::optional<QJsonValue> ShellCapabilities::configValue(const QString& path) const
{
    // Resolved by the schema, which is the only place that knows which keys exist and what a value is once
    // it has been validated. An unknown path is nothing here, and the server turns that into a refusal
    // naming the path rather than into a null value a script would have to detect.
    const std::optional<QVariant> value = quantum::config::configValueForPath(config_.values(), path);
    if (!value.has_value())
        return std::nullopt;
    return QJsonValue::fromVariant(*value);
}

bool ShellCapabilities::toggleBar()
{
    if (bars_ == nullptr)
        return false;

    // Every bar, through the host that holds them: the shell's bars are one surface per output, so hiding
    // "the bar" is hiding all of them, and a verb that moved only the first would leave a monitor lit.
    // The state answered is the bars' own visibility after the move rather than a flag of ours to keep in
    // step with it — the compositor's answer is the surface being in or out of its layer list, and a second
    // copy of the state here could disagree with the compositor and be believed.
    return bars_->toggleAll();
}

bool ShellCapabilities::toggleLauncher()
{
    if (launcher_ == nullptr)
        return false;
    // The service's own toggle, the one the launcher's surface follows: the answer is the service's state after
    // the move, not a flag kept here.
    return launcher_->toggle();
}

bool ShellCapabilities::toggleControlCenter()
{
    if (controlCenter_ == nullptr)
        return false;
    return controlCenter_->toggle();
}

}  // namespace quantum::app
