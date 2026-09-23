#include "config/ConfigSchema.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <optional>
#include <string_view>

namespace quantum::config {
namespace {

// The known keys, written as the paths a user types. Unknown keys are reported by their full path —
// `bar.widht`, not `widht` — because that is the line in the file to go and look at.
bool isKnownTopLevelKey(std::string_view key) {
    return key == "schema_version" || key == "bar";
}

bool isKnownBarKey(std::string_view key) {
    return key == "height" || key == "namespace" || key == "system" || key == "audio" ||
           key == "network" || key == "battery" || key == "media" || key == "notifications";
}

// `[bar.system]`'s keys, in the file's spelling and in the order a warning names them, so that the list a
// user is shown and the list this code reads are one list rather than two that can drift apart. The file's
// spelling is snake_case, like `schema_version`, while the C++ field each lands in is camelCase like every
// other member — the mapping is mechanical and both spellings are named where they meet, which is here.
constexpr std::array<std::string_view, 4> systemKeys{"sample_interval_ms", "show_cpu", "show_memory",
                                                     "memory_format"};

bool isKnownSystemKey(std::string_view key)
{
    return std::find(systemKeys.begin(), systemKeys.end(), key) != systemKeys.end();
}

QString knownSystemKeysText()
{
    QStringList names;
    for (const std::string_view key : systemKeys)
        names.append(QString::fromUtf8(key.data(), static_cast<int>(key.size())));
    return names.join(QStringLiteral(", "));
}

// `[bar.audio]`'s keys, in the same shape and for the same reason as `[bar.system]`'s above: one list, so
// the names a user is shown and the names this code reads cannot drift apart.
constexpr std::array<std::string_view, 4> audioKeys{"show_volume", "volume_scale", "step_percent",
                                                     "step_decibels"};

bool isKnownAudioKey(std::string_view key)
{
    return std::find(audioKeys.begin(), audioKeys.end(), key) != audioKeys.end();
}

QString knownAudioKeysText()
{
    QStringList names;
    for (const std::string_view key : audioKeys)
        names.append(QString::fromUtf8(key.data(), static_cast<int>(key.size())));
    return names.join(QStringLiteral(", "));
}

// `[bar.network]`'s keys, in the same shape as the two tables above. All three are flags, which is what the
// readout's shape decides rather than a preference: every other choice the network readout could offer is a
// fact NetworkManager owns — which connection is in use, what kind of device carries it, what the signal
// quality is — so the only settings left are which of those a person wants drawn.
constexpr std::array<std::string_view, 3> networkKeys{"show_status", "show_name", "show_strength"};

bool isKnownNetworkKey(std::string_view key)
{
    return std::find(networkKeys.begin(), networkKeys.end(), key) != networkKeys.end();
}

QString knownNetworkKeysText()
{
    QStringList names;
    for (const std::string_view key : networkKeys)
        names.append(QString::fromUtf8(key.data(), static_cast<int>(key.size())));
    return names.join(QStringLiteral(", "));
}

// `[bar.battery]`'s keys, in the same shape as the three tables above. All three are flags, for the reason the
// network table gives: every other choice the battery readout could offer is a fact UPower owns — whether there
// is a battery, what its charge is, which way the charge is going, how long it will take and how worried the
// daemon is — so the only settings left are which of those facts a person wants drawn.
constexpr std::array<std::string_view, 3> batteryKeys{"show_status", "show_percentage", "show_time"};

bool isKnownBatteryKey(std::string_view key)
{
    return std::find(batteryKeys.begin(), batteryKeys.end(), key) != batteryKeys.end();
}

QString knownBatteryKeysText()
{
    QStringList names;
    for (const std::string_view key : batteryKeys)
        names.append(QString::fromUtf8(key.data(), static_cast<int>(key.size())));
    return names.join(QStringLiteral(", "));
}

// `[bar.media]`'s key, in the same shape as the four tables above. One flag, for the reason they give: every other
// choice the media readout could offer is a fact MPRIS players own — whether a player exists, what track is
// playing, who made it and whether it is playing or paused — so the only setting left is whether that readout is
// drawn.
constexpr std::array<std::string_view, 1> mediaKeys{"show_media"};

bool isKnownMediaKey(std::string_view key)
{
    return std::find(mediaKeys.begin(), mediaKeys.end(), key) != mediaKeys.end();
}

QString knownMediaKeysText()
{
    QStringList names;
    for (const std::string_view key : mediaKeys)
        names.append(QString::fromUtf8(key.data(), static_cast<int>(key.size())));
    return names.join(QStringLiteral(", "));
}

// `[bar.notifications]`'s key, in the same shape as the tables above. One flag, for the same reason the
// media table's is one: the readout's only configurable choice is whether it is drawn, because the
// summary, the body and the application name are a sender's facts and not this shell's settings.
constexpr std::array<std::string_view, 1> notificationsKeys{"show_notifications"};

bool isKnownNotificationsKey(std::string_view key)
{
    return std::find(notificationsKeys.begin(), notificationsKeys.end(), key) != notificationsKeys.end();
}

QString knownNotificationsKeysText()
{
    QStringList names;
    for (const std::string_view key : notificationsKeys)
        names.append(QString::fromUtf8(key.data(), static_cast<int>(key.size())));
    return names.join(QStringLiteral(", "));
}

// The name of a node's type, for the "expected an integer, found a string" half of a warning.
QString typeName(const toml::node& node) {
    switch (node.type()) {
    case toml::node_type::none:
        break;
    case toml::node_type::boolean:
        return QStringLiteral("a boolean");
    case toml::node_type::integer:
        return QStringLiteral("an integer");
    case toml::node_type::floating_point:
        return QStringLiteral("a float");
    case toml::node_type::string:
        return QStringLiteral("a string");
    case toml::node_type::array:
        return QStringLiteral("an array");
    case toml::node_type::table:
        return QStringLiteral("a table");
    case toml::node_type::date:
        return QStringLiteral("a date");
    case toml::node_type::time:
        return QStringLiteral("a time");
    case toml::node_type::date_time:
        return QStringLiteral("a date and time");
    }
    return QStringLiteral("nothing");
}

QString keyPath(std::string_view table, std::string_view key) {
    if (table.empty())
        return QString::fromUtf8(key.data(), static_cast<int>(key.size()));
    return QStringLiteral("%1.%2")
        .arg(QString::fromUtf8(table.data(), static_cast<int>(table.size())),
             QString::fromUtf8(key.data(), static_cast<int>(key.size())));
}

std::string_view keyText(const toml::key& key) {
    return key.str();
}

// `[bar.system]` keys. Each known key is read if it is present and of a usable type; anything else leaves
// the default standing and says so, by the same rule as `[bar]`'s own keys.
void readSystemTable(const toml::table& table, SystemConfig& system, QStringList& warnings)
{
    for (const auto& [key, node] : table) {
        const std::string_view name = keyText(key);
        if (!isKnownSystemKey(name)) {
            warnings.append(QStringLiteral("%1 is not a key this shell reads; known keys in [bar.system]: %2")
                                .arg(keyPath("bar.system", name), knownSystemKeysText()));
            continue;
        }

        const QString path = keyPath("bar.system", name);

        // The two flags that say whether a readout is drawn. `as_boolean` rather than `value<bool>()`,
        // because the latter coerces: a file saying `show_memory = 1` was accepted and the value was set,
        // with no warning at all, which is exactly the kind of quiet acceptance this file exists to refuse.
        // `show_cpu = "no"` is a mistake, and a mistake has to be reported rather than interpreted.
        if (name == "show_cpu" || name == "show_memory") {
            const toml::value<bool>* shown = node.as_boolean();
            const bool kept = name == "show_cpu" ? system.showCpu : system.showMemory;
            if (shown == nullptr) {
                warnings.append(QStringLiteral("%1: expected a boolean, found %2; keeping %3")
                                    .arg(path, typeName(node), kept ? QStringLiteral("true")
                                                                   : QStringLiteral("false")));
                continue;
            }
            (name == "show_cpu" ? system.showCpu : system.showMemory) = shown->get();
            continue;
        }

        // The form the memory readout is drawn in: one of the tokens `MemoryFormats` spells, and nothing
        // else. Checked here rather than in QML because a widget handed a token it has no branch for would
        // silently draw the default — a configuration value that does nothing is the failure this file exists
        // to report instead.
        if (name == "memory_format") {
            const std::optional<std::string_view> format = node.value<std::string_view>();
            if (!format.has_value()) {
                warnings.append(QStringLiteral("%1: expected a string, found %2; keeping \"%3\"")
                                    .arg(path, typeName(node), system.memoryFormat));
                continue;
            }
            const QString candidate =
                QString::fromUtf8(format->data(), static_cast<int>(format->size()));
            const bool known =
                std::any_of(MemoryFormats.begin(), MemoryFormats.end(), [&candidate](const char* token) {
                    return candidate == QLatin1StringView(token);
                });
            if (!known) {
                QStringList tokens;
                for (const char* token : MemoryFormats)
                    tokens.append(QLatin1String(token));
                warnings.append(QStringLiteral("%1: \"%2\" is not a form this shell draws memory in "
                                               "(%3); keeping \"%4\"")
                                    .arg(path, candidate, tokens.join(QStringLiteral(", ")),
                                         system.memoryFormat));
                continue;
            }
            system.memoryFormat = candidate;
            continue;
        }

        const std::optional<int64_t> interval = node.value<int64_t>();
        if (!interval.has_value()) {
            warnings.append(QStringLiteral("%1: expected an integer, found %2; keeping %3")
                                .arg(path, typeName(node))
                                .arg(system.sampleIntervalMs));
            continue;
        }
        // Both bounds before the narrowing, for the reason `bar.height` checks its own: a value from the file
        // must never wrap into one that means something else. The floor is not arbitrary — it is the kernel's
        // own `USER_HZ`, and `SysMonService` refuses the same values for the same reason.
        if (*interval < MinSampleIntervalMs || *interval > std::numeric_limits<int>::max()) {
            warnings.append(QStringLiteral("%1: %2 ms is not an interval this shell can sample at (one CPU "
                                           "tick is %3 ms, and a timer holds no more than %4); keeping %5")
                                .arg(path)
                                .arg(*interval)
                                .arg(MinSampleIntervalMs)
                                .arg(std::numeric_limits<int>::max())
                                .arg(system.sampleIntervalMs));
            continue;
        }
        system.sampleIntervalMs = static_cast<int>(*interval);
    }
}

// `[bar.network]` keys. Three flags, read by the same rule as every other table here: a key of the wrong type
// is reported rather than coerced, because `show_name = "yes"` is a mistake and reading it as true would hide
// it. There is nothing here that needs a range or a token check — which is what the readout being entirely
// NetworkManager's facts leaves a configuration to be.
void readNetworkTable(const toml::table& table, NetworkConfig& network, QStringList& warnings)
{
    for (const auto& [key, node] : table) {
        const std::string_view name = keyText(key);
        if (!isKnownNetworkKey(name)) {
            warnings.append(QStringLiteral("%1 is not a key this shell reads; known keys in [bar.network]: %2")
                                .arg(keyPath("bar.network", name), knownNetworkKeysText()));
            continue;
        }

        const QString path = keyPath("bar.network", name);
        const toml::value<bool>* shown = node.as_boolean();
        const bool kept = name == "show_status"  ? network.showStatus
                          : name == "show_name" ? network.showName
                                                : network.showStrength;
        if (shown == nullptr) {
            warnings.append(QStringLiteral("%1: expected a boolean, found %2; keeping %3")
                                .arg(path, typeName(node),
                                     kept ? QStringLiteral("true") : QStringLiteral("false")));
            continue;
        }
        (name == "show_status" ? network.showStatus
         : name == "show_name" ? network.showName
                                : network.showStrength) = shown->get();
    }
}

// `[bar.battery]` keys. Three flags, read by the same rule as every other table here: a key of the wrong type is
// reported rather than coerced, because `show_time = "yes"` is a mistake and reading it as true would hide it.
// There is nothing here that needs a range or a token check — which is what the readout being entirely UPower's
// facts leaves a configuration to be.
void readBatteryTable(const toml::table& table, BatteryConfig& battery, QStringList& warnings)
{
    for (const auto& [key, node] : table) {
        const std::string_view name = keyText(key);
        if (!isKnownBatteryKey(name)) {
            warnings.append(QStringLiteral("%1 is not a key this shell reads; known keys in [bar.battery]: %2")
                                .arg(keyPath("bar.battery", name), knownBatteryKeysText()));
            continue;
        }

        const QString path = keyPath("bar.battery", name);
        const toml::value<bool>* shown = node.as_boolean();
        const bool kept = name == "show_status"     ? battery.showStatus
                          : name == "show_percentage" ? battery.showPercentage
                                                       : battery.showTime;
        if (shown == nullptr) {
            warnings.append(QStringLiteral("%1: expected a boolean, found %2; keeping %3")
                                .arg(path, typeName(node),
                                     kept ? QStringLiteral("true") : QStringLiteral("false")));
            continue;
        }
        (name == "show_status" ? battery.showStatus
         : name == "show_percentage" ? battery.showPercentage
                                      : battery.showTime) = shown->get();
    }
}

// `[bar.media]` key. One flag, read by the same rule as every other table here: a key of the wrong type is
// reported rather than coerced. There is nothing here that needs a range or a token check — which is what the
// readout being entirely MPRIS players' facts leaves a configuration to be.
void readMediaTable(const toml::table& table, MediaConfig& media, QStringList& warnings)
{
    for (const auto& [key, node] : table) {
        const std::string_view name = keyText(key);
        if (!isKnownMediaKey(name)) {
            warnings.append(QStringLiteral("%1 is not a key this shell reads; known keys in [bar.media]: %2")
                                .arg(keyPath("bar.media", name), knownMediaKeysText()));
            continue;
        }

        const QString path = keyPath("bar.media", name);
        const toml::value<bool>* shown = node.as_boolean();
        if (shown == nullptr) {
            warnings.append(QStringLiteral("%1: expected a boolean, found %2; keeping %3")
                                .arg(path, typeName(node),
                                     media.showMedia ? QStringLiteral("true") : QStringLiteral("false")));
            continue;
        }
        media.showMedia = shown->get();
    }
}

// `[bar.notifications]` key, read by the same rule as every other table here: a key of the wrong type
// is reported rather than coerced, for the same reason a string `height` is.
void readNotificationsTable(const toml::table& table, NotificationsConfig& notifications,
                            QStringList& warnings)
{
    for (const auto& [key, node] : table) {
        const std::string_view name = keyText(key);
        if (!isKnownNotificationsKey(name)) {
            warnings.append(QStringLiteral("%1 is not a key this shell reads; known keys in "
                                           "[bar.notifications]: %2")
                                .arg(keyPath("bar.notifications", name), knownNotificationsKeysText()));
            continue;
        }

        const QString path = keyPath("bar.notifications", name);
        const toml::value<bool>* shown = node.as_boolean();
        if (shown == nullptr) {
            warnings.append(QStringLiteral("%1: expected a boolean, found %2; keeping %3")
                                .arg(path, typeName(node),
                                     notifications.showNotifications ? QStringLiteral("true")
                                                                      : QStringLiteral("false")));
            continue;
        }
        notifications.showNotifications = shown->get();
    }
}

// `[bar]` keys. Each known key is read if it is present and of a usable type; anything else leaves the
// default standing and says so. A key of the wrong type is reported and not coerced: `height = "32"`
// is a mistake, and reading it as 32 would hide it.
void readBarTable(const toml::table& table, BarConfig& bar, QStringList& warnings);

// `[bar.audio]` keys. Each known key is read if it is present and of a usable type; anything else leaves
// the default standing and says so, by the same rule as every other table here.
void readAudioTable(const toml::table& table, AudioConfig& audio, QStringList& warnings)
{
    for (const auto& [key, node] : table) {
        const std::string_view name = keyText(key);
        if (!isKnownAudioKey(name)) {
            warnings.append(QStringLiteral("%1 is not a key this shell reads; known keys in [bar.audio]: %2")
                                .arg(keyPath("bar.audio", name), knownAudioKeysText()));
            continue;
        }

        const QString path = keyPath("bar.audio", name);

        // Whether the readout is drawn. `as_boolean` rather than `value<bool>()`, for the reason
        // `[bar.system]`'s two flags are: the latter coerces, so `show_volume = 1` would be accepted as
        // true with nothing said about it.
        if (name == "show_volume") {
            const toml::value<bool>* shown = node.as_boolean();
            if (shown == nullptr) {
                warnings.append(QStringLiteral("%1: expected a boolean, found %2; keeping %3")
                                    .arg(path, typeName(node),
                                         audio.showVolume ? QStringLiteral("true") : QStringLiteral("false")));
                continue;
            }
            audio.showVolume = shown->get();
            continue;
        }

        // The unit the readout is drawn in: one of the tokens `VolumeScales` spells, and nothing else. Checked
        // here rather than in QML for the reason `memory_format` is — a widget handed a unit it has no branch
        // for would silently draw the default, and a configuration value that does nothing is the failure this
        // file exists to report instead.
        if (name == "volume_scale") {
            const std::optional<std::string_view> scale = node.value<std::string_view>();
            if (!scale.has_value()) {
                warnings.append(QStringLiteral("%1: expected a string, found %2; keeping \"%3\"")
                                    .arg(path, typeName(node), audio.volumeScale));
                continue;
            }
            const QString candidate = QString::fromUtf8(scale->data(), static_cast<int>(scale->size()));
            const bool known =
                std::any_of(VolumeScales.begin(), VolumeScales.end(), [&candidate](const char* token) {
                    return candidate == QLatin1StringView(token);
                });
            if (!known) {
                QStringList tokens;
                for (const char* token : VolumeScales)
                    tokens.append(QLatin1String(token));
                warnings.append(QStringLiteral("%1: \"%2\" is not a unit this shell draws the volume in "
                                               "(%3); keeping \"%4\"")
                                    .arg(path, candidate, tokens.join(QStringLiteral(", ")),
                                         audio.volumeScale));
                continue;
            }
            audio.volumeScale = candidate;
            continue;
        }

        if (name == "step_decibels") {
            // The same distance in the other unit, applied when the readout is drawn in it — one question with
            // one answer, so what is drawn and how far a notch moves cannot disagree. A number rather than an
            // integer, because a tenth of a decibel is a step this readout can show and half a decibel is a
            // step a person asks for. Both of the file's spellings of a number are read — `2` and `2.0` are the
            // same step — rather than only the floating one, which would refuse the obvious way to write it and
            // report a mistake where there is none. The floor is the readout's own resolution rather than a
            // judgement about hearing, and there is no ceiling worth checking: a step larger than the range is
            // a person asking for one notch to reach the end of it, which the arithmetic clamps.
            // `as_floating_point` and `as_integer` rather than `value<double>()`, which coerces: this file's rule
            // is that a key of the wrong type is reported and not converted, and both of a number's spellings
            // are numbers. The pointers are `auto` so their types are the library's business, which is the
            // reason the boolean above is read through `as_boolean` too.
            const auto* asFloating = node.as_floating_point();
            const auto* asInteger = node.as_integer();
            if (!asFloating && !asInteger) {
                warnings.append(QStringLiteral("%1: expected a number, found %2; keeping %3")
                                    .arg(path, typeName(node))
                                    .arg(audio.stepDecibels));
                continue;
            }
            // The unit's own tenth is the readout's resolution, so a step shorter than that is a step the
            // widget cannot show a person. Refused by name in the service as well, and the two spellings
            // are compared at compile time in `bar-interaction-test`.
            const double decibels = asFloating != nullptr ? asFloating->get()
                                                          : static_cast<double>(asInteger->get());
            if (!(decibels >= MinVolumeStepDecibels)) {
                warnings.append(QStringLiteral("%1: %2 is not a volume step (the shortest is %3 dB, which is "
                                               "the resolution the readout draws decibels at); keeping %4")
                                    .arg(path)
                                    .arg(decibels)
                                    .arg(MinVolumeStepDecibels)
                                    .arg(audio.stepDecibels));
                continue;
            }
            audio.stepDecibels = decibels;
            continue;
        }

        // How far one wheel notch moves the volume in percentage points, which is what a notch is measured in
        // while the readout is drawn in them. The floor is checked and the ceiling is only the int64-to-int
        // narrowing, because a step larger than the range is a person asking for one notch to reach the end of
        // it: `steppedPercent` clamps, so no large step is a mistake. A step of zero is.
        const std::optional<int64_t> step = node.value<int64_t>();
        if (!step.has_value()) {
            warnings.append(QStringLiteral("%1: expected an integer, found %2; keeping %3")
                                .arg(path, typeName(node))
                                .arg(audio.stepPercent));
            continue;
        }
        if (*step < MinVolumeStepPercent || *step > std::numeric_limits<int>::max()) {
            warnings.append(QStringLiteral("%1: %2 is not a volume step (the smallest is %3, and a step "
                                           "of zero is a wheel that does nothing); keeping %4")
                                .arg(path)
                                .arg(*step)
                                .arg(MinVolumeStepPercent)
                                .arg(audio.stepPercent));
            continue;
        }        audio.stepPercent = static_cast<int>(*step);
    }
}

void readBarTable(const toml::table& table, BarConfig& bar, QStringList& warnings) {
    for (const auto& [key, node] : table) {
        const std::string_view name = keyText(key);
        if (!isKnownBarKey(name)) {
            warnings.append(QStringLiteral("%1 is not a key this shell reads; known keys in [bar]: "
                                           "height, namespace, system, audio, network, battery, media, "
                                           "notifications")
                                .arg(keyPath("bar", name)));
            continue;
        }

        const QString path = keyPath("bar", name);

        // A table inside `[bar]`, which is a shape the bar had none of until the system status needed one.
        if (name == "system") {
            const toml::table* systemTable = node.as_table();
            if (systemTable == nullptr) {
                warnings.append(QStringLiteral("%1: expected a table, found %2; keeping the defaults for it")
                                    .arg(path, typeName(node)));
                continue;
            }
            readSystemTable(*systemTable, bar.system, warnings);
            continue;
        }

        // The second table inside `[bar]`. It is a table for the same reason the first one is: the settings
        // belong to the widget that reads them, and a widget's settings are written together.
        if (name == "audio") {
            const toml::table* audioTable = node.as_table();
            if (audioTable == nullptr) {
                warnings.append(QStringLiteral("%1: expected a table, found %2; keeping the defaults for it")
                                    .arg(path, typeName(node)));
                continue;
            }
            readAudioTable(*audioTable, bar.audio, warnings);
            continue;
        }

        // The third table inside `[bar]`, for the third readout to own one.
        if (name == "network") {
            const toml::table* networkTable = node.as_table();
            if (networkTable == nullptr) {
                warnings.append(QStringLiteral("%1: expected a table, found %2; keeping the defaults for it")
                                    .arg(path, typeName(node)));
                continue;
            }
            readNetworkTable(*networkTable, bar.network, warnings);
            continue;
        }

        // The fourth table inside `[bar]`, for the fourth readout to own one.
        if (name == "battery") {
            const toml::table* batteryTable = node.as_table();
            if (batteryTable == nullptr) {
                warnings.append(QStringLiteral("%1: expected a table, found %2; keeping the defaults for it")
                                    .arg(path, typeName(node)));
                continue;
            }
            readBatteryTable(*batteryTable, bar.battery, warnings);
            continue;
        }

        // The fifth table inside `[bar]`, for the fifth readout to own one.
        if (name == "media") {
            const toml::table* mediaTable = node.as_table();
            if (mediaTable == nullptr) {
                warnings.append(QStringLiteral("%1: expected a table, found %2; keeping the defaults for it")
                                    .arg(path, typeName(node)));
                continue;
            }
            readMediaTable(*mediaTable, bar.media, warnings);
            continue;
        }

        // The sixth table inside `[bar]`, for the sixth readout to own one.
        if (name == "notifications") {
            const toml::table* notificationsTable = node.as_table();
            if (notificationsTable == nullptr) {
                warnings.append(QStringLiteral("%1: expected a table, found %2; keeping the defaults for it")
                                    .arg(path, typeName(node)));
                continue;
            }
            readNotificationsTable(*notificationsTable, bar.notifications, warnings);
            continue;
        }

        if (name == "height") {
            const std::optional<int64_t> height = node.value<int64_t>();
            if (!height.has_value()) {
                warnings.append(QStringLiteral("%1: expected an integer, found %2; keeping %3")
                                    .arg(path, typeName(node))
                                    .arg(bar.height));
                continue;
            }
            if (*height < 1 || *height > std::numeric_limits<int>::max()) {
                // Validate both bounds before narrowing TOML's int64 to the int property consumed by
                // QML and the exclusive zone. A positive file value must never wrap into a negative one.
                warnings.append(QStringLiteral("%1: %2 is not a height the bar can have (it must be "
                                               "between 1 and %3); keeping %4")
                                    .arg(path)
                                    .arg(*height)
                                    .arg(std::numeric_limits<int>::max())
                                    .arg(bar.height));
                continue;
            }
            bar.height = static_cast<int>(*height);
            continue;
        }

        // namespace
        const std::optional<std::string_view> nameSpace = node.value<std::string_view>();
        if (!nameSpace.has_value()) {
            warnings.append(QStringLiteral("%1: expected a string, found %2; keeping \"%3\"")
                                .arg(path, typeName(node), bar.layerNamespace));
            continue;
        }
        const QString candidate = QString::fromUtf8(nameSpace->data(), static_cast<int>(nameSpace->size()));
        if (!candidate.startsWith(QLatin1StringView(LayerNamespacePrefix))) {
            warnings.append(QStringLiteral("%1: \"%2\" does not begin with %3, which every Quantum Shell "
                                           "layer-shell namespace must (AGENTS.md); keeping \"%4\"")
                                .arg(path, candidate, QLatin1StringView(LayerNamespacePrefix),
                                     bar.layerNamespace));
            continue;
        }
        bar.layerNamespace = candidate;
    }
}

}  // namespace

ParseResult parseConfig(const QByteArray& text) {
    ParseResult result;

    toml::table table;
    try {
        table = toml::parse(std::string_view(text.constData(), static_cast<size_t>(text.size())));
    } catch (const toml::parse_error& error) {
        // The message carries the line and column, which is the whole of what a user needs to fix it.
        result.errors.append(QStringLiteral("not valid TOML: %1")
                                 .arg(QString::fromUtf8(error.description().data(),
                                                        static_cast<int>(error.description().size()))));
        return result;
    } catch (const std::exception& error) {
        result.errors.append(
            QStringLiteral("could not be read: %1").arg(QString::fromUtf8(error.what())));
        return result;
    }

    // The version decides whether anything below is even the right thing to be reading. A file from a
    // newer shell is refused as a whole rather than partly understood: a key that has changed meaning is
    // a value the shell would be inventing, and partly applying it would leave the configuration neither
    // old nor new.
    int declaredVersion = SchemaVersion;
    if (const toml::node* versionNode = table.get("schema_version"); versionNode != nullptr) {
        const std::optional<int64_t> version = versionNode->value<int64_t>();
        if (!version.has_value()) {
            result.errors.append(QStringLiteral("schema_version: expected an integer, found %1; the file "
                                                "was not applied")
                                     .arg(typeName(*versionNode)));
            return result;
        }
        if (*version != SchemaVersion) {
            result.errors.append(QStringLiteral("schema_version %1 is not a version this shell knows "
                                                "(%2 is current); the file was not applied")
                                     .arg(*version)
                                     .arg(SchemaVersion));
            return result;
        }
        declaredVersion = static_cast<int>(*version);
    } else {
        // Missing keys fall back to defaults, and this one has a default like any other — but it is
        // worth a warning of its own, because a file that omits it is a file written before the field
        // existed, and that is the case a migration will have to be written for.
        result.warnings.append(QStringLiteral("no schema_version; assuming %1").arg(declaredVersion));
    }

    for (const auto& [key, node] : table) {
        const std::string_view name = keyText(key);
        if (!isKnownTopLevelKey(name)) {
            result.warnings.append(QStringLiteral("%1 is not a key this shell reads").arg(keyPath("", name)));
            continue;
        }
        if (name == "schema_version")
            continue;  // read above, before any of this could be applied

        const toml::table* barTable = node.as_table();
        if (barTable == nullptr) {
            result.warnings.append(QStringLiteral("%1: expected a table, found %2; keeping the defaults "
                                                  "for it")
                                       .arg(keyPath("", name), typeName(node)));
            continue;
        }
        readBarTable(*barTable, result.values.bar, result.warnings);
    }

    return result;
}

std::optional<QVariant> configValueForPath(const ConfigValues& values, QStringView path) {
    // Compared against the constants above rather than against literals, so the list a caller is offered
    // and the paths that resolve are the same list: a key that resolves but is not offered, or the
    // reverse, is what the guard in config-test exists to catch.
    if (path == QLatin1StringView(KeyBarHeight))
        return QVariant(values.bar.height);
    if (path == QLatin1StringView(KeyBarLayerNamespace))
        return QVariant(values.bar.layerNamespace);
    if (path == QLatin1StringView(KeyBarSystemSampleIntervalMs))
        return QVariant(values.bar.system.sampleIntervalMs);
    if (path == QLatin1StringView(KeyBarSystemShowCpu))
        return QVariant(values.bar.system.showCpu);
    if (path == QLatin1StringView(KeyBarSystemShowMemory))
        return QVariant(values.bar.system.showMemory);
    if (path == QLatin1StringView(KeyBarSystemMemoryFormat))
        return QVariant(values.bar.system.memoryFormat);
    if (path == QLatin1StringView(KeyBarAudioShowVolume))
        return QVariant(values.bar.audio.showVolume);
    if (path == QLatin1StringView(KeyBarAudioVolumeScale))
        return QVariant(values.bar.audio.volumeScale);
    if (path == QLatin1StringView(KeyBarAudioStepDecibels))
        return QVariant(values.bar.audio.stepDecibels);
    if (path == QLatin1StringView(KeyBarAudioStepPercent))
        return QVariant(values.bar.audio.stepPercent);
    if (path == QLatin1StringView(KeyBarNetworkShowStatus))
        return QVariant(values.bar.network.showStatus);
    if (path == QLatin1StringView(KeyBarNetworkShowName))
        return QVariant(values.bar.network.showName);
    if (path == QLatin1StringView(KeyBarNetworkShowStrength))
        return QVariant(values.bar.network.showStrength);
    if (path == QLatin1StringView(KeyBarBatteryShowStatus))
        return QVariant(values.bar.battery.showStatus);
    if (path == QLatin1StringView(KeyBarBatteryShowPercentage))
        return QVariant(values.bar.battery.showPercentage);
    if (path == QLatin1StringView(KeyBarBatteryShowTime))
        return QVariant(values.bar.battery.showTime);
    if (path == QLatin1StringView(KeyBarMediaShowMedia))
        return QVariant(values.bar.media.showMedia);
    if (path == QLatin1StringView(KeyBarNotificationsShowNotifications))
        return QVariant(values.bar.notifications.showNotifications);
    return std::nullopt;
}

}  // namespace quantum::config
