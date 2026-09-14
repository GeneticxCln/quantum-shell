// The actions the shell performs, as the variants of niri-ipc v26.04's `Action` enum.
//
// Only the actions something actually performs are here. niri-ipc v26.04 defines 141 variants — window
// and column movement, sizing, floating, casts, output configuration, quitting the compositor — and
// declaring the other 133 would be surface with no caller. Each method below exists because the shell's
// bar or its tests needs it.
//
// Every action carries fields, so a request can never be spelled as the bare JSON string
// `NiriIPC::send` takes: each one goes through `NiriIPC::sendObject`, which serialises it compactly and
// keeps niri's one-request-per-line rule intact. Nothing here assembles JSON by concatenation, because
// a value containing a quote or a line break would otherwise put a second request on the connection.
//
// Fields niri documents as optional are sent explicitly as JSON null rather than left out. Null is what
// serde reads as `None`; whether an absent field would also be accepted depends on serde attributes
// inside niri-ipc that this build cannot verify without performing the action, and several of these
// actions have effects worth not performing twice.
//
// This is also the surface QML calls, which is why the three methods at the bottom of the public section
// are `Q_INVOKABLE`: the bar focuses the workspace a capsule names and moves to the one below or above
// when the wheel turns over the strip. One class rather than a second object translating this one,
// because a second object would be a second view of the same actions that nothing keeps in step — the
// rule NiriService is the only translation of `NiriState` exists for. The object is registered as the
// `NiriActions` singleton in the same QML module `NiriService` lives in.
#pragma once

#include "niri/NiriIPC.h"

#include <QJsonObject>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <limits>
#include <optional>

namespace quantum::niri {

class NiriActions : public QObject {
    Q_OBJECT

public:
    // A workspace to act on, as niri-ipc v26.04's `WorkspaceReferenceArg`. By id is the id niri gives it,
    // which the state model holds; by name is the name the user configured; by index is a **slot on the
    // active output**, which is not the same thing as the `idx` field a workspace carries.
    //
    // Verified in niri v26.04's own source: `WorkspaceReference::Index(n)` is resolved by
    // `find_output_and_workspace_index` (src/niri.rs) to `(None, n - 1)` — no workspace is looked up —
    // and `Action::FocusWorkspace` (src/input/mod.rs) then calls `switch_workspace(n - 1)`, or
    // `switch_workspace_auto_back_and_forth(n - 1)` when the active output is the implied one. So index
    // addresses a slot and *switches* to it: on a config with `workspace-auto-back-and-forth` — which
    // this project's own test session has — passing the index of the workspace already focused lands on
    // the previously focused workspace and is **not** a no-op. By id and by name niri does resolve a real
    // workspace, but they too end in `switch_workspace`, so the same applies once the target is on the
    // active output.
    struct WorkspaceReference {
        enum class Kind { index, id, name };

        static WorkspaceReference atIndex(int index);
        static WorkspaceReference withId(quint64 id);
        static WorkspaceReference named(const QString& name);

        Kind kind() const;
        // False for an index that a `u8` cannot hold, for an id this client cannot write as a JSON
        // number (`isAddressableId`), or for an empty name. Such a reference is refused before anything
        // is sent rather than truncated into a different workspace.
        bool isValid() const;
        // The JSON niri expects for this reference, e.g. {"Index":2}. Empty when isValid() is false.
        QJsonObject toJson() const;
        QString describe() const;

    private:
        Kind kind_ = Kind::index;
        int index_ = 0;
        quint64 id_ = 0;
        QString name_;
    };

    // Which layout to switch to, as niri-ipc v26.04's `LayoutSwitchTarget`.
    struct LayoutTarget {
        enum class Kind { next, previous, index };

        static LayoutTarget next();
        static LayoutTarget previous();
        static LayoutTarget atIndex(int index);

        Kind kind() const;
        bool isValid() const;
        QJsonObject toJson() const;
        QString describe() const;

    private:
        Kind kind_ = Kind::next;
        int index_ = 0;
    };

    // What became of one action.
    struct Result {
        enum class Outcome {
            handled,       // {"Ok":"Handled"} — the compositor performed it
            refused,       // {"Err":...} — the compositor understood it and declined, or could not parse it
            notDelivered,  // nothing was sent, or the connection went away before an answer came
            unexpected,    // an ok reply that is not the `Handled` response this protocol defines
        };

        Outcome outcome = Outcome::notDelivered;
        QString detail;

        bool isHandled() const;
        QString describe() const;
    };

    // The largest niri id this client can put on the wire. A JSON number reaches `QJsonValue` either as
    // a double — exact only to 2^53, so an id above that would go out as a different one — or as a
    // signed 64-bit integer, which is written as its exact digits. niri's ids are `u64` and niri
    // documents that they may come to be generated at random, so the integer form is the one used here,
    // and an id it cannot hold is refused: a request naming the wrong workspace is worse than one that
    // says it cannot name this one.
    static constexpr quint64 maximumId = static_cast<quint64>(std::numeric_limits<qint64>::max());
    static constexpr bool isAddressableId(quint64 id) { return id <= maximumId; }

    using ResultHandler = std::function<void(const Result&)>;

    // `requests` must outlive this object. It is the request connection: niri stops reading requests on
    // a connection once an event stream has been requested on it.
    explicit NiriActions(NiriIPC& requests, QObject* parent = nullptr);

    void focusWorkspace(const WorkspaceReference& reference, ResultHandler handler = {});
    // Runs `command` directly: no shell, so no globbing, pipes or expansion. spawnSh() is the form that
    // hands the string to a shell.
    void spawn(const QStringList& command, ResultHandler handler = {});
    void spawnSh(const QString& command, ResultHandler handler = {});
    // `windowId` defaults to the focused window. `focus` says whether the focus follows the window.
    void moveWindowToWorkspace(const WorkspaceReference& reference, bool focus,
                               std::optional<quint64> windowId = std::nullopt,
                               ResultHandler handler = {});
    // Screenshots the focused screen. `writeToDisk` also saves it according to niri's own
    // screenshot-path setting — the path is niri's to choose, so it is not a parameter here — and the
    // screenshot always goes to the clipboard.
    void screenshotScreen(bool writeToDisk, bool showPointer, ResultHandler handler = {});
    void openOverview(ResultHandler handler = {});
    void closeOverview(ResultHandler handler = {});
    void switchLayout(const LayoutTarget& target, ResultHandler handler = {});

    // --- what QML performs ----------------------------------------------------------------------
    //
    // `FocusWorkspace` by id, which is the reference a QML capsule has: niri documents that workspace
    // ids need not be small and may be generated at random, so an id crosses the boundary as the text QML
    // read (`NiriService.workspaces[i].id`) and is parsed back to a `u64` here. A text that is not
    // decimal digits, or too large for a `u64`, is refused before anything is sent — this build will not
    // round a value it cannot hold exactly into a different workspace.
    Q_INVOKABLE void focusWorkspaceById(const QString& idText);
    // `FocusWorkspaceUp` and `FocusWorkspaceDown`, niri's own "workspace above/below" actions — the same
    // pair niri's default config binds to the wheel (`Mod+WheelScrollDown { focus-workspace-down; }`).
    // Which workspace is "below" is the compositor's answer, not one computed from the strip's own
    // order here.
    Q_INVOKABLE void focusWorkspaceUp();
    Q_INVOKABLE void focusWorkspaceDown();

    // Seeded from the object that performs them and registered as the `NiriActions` singleton, the way
    // NiriService registers the state: QML only calls into an object the shell's C++ owns, so a component
    // cannot create a second one that performs actions nothing connected.
    static void registerQmlSingleton(NiriActions& actions);

signals:
    // Every action whose outcome is not `handled`, whether or not a handler was passed. A failure must
    // not be reportable only by a handler the caller happened to leave out.
    void actionFailed(const QString& action, const quantum::niri::NiriActions::Result& result);

private:
    void dispatch(const QString& action, const QJsonObject& fields, const ResultHandler& handler);
    // Reports an action that is not worth sending, without sending it.
    void refuse(const QString& action, const QString& reason, const ResultHandler& handler);
    // The same for the two actions that need a workspace; true means it was refused.
    bool refuseWithoutAWorkspace(const QString& action, const WorkspaceReference& reference,
                                 const ResultHandler& handler);

    NiriIPC& requests_;
};

}  // namespace quantum::niri

// Results travel through signals, so they can be queued across threads later.
Q_DECLARE_METATYPE(quantum::niri::NiriActions::Result)
