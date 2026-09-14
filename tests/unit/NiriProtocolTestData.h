// Wire-shaped test data for the niri tests: the JSON niri sends, built in process.
//
// Synthetic on purpose. A fixture cut from a real session would carry one machine's window titles and
// output names into the repository, and a device name that matches the developer's machine is exactly
// the sort of literal that hides a mockup. Real values belong in the live tests, which read them from
// the running compositor.
//
// These are not the gate's fixture trees under tests/fixtures: they are compiled into the tests.
#pragma once

#include "niri/NiriWindow.h"
#include "niri/NiriWorkspace.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <optional>

namespace qstest {

inline QJsonValue number(quint64 value) {
    return QJsonValue(static_cast<double>(value));
}

// A workspace object with every field niri-ipc v26.04 defines. `activeWindowId` is an optional rather
// than a zero sentinel because niri reports "no active window" as a JSON null and a real id is never
// zero: a fixture that could not tell those apart would let an implementation conflate them.
inline QJsonObject workspaceObject(quint64 id, int idx, const QString& output, bool focused = false,
                                   bool active = false, const QString& name = QString(),
                                   bool urgent = false,
                                   std::optional<quint64> activeWindowId = std::nullopt) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), number(id));
    object.insert(QStringLiteral("idx"), QJsonValue(idx));
    object.insert(QStringLiteral("output"), output.isEmpty() ? QJsonValue() : QJsonValue(output));
    object.insert(QStringLiteral("name"), name.isEmpty() ? QJsonValue() : QJsonValue(name));
    object.insert(QStringLiteral("is_urgent"), QJsonValue(urgent));
    object.insert(QStringLiteral("is_active"), QJsonValue(active));
    object.insert(QStringLiteral("is_focused"), QJsonValue(focused));
    object.insert(QStringLiteral("active_window_id"),
                  activeWindowId.has_value() ? number(*activeWindowId) : QJsonValue());
    return object;
}

// A window object with every field niri-ipc v26.04 defines, including the two this build ignores.
// `floating` and `urgent` are parameters because they are the two fields of a window that no event on
// the stream changes — a test that needs a floating window has to build one that says so. `workspaceId`
// is an optional so a test can build niri's `workspace_id: null`, which is a window that is not placed
// on any workspace: the shell reports that by leaving the key out, and a fixture that could not say
// null would let that claim go untested.
inline QJsonObject windowObject(quint64 id, const QString& appId,
                               std::optional<quint64> workspaceId, bool focused = false,
                               bool floating = false, bool urgent = false) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), number(id));
    object.insert(QStringLiteral("title"), QJsonValue(QStringLiteral("title of %1").arg(appId)));
    object.insert(QStringLiteral("app_id"), QJsonValue(appId));
    object.insert(QStringLiteral("pid"), QJsonValue(4242));
    object.insert(QStringLiteral("workspace_id"),
                  workspaceId.has_value() ? number(*workspaceId) : QJsonValue());
    object.insert(QStringLiteral("is_focused"), QJsonValue(focused));
    object.insert(QStringLiteral("is_floating"), QJsonValue(floating));
    object.insert(QStringLiteral("is_urgent"), QJsonValue(urgent));
    object.insert(QStringLiteral("layout"),
                  QJsonObject{{QStringLiteral("tile_size"), QJsonArray{800, 600}}});
    object.insert(QStringLiteral("focus_timestamp"),
                  QJsonObject{{QStringLiteral("secs"), QJsonValue(1)},
                              {QStringLiteral("nanos"), QJsonValue(0)}});
    return object;
}

// Parsed values, for the tests that drive the state model directly rather than through a socket. Both
// keep the fixture's synthetic values: the live tests are where a real compositor's readings belong.
inline quantum::niri::NiriWorkspace workspace(quint64 id, int idx, const QString& output,
                                             bool focused = false, bool active = false,
                                             const QString& name = QString()) {
    return quantum::niri::NiriWorkspace::fromJson(workspaceObject(id, idx, output, focused, active, name));
}

inline quantum::niri::NiriWindow window(quint64 id, const QString& appId, quint64 workspaceId,
                                       bool focused = false) {
    return quantum::niri::NiriWindow::fromJson(windowObject(id, appId, workspaceId, focused));
}

inline QJsonArray array(const QList<QJsonObject>& objects) {
    QJsonArray json;
    for (const QJsonObject& object : objects) {
        json.append(object);
    }
    return json;
}

// One output mode, as niri-ipc v26.04's `Mode`. `refreshRate` is in millihertz, as niri reports it.
inline QJsonObject modeObject(quint16 width, quint16 height, quint32 refreshRate,
                              bool isPreferred = false) {
    return QJsonObject{{QStringLiteral("width"), QJsonValue(static_cast<int>(width))},
                       {QStringLiteral("height"), QJsonValue(static_cast<int>(height))},
                       {QStringLiteral("refresh_rate"), number(refreshRate)},
                       {QStringLiteral("is_preferred"), QJsonValue(isPreferred)}};
}

// One logical output, as niri-ipc v26.04's `LogicalOutput`. `scale` stays fractional on purpose: a
// test that rounds it would stop noticing an implementation that assumed an integer.
inline QJsonObject logicalObject(qint32 x, qint32 y, quint32 width, quint32 height, double scale,
                                 const QString& transform = QStringLiteral("Normal")) {
    return QJsonObject{{QStringLiteral("x"), QJsonValue(static_cast<int>(x))},
                       {QStringLiteral("y"), QJsonValue(static_cast<int>(y))},
                       {QStringLiteral("width"), number(width)},
                       {QStringLiteral("height"), number(height)},
                       {QStringLiteral("scale"), QJsonValue(scale)},
                       {QStringLiteral("transform"), QJsonValue(transform)}};
}

// One output, as niri-ipc v26.04's `Output` — every field it defines, including `serial`,
// `physical_size` and `is_custom_mode`, which this build deliberately does not parse. A JSON null for
// `currentMode` or `logical` is how niri reports a disabled output.
// `make` is a parameter and not a constant on purpose: the service maps `make` and `model` to two
// different keys, and a caller that needs to prove the two are not swapped passes values that differ.
inline QJsonObject outputObject(const QString& name, const QList<QJsonObject>& modes,
                                const QJsonValue& currentMode, const QJsonValue& logical,
                                const QString& model = QStringLiteral("MODEL-A"),
                                bool vrrSupported = false, bool vrrEnabled = false,
                                const QString& make = QStringLiteral("MAKE")) {
    QJsonObject object;
    object.insert(QStringLiteral("name"), QJsonValue(name));
    object.insert(QStringLiteral("make"), QJsonValue(make));
    object.insert(QStringLiteral("model"), QJsonValue(model));
    object.insert(QStringLiteral("serial"), QJsonValue(QStringLiteral("SERIAL-1")));
    object.insert(QStringLiteral("physical_size"), QJsonArray{600, 340});
    object.insert(QStringLiteral("modes"), array(modes));
    object.insert(QStringLiteral("current_mode"), currentMode);
    object.insert(QStringLiteral("is_custom_mode"), QJsonValue(false));
    object.insert(QStringLiteral("vrr_supported"), QJsonValue(vrrSupported));
    object.insert(QStringLiteral("vrr_enabled"), QJsonValue(vrrEnabled));
    object.insert(QStringLiteral("logical"), logical);
    return object;
}

// The `Outputs` reply, as `Response::Outputs` sends it: an object keyed by connector name.
inline QByteArray outputsReplyLine(const QList<QJsonObject>& outputs) {
    QJsonObject map;
    for (const QJsonObject& output : outputs) {
        map.insert(output.value(QStringLiteral("name")).toString(), output);
    }
    const QJsonObject value{{QStringLiteral("Outputs"), QJsonValue(map)}};
    return QJsonDocument(QJsonObject{{QStringLiteral("Ok"), QJsonValue(value)}})
        .toJson(QJsonDocument::Compact);
}

// niri-ipc v26.04's `KeyboardLayouts`.
inline QJsonObject keyboardLayoutsObject(const QStringList& names, int currentIdx) {
    QJsonArray list;
    for (const QString& name : names) {
        list.append(name);
    }
    return QJsonObject{{QStringLiteral("names"), QJsonValue(list)},
                       {QStringLiteral("current_idx"), QJsonValue(currentIdx)}};
}

// One event line, as it arrives on the socket: a single-key object plus the newline niri terminates
// every line with.
inline QByteArray eventLine(const QString& name, const QJsonObject& fields) {
    const QJsonObject wrapper{{name, fields}};
    return QJsonDocument(wrapper).toJson(QJsonDocument::Compact) + '\n';
}

}  // namespace qstest
