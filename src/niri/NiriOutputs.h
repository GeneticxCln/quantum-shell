// Keeps the connected outputs in step, on the only terms niri 26.04 offers.
//
// niri sends no event for outputs: niri-ipc v26.04's `Event` enum has no output variant and the
// event-stream state has no outputs part, so a subscription is answered with workspaces, windows,
// keyboard layouts, overview, config and casts and nothing else. Outputs are available only from an
// `Outputs` request, and doing that on a timer would be polling.
//
// So this class asks for them once, and then again on the events that can carry news about the output
// set. Both triggers are derived from data rather than guessed at:
//
//   - `WorkspacesChanged`, when the set of output names the workspaces reference changed. A workspace
//     names its output, so an output that appeared or went away is visible there — and this is a
//     change in that set, not every workspace event.
//   - `ConfigLoaded`, because output configuration lives in niri's config file, so a reload is
//     exactly when scale, mode and position may have changed.
//
// Requests are coalesced: at most one `Outputs` request is in flight, and a trigger that arrives
// while one is in flight causes exactly one more afterwards.
//
// The gap this leaves, stated rather than hidden: an output change that fires neither of those events
// — a transient `niri msg output` change, or an output that is plugged in without gaining a workspace
// — is not noticed until one of them does. `refresh()` is public so the caller can force one when it
// has a reason to.
//
// A reply that cannot be read in full is treated as a failed refresh rather than applied in part: a
// missing output in the list is indistinguishable from an unplugged monitor, so the last complete
// list is the safer answer, and `refreshFailed` says so.
#pragma once

#include "niri/NiriOutput.h"

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace quantum::niri {

class NiriEventStream;
class NiriIPC;
class NiriWorkspace;
struct Reply;

class NiriOutputs : public QObject {
    Q_OBJECT

public:
    // `requests` must outlive this object; it is the request connection, never the event stream.
    explicit NiriOutputs(NiriIPC& requests, QObject* parent = nullptr);

    // Starts asking for the outputs and subscribes to the events that can change them.
    void observe(NiriEventStream& stream);

    // Asks now. Coalesced with any request already in flight.
    void refresh();

    bool isRefreshing() const;
    // The last complete list the compositor gave, ordered by connector name. Empty until one arrives
    // or after the compositor goes away.
    QList<NiriOutput> outputs() const;

signals:
    void outputsChanged(const QList<quantum::niri::NiriOutput>& outputs);
    void refreshFailed(const QString& reason);

private:
    void requestOutputs();
    void handleReply(const Reply& reply);
    void handleWorkspacesChanged(const QList<NiriWorkspace>& workspaces);
    void handleDisconnected();

    NiriIPC& requests_;
    QList<NiriOutput> outputs_;
    // The output names the last workspace event referenced, so a repeat of the same set is not a
    // reason to ask again.
    QSet<QString> workspaceOutputs_;
    bool inFlight_ = false;
    bool pending_ = false;
};

}  // namespace quantum::niri
