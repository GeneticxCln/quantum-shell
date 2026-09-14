#include "niri/NiriOutputs.h"

#include "niri/NiriEventStream.h"
#include "niri/NiriIPC.h"
#include "niri/NiriProtocol.h"
#include "niri/NiriWorkspace.h"

#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>

namespace quantum::niri {
namespace {

// Reads the `Outputs` reply, which is a map from connector name to the output object. False means at
// least one entry could not be read, in which case `failure` says which.
bool parseOutputs(const QJsonValue& value, QList<NiriOutput>& outputs, QString& failure) {
    if (!value.isObject()) {
        failure = QStringLiteral("the Outputs reply was not an object of outputs");
        return false;
    }

    outputs.clear();
    QStringList unreadable;
    const QJsonObject map = value.toObject();
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (!it.value().isObject()) {
            unreadable.append(it.key());
            continue;
        }
        const NiriOutput output = NiriOutput::fromJson(it.value().toObject());
        if (!output.isValid()) {
            unreadable.append(it.key());
            continue;
        }
        outputs.append(output);
    }

    if (!unreadable.isEmpty()) {
        failure = QStringLiteral("the Outputs reply had entries with no usable name: %1")
                      .arg(unreadable.join(QStringLiteral(", ")));
        return false;
    }

    // Ordered by connector name, so the list is stable for a given compositor state and never depends
    // on the map's iteration order.
    std::sort(outputs.begin(), outputs.end(),
              [](const NiriOutput& left, const NiriOutput& right) { return left.name() < right.name(); });
    return true;
}

}  // namespace

NiriOutputs::NiriOutputs(NiriIPC& requests, QObject* parent) : QObject(parent), requests_(requests) {
    // A request whose answer never comes — the connection dropped, or the compositor is gone — must
    // not leave this class believing one is still in flight, or no later trigger would ever be acted
    // on. Both signals are the ways that happens.
    connect(&requests_, &NiriIPC::disconnected, this, [this] {
        inFlight_ = false;
        pending_ = false;
    });
    connect(&requests_, &NiriIPC::transportError, this, [this](const QString& reason) {
        inFlight_ = false;
        pending_ = false;
        emit refreshFailed(reason);
    });
}

void NiriOutputs::observe(NiriEventStream& stream) {
    connect(&stream, &NiriEventStream::workspacesChanged, this,
            &NiriOutputs::handleWorkspacesChanged);
    connect(&stream, &NiriEventStream::configLoaded, this,
            [this](bool) { refresh(); });
    connect(&stream, &NiriEventStream::disconnected, this, &NiriOutputs::handleDisconnected);

    // The initial answer, asked for rather than assumed. The burst that follows a subscription also
    // triggers one of the two events below, and the coalescing in refresh() turns the pair into a
    // single extra request at most.
    refresh();
}

void NiriOutputs::refresh() {
    if (inFlight_) {
        pending_ = true;
        return;
    }
    requestOutputs();
}

bool NiriOutputs::isRefreshing() const {
    return inFlight_;
}

QList<NiriOutput> NiriOutputs::outputs() const {
    return outputs_;
}

void NiriOutputs::requestOutputs() {
    inFlight_ = true;
    requests_.send(QStringLiteral("Outputs"),
                   [this](const Reply& reply) { handleReply(reply); });
}

void NiriOutputs::handleReply(const Reply& reply) {
    inFlight_ = false;

    if (!reply.isOk()) {
        emit refreshFailed(
            QStringLiteral("the compositor refused an Outputs request: %1").arg(reply.describe()));
    } else {
        QList<NiriOutput> parsed;
        QString failure;
        if (parseOutputs(reply.variant(QStringLiteral("Outputs")), parsed, failure)) {
            if (parsed != outputs_) {
                outputs_ = parsed;
                emit outputsChanged(outputs_);
            }
        } else {
            emit refreshFailed(failure);
        }
    }

    if (pending_) {
        pending_ = false;
        requestOutputs();
    }
}

void NiriOutputs::handleWorkspacesChanged(const QList<NiriWorkspace>& workspaces) {
    QSet<QString> referenced;
    for (const NiriWorkspace& workspace : workspaces) {
        if (!workspace.output().isEmpty()) {
            referenced.insert(workspace.output());
        }
    }
    if (referenced == workspaceOutputs_) {
        return;
    }
    workspaceOutputs_ = referenced;
    refresh();
}

void NiriOutputs::handleDisconnected() {
    inFlight_ = false;
    pending_ = false;
    workspaceOutputs_.clear();
    // Nothing about the compositor's outputs is known once the compositor is gone.
    if (!outputs_.isEmpty()) {
        outputs_.clear();
        emit outputsChanged(outputs_);
    }
}

}  // namespace quantum::niri
