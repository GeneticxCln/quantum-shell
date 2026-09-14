#include "config/ConfigSchema.h"

#include <toml++/toml.hpp>

#include <exception>
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
    return key == "height" || key == "namespace";
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

// `[bar]` keys. Each known key is read if it is present and of a usable type; anything else leaves the
// default standing and says so. A key of the wrong type is reported and not coerced: `height = "32"`
// is a mistake, and reading it as 32 would hide it.
void readBarTable(const toml::table& table, BarConfig& bar, QStringList& warnings) {
    for (const auto& [key, node] : table) {
        const std::string_view name = keyText(key);
        if (!isKnownBarKey(name)) {
            warnings.append(QStringLiteral("%1 is not a key this shell reads; known keys in [bar]: "
                                           "height, namespace")
                                .arg(keyPath("bar", name)));
            continue;
        }

        const QString path = keyPath("bar", name);

        if (name == "height") {
            const std::optional<int64_t> height = node.value<int64_t>();
            if (!height.has_value()) {
                warnings.append(QStringLiteral("%1: expected an integer, found %2; keeping %3")
                                    .arg(path, typeName(node))
                                    .arg(bar.height));
                continue;
            }
            if (*height < 1) {
                // Zero is not a way of declining a height: a surface still has to be given one, and it is
                // the value the compositor reserves from the tiling area.
                warnings.append(QStringLiteral("%1: %2 is not a height the bar can have (it must be at "
                                               "least 1); keeping %3")
                                    .arg(path)
                                    .arg(*height)
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
    return std::nullopt;
}

}  // namespace quantum::config
