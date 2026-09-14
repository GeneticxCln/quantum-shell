// The niri state model as QML sees it.
//
// `NiriState` is a C++ model: it holds `NiriWorkspace`, `NiriWindow` and `NiriOutput` values, which
// QML cannot read, and it signals that something changed rather than what the value became. QML needs
// the opposite, so this class is the one place that translation happens — plain QVariant values behind
// Q_PROPERTY, each with a NOTIFY signal a binding can follow. Nothing here decides layout, size,
// colour or animation, and nothing reaches a socket: it reads `NiriState` and nothing else.
//
// Every property has a binding it exists for, and the list is that short on purpose — a property with
// no consumer is surface nobody asked for:
//
//   workspaces       the workspace strip: one entry per workspace, in the model's display order
//   focusedWindow    a title widget's app_id and title for the focused window
//   outputs          per-monitor widgets: connector name, and the logical size and fractional scale
//   keyboardLayout   the keyboard layout indicator
//   overviewOpen     a bar that hides itself or changes while the overview is up
//   connected        the honest empty state: with no compositor, the lists above are empty, and this
//                    is what tells a widget that "empty" means "no niri" rather than "no workspaces"
//
// Change signals fire only when the value QML would read actually changed. `NiriState` already
// suppresses events carrying no news, but its signals are coarser than these properties — every
// workspace event wakes `workspacesChanged` — so each value is compared before its signal is emitted
// rather than relayed. A binding that re-evaluates on an event that changed nothing is not wrong, but
// it is work the model can do once instead of every delegate doing it.
//
// Names are camelCase like the C++ model's own accessors, not the snake_case of niri's JSON: QML
// property naming is what a binding is written in, and the mapping is mechanical —
//
//   id/idx/name/output/is_active/is_focused/is_urgent/active_window_id  ->  id/idx/name/output/
//                                                                           isActive/isFocused/
//                                                                           isUrgent/activeWindowId
//
// An absent value is an empty map or an empty list, never a plausible default: `focusedWindow` is
// empty when no window is focused and `keyboardLayout` is empty until the compositor reports the
// layouts. A widget can therefore tell "nothing there" from a real reading, because a real reading
// always has keys.
#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace quantum::niri {

class NiriEventStream;
class NiriState;

class NiriService : public QObject {
    Q_OBJECT

public:
    // `state` and `stream` must outlive this object. The stream is read only for whether it is
    // subscribed, which is what `connected` reports; the state is where every other value comes from.
    explicit NiriService(NiriState& state, NiriEventStream& stream, QObject* parent = nullptr);

    Q_PROPERTY(QVariantList workspaces READ workspaces NOTIFY workspacesChanged)
    Q_PROPERTY(QVariantMap focusedWindow READ focusedWindow NOTIFY focusedWindowChanged)
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY outputsChanged)
    Q_PROPERTY(QVariantMap keyboardLayout READ keyboardLayout NOTIFY keyboardLayoutChanged)
    Q_PROPERTY(bool overviewOpen READ overviewOpen NOTIFY overviewOpenChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

    QVariantList workspaces() const;
    QVariantMap focusedWindow() const;
    QVariantList outputs() const;
    QVariantMap keyboardLayout() const;
    bool overviewOpen() const;
    bool connected() const;

    // Registers `service` so a component can import the module and bind to it:
    //
    //     import QuantumShell 1.0
    //     Text { text: NiriService.keyboardLayout.currentName }
    //
    // The instance stays owned by C++; QML only reads it. The module URI and type name are the two
    // public names this file introduces, and the first QML file written decides whether they fit.
    static void registerQmlSingleton(NiriService& service);

signals:
    void workspacesChanged();
    void focusedWindowChanged();
    void outputsChanged();
    void keyboardLayoutChanged();
    void overviewOpenChanged();
    void connectedChanged();

private:
    void refreshWorkspaces();
    void refreshFocusedWindow();
    void refreshOutputs();
    void refreshKeyboardLayout();
    void refreshOverview();
    void refreshConnected();

    NiriState& state_;
    NiriEventStream& stream_;

    // The last value each property reported, so an event that changes nothing emits nothing.
    QVariantList workspaces_;
    QVariantMap focusedWindow_;
    QVariantList outputs_;
    QVariantMap keyboardLayout_;
    bool overviewOpen_ = false;
    bool connected_ = false;
};

}  // namespace quantum::niri
