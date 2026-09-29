// What the IPC can reach in this shell, answered from the same objects the bar is drawn from.
//
// QUANTUM_SHELL.md § IPC requires that the socket expose **only the same `Shell.*` capabilities as the QML
// API**, never arbitrary C++ entry points. This class is that requirement in code: it has three methods,
// each of which reads something QML already has, and there is nothing else it can do. A verb added to the
// IPC that reaches further than the QML API can would have to be a method here, which is a visible change
// rather than a new case in a switch.
//
// It is deliberately thin, and thin in a way that is worth stating: this class holds no state, makes no
// decision about what a value means, and formats nothing. The keys it puts in `state()` are `NiriService`'s
// own property names, the value of a configuration key comes from the schema's resolver, and hiding the bar
// is the bar window's own `setVisible`. Anything more here would be a second implementation of a value that
// already exists in one place.
#pragma once

#include "ipc/IPCServer.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QString>

#include <optional>

class QWindow;

namespace quantum::config {
class Config;
}

namespace quantum::niri {
class NiriService;
}

namespace quantum::app {

class BarHost;

class ShellCapabilities : public quantum::ipc::Capabilities
{
public:
    // All three must outlive this object. `bars` may be null — the QML is created by the engine, and a
    // shell whose bar failed to load still has state to report — in which case `toggleBar` answers that
    // there are no bars rather than pretending to have hidden one.
    ShellCapabilities(quantum::niri::NiriService& service, quantum::config::Config& config,
                      BarHost* bars);

    QJsonObject state() const override;
    std::optional<QJsonValue> configValue(const QString& path) const override;
    bool toggleBar() override;

private:
    quantum::niri::NiriService& service_;
    quantum::config::Config& config_;

    // Not owned, and not a weak pointer either: the host is created by the composition root and lives as
    // long as the shell does, unlike the windows it holds. Those are its own business — it follows them
    // going away — which is why what is kept here is the host rather than a list of windows that could
    // name one that is gone.
    BarHost* bars_ = nullptr;
};

}  // namespace quantum::app
