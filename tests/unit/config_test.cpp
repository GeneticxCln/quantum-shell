// The configuration as QML reads it: the schema's rules, the diffing that decides what a change emits,
// and a binding evaluated in a real QML engine with no display.
//
// The schema is tested by handing it text, which is the whole reason it is a pure function of the file:
// every rule below is a case in a string rather than a file on disk. The watcher — the part that has to
// talk to a filesystem — is a separate binary, `config-watcher-test`.
#include "config/Config.h"
#include "config/ConfigSchema.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>

#include <array>
#include <memory>
#include <optional>
#include <string_view>

using quantum::config::BarConfig;
using quantum::config::Config;
using quantum::config::ConfigBar;
using quantum::config::ConfigValues;
using quantum::config::parseConfig;
using quantum::config::ParseResult;

namespace {

// The shell's shipped values, written out here independently of the schema and compared with it below.
// The duplication is the point: these are what the documents and the live layer-shell test quote, so a
// default that moves has to move in all three places at once rather than only in the code.
constexpr int coveredDefaultHeight = 32;
constexpr auto coveredDefaultNamespace = "quantum-shell-bar";

// The key paths the shell offers to `qsctl config get`, mirrored the same way and for a sharper reason: a
// path is what a script types. It is interface, so a rename has to be acknowledged here — and in the
// document that lists the keys — rather than being free to happen in the schema alone.
constexpr std::array<const char*, 2> coveredKeyPaths{"bar.height", "bar.layerNamespace"};

template <std::size_t Declared, std::size_t Covered>
constexpr bool samePaths(const std::array<const char*, Declared>& declared,
                         const std::array<const char*, Covered>& covered) {
    if (Declared != Covered) {
        return false;
    }
    for (std::size_t index = 0; index < Declared; ++index) {
        if (std::string_view(declared[index]) != std::string_view(covered[index])) {
            return false;
        }
    }
    return true;
}

static_assert(samePaths(quantum::config::KeyPaths, coveredKeyPaths),
              "the configuration key paths changed: update the mirror above, and every document that names them");

bool mentions(const QStringList& messages, const QString& key) {
    for (const QString& message : messages) {
        if (message.contains(key))
            return true;
    }
    return false;
}

}  // namespace

class ConfigTest : public QObject {
    Q_OBJECT

private slots:
    void defaultsAreWhatTheShellShipsWithoutAFile();
    void readsEveryKeyTheBarUses();
    void warnsAboutEveryUnknownKeyByName();
    void aValueOfTheWrongTypeIsReportedAndTheDefaultKept();
    void refusesANamespaceOutsideTheFrozenPrefix();
    void refusesAHeightTheBarCannotHave();
    void refusesAFileFromASchemaVersionItDoesNotKnow();
    void warnsWhenTheSchemaVersionIsMissingButStillUsesTheFile();
    void refusesAFileThatIsNotToml();
    void onlyThePropertyThatChangedEmits();
    void everyPropertyIsOneABindingNeeds();
    void qmlReadsTheSameValuesAndFollowsAChange();
    void everyKeyPathTheSchemaOffersResolvesAndNothingElseDoes();
};

void ConfigTest::defaultsAreWhatTheShellShipsWithoutAFile() {
    // A file that says nothing — empty, or holding only comments — is a valid TOML document with no keys
    // in it, so every value falls back to its default and the only thing to report is the missing
    // `schema_version`. A file that is not there at all is a different case and not one this function
    // sees: the watcher turns that into the defaults without a word, which is where `aMissingFileIsNotAProblem`
    // checks it.
    for (const QByteArray& text : {QByteArray(""), QByteArray("# only a comment\n")}) {
        const ParseResult result = parseConfig(text);
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join(QStringLiteral("; "))));
        QCOMPARE(result.warnings.size(), 1);
        QVERIFY(mentions(result.warnings, QStringLiteral("schema_version")));
        QCOMPARE(result.values.bar.height, coveredDefaultHeight);
        QCOMPARE(result.values.bar.layerNamespace, QString::fromLatin1(coveredDefaultNamespace));
    }

    // And the objects the shell actually starts with hold those same values, rather than leaving them to
    // the schema: `Config` is what a binding reads before any file has been parsed.
    Config config;
    QCOMPARE(config.bar()->height(), coveredDefaultHeight);
    QCOMPARE(config.bar()->layerNamespace(), QString::fromLatin1(coveredDefaultNamespace));
}

void ConfigTest::readsEveryKeyTheBarUses() {
    const ParseResult result = parseConfig(QByteArray(
        "schema_version = 1\n"
        "\n"
        "[bar]\n"
        "height = 44\n"
        "namespace = \"quantum-shell-bar-alt\"\n"));

    QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join(QStringLiteral("; "))));
    QVERIFY2(result.warnings.isEmpty(), qPrintable(result.warnings.join(QStringLiteral("; "))));
    QCOMPARE(result.values.bar.height, 44);
    QCOMPARE(result.values.bar.layerNamespace, QStringLiteral("quantum-shell-bar-alt"));
}

void ConfigTest::warnsAboutEveryUnknownKeyByName() {
    const ParseResult result = parseConfig(QByteArray(
        "schema_version = 1\n"
        "transparency = 0.5\n"
        "\n"
        "[bar]\n"
        "height = 40\n"
        "widht = 3\n"));

    QVERIFY(result.errors.isEmpty());
    // Each unknown key is named by the path a user would search the file for, and each is a warning
    // rather than an error: the keys the shell does read were usable and have been applied.
    QCOMPARE(result.warnings.size(), 2);
    QVERIFY(mentions(result.warnings, QStringLiteral("transparency")));
    QVERIFY(mentions(result.warnings, QStringLiteral("bar.widht")));
    QCOMPARE(result.values.bar.height, 40);
}

void ConfigTest::aValueOfTheWrongTypeIsReportedAndTheDefaultKept() {
    const ParseResult result = parseConfig(QByteArray(
        "schema_version = 1\n"
        "\n"
        "[bar]\n"
        "height = \"44\"\n"
        "namespace = 7\n"));

    QVERIFY(result.errors.isEmpty());
    QCOMPARE(result.warnings.size(), 2);
    QVERIFY(mentions(result.warnings, QStringLiteral("bar.height")));
    QVERIFY(mentions(result.warnings, QStringLiteral("an integer")));
    QVERIFY(mentions(result.warnings, QStringLiteral("bar.namespace")));
    QVERIFY(mentions(result.warnings, QStringLiteral("a string")));

    // Not coerced. `height = "44"` is a mistake, and reading it as 44 would hide it.
    QCOMPARE(result.values.bar.height, coveredDefaultHeight);
    QCOMPARE(result.values.bar.layerNamespace, QString::fromLatin1(coveredDefaultNamespace));
}

void ConfigTest::refusesANamespaceOutsideTheFrozenPrefix() {
    // AGENTS.md freezes the shell's layer-shell namespaces as `quantum-shell-*`. The integration refuses
    // a foreign name before it reaches the protocol; this is the earlier refusal, and the one that can
    // name the key that has to be edited.
    const ParseResult result = parseConfig(QByteArray(
        "schema_version = 1\n"
        "\n"
        "[bar]\n"
        "namespace = \"noctalia-bar\"\n"));

    QVERIFY(result.errors.isEmpty());
    QCOMPARE(result.warnings.size(), 1);
    QVERIFY(mentions(result.warnings, QStringLiteral("bar.namespace")));
    QVERIFY(mentions(result.warnings, QString::fromLatin1(coveredDefaultNamespace)));
    QCOMPARE(result.values.bar.layerNamespace, QString::fromLatin1(coveredDefaultNamespace));
}

void ConfigTest::refusesAHeightTheBarCannotHave() {
    // Zero is not a way of declining a height: the surface still has to be given one, and it is the value
    // the compositor reserves from the tiling area. A negative height is not a size at all.
    for (const QByteArray& line : {QByteArray("height = 0\n"), QByteArray("height = -3\n")}) {
        const ParseResult result = parseConfig(QByteArray("schema_version = 1\n[bar]\n") + line);
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.warnings.size(), 1);
        QVERIFY(mentions(result.warnings, QStringLiteral("bar.height")));
        QCOMPARE(result.values.bar.height, coveredDefaultHeight);
    }
}

void ConfigTest::refusesAFileFromASchemaVersionItDoesNotKnow() {
    // A file written for a newer shell is refused as a whole rather than partly understood: a key that
    // has changed meaning is a value this build would be inventing, and this build's defaults are at
    // least a configuration somebody reasoned about.
    const ParseResult result = parseConfig(QByteArray(
        "schema_version = 2\n"
        "[bar]\n"
        "height = 44\n"));

    QVERIFY(result.warnings.isEmpty());
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(mentions(result.errors, QStringLiteral("schema_version")));
    QVERIFY(mentions(result.errors, QStringLiteral("not applied")));
    QCOMPARE(result.values.bar.height, coveredDefaultHeight);

    // The same is true of a version that is not a version: the value is unusable rather than merely
    // unknown, and guessing at it would be worse than refusing the file.
    const ParseResult wrongType = parseConfig(QByteArray("schema_version = \"1\"\n[bar]\nheight = 44\n"));
    QCOMPARE(wrongType.errors.size(), 1);
    QCOMPARE(wrongType.values.bar.height, coveredDefaultHeight);
}

void ConfigTest::warnsWhenTheSchemaVersionIsMissingButStillUsesTheFile() {
    // Missing keys fall back to defaults, and this one has a default like any other — but a file without
    // it is a file written before the field existed, which is exactly the case a migration is written
    // for, so it is worth saying out loud rather than passing over.
    const ParseResult result = parseConfig(QByteArray("[bar]\nheight = 44\n"));

    QVERIFY(result.errors.isEmpty());
    QCOMPARE(result.warnings.size(), 1);
    QVERIFY(mentions(result.warnings, QStringLiteral("schema_version")));
    QCOMPARE(result.values.bar.height, 44);
}

void ConfigTest::refusesAFileThatIsNotToml() {
    const ParseResult result = parseConfig(QByteArray("this is not toml {{{"));

    QVERIFY(result.warnings.isEmpty());
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(mentions(result.errors, QStringLiteral("TOML")));
    QCOMPARE(result.values.bar.height, coveredDefaultHeight);
}

void ConfigTest::onlyThePropertyThatChangedEmits() {
    Config config;
    QSignalSpy heightSpy(config.bar(), &ConfigBar::heightChanged);
    QSignalSpy namespaceSpy(config.bar(), &ConfigBar::layerNamespaceChanged);

    // A whole value set applied twice: the first changes one property, the second changes nothing.
    ConfigValues values;
    values.bar.height = 40;
    config.apply(values);
    QCOMPARE(heightSpy.count(), 1);
    QCOMPARE(namespaceSpy.count(), 0);
    QCOMPARE(config.bar()->height(), 40);

    config.apply(values);
    QCOMPARE(heightSpy.count(), 1);
    QCOMPARE(namespaceSpy.count(), 0);

    // And the other property on its own, to show neither is being carried by the other's signal.
    values.bar.layerNamespace = QStringLiteral("quantum-shell-bar-alt");
    config.apply(values);
    QCOMPARE(heightSpy.count(), 1);
    QCOMPARE(namespaceSpy.count(), 1);
    QCOMPARE(config.bar()->layerNamespace(), QStringLiteral("quantum-shell-bar-alt"));
}

void ConfigTest::everyPropertyIsOneABindingNeeds() {
    // The QML-visible property set, written out here rather than read from the classes, so that a
    // property added to `Config` or `ConfigBar` without a decision about what it is for fails this test
    // instead of being noticed by an audit later.
    QStringList expected{QStringLiteral("Config.bar"), QStringLiteral("Config.bar.height"),
                         QStringLiteral("Config.bar.layerNamespace")};

    Config config;
    const QMetaObject* rootMeta = config.metaObject();
    const QMetaObject* barMeta = config.bar()->metaObject();

    QStringList actual;
    for (int index = rootMeta->propertyOffset(); index < rootMeta->propertyCount(); ++index) {
        actual.append(QStringLiteral("Config.") + QString::fromLatin1(rootMeta->property(index).name()));
    }
    for (int index = barMeta->propertyOffset(); index < barMeta->propertyCount(); ++index) {
        const QMetaProperty property = barMeta->property(index);
        actual.append(QStringLiteral("Config.bar.") + QString::fromLatin1(property.name()));
        // A property a bar binds to without a NOTIFY signal is one that never updates: the binding is
        // evaluated once and the bar keeps showing the value it was built with. Only the leaves are
        // checked, because `Config.bar` is deliberately CONSTANT — the object behind it does not change,
        // the values inside it do.
        QVERIFY2(property.hasNotifySignal(), qPrintable(property.name()));
    }

    actual.sort();
    expected.sort();
    QCOMPARE(actual, expected);
}

void ConfigTest::qmlReadsTheSameValuesAndFollowsAChange() {
    Config config;
    Config::registerQmlSingleton(config);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    // The two bindings the bar itself makes, written the way `qml/Main.qml` writes them. The import line
    // is spelled out rather than substituted, because it is the interface being checked: a module URI
    // that changes breaks this line exactly as it would break the bar.
    const QString source =
        QStringLiteral("import QtQml\n"
                       "import QuantumShell 1.0\n"
                       "QtObject {\n"
                       "    property int barHeight: Config.bar.height\n"
                       "    property string layerNamespace: Config.bar.layerNamespace\n"
                       "}\n");
    component.setData(source.toUtf8(), QUrl(QStringLiteral("qrc:/test/ConfigBinding.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    std::unique_ptr<QObject> bound(component.create());
    QVERIFY2(bound != nullptr, qPrintable(component.errorString()));

    QCOMPARE(bound->property("barHeight").toInt(), coveredDefaultHeight);
    QCOMPARE(bound->property("layerNamespace").toString(),
             QString::fromLatin1(coveredDefaultNamespace));

    // Now the configuration changes and nothing in the QML is touched: the bindings re-evaluate because
    // the notify signals fired. Both directions are checked — the edited value follows, and the value
    // nobody edited is still what the bar was built with.
    ConfigValues values;
    values.bar.height = 44;
    config.apply(values);

    QCOMPARE(bound->property("barHeight").toInt(), 44);
    QCOMPARE(bound->property("layerNamespace").toString(),
             QString::fromLatin1(coveredDefaultNamespace));
}

void ConfigTest::everyKeyPathTheSchemaOffersResolvesAndNothingElseDoes() {
    const ConfigValues defaults;

    // Every path the schema offers resolves, and to the value the shell would use: a path offered to a
    // script that resolved to nothing would be a name that lies about what this build reads.
    for (const char* path : quantum::config::KeyPaths) {
        const QString key = QString::fromLatin1(path);
        const std::optional<QVariant> value = quantum::config::configValueForPath(defaults, key);
        QVERIFY2(value.has_value(), qPrintable(key));
    }

    // Resolved from the values it is given rather than from anything remembered, which is checked by asking
    // twice with a file's values in between.
    const ParseResult parsed = parseConfig(QByteArrayLiteral("schema_version = 1\n"
                                                            "[bar]\n"
                                                            "height = 44\n"
                                                            "namespace = \"quantum-shell-bar-two\"\n"));
    QVERIFY2(parsed.errors.isEmpty(), qPrintable(parsed.errors.join(QStringLiteral("; "))));
    QCOMPARE(quantum::config::configValueForPath(parsed.values, QStringLiteral("bar.height"))->toInt(), 44);
    QCOMPARE(quantum::config::configValueForPath(parsed.values, QStringLiteral("bar.layerNamespace"))
                 ->toString(),
             QStringLiteral("quantum-shell-bar-two"));

    // And every shape of path that is not one of those keys is nothing, never an empty string or a default:
    // the server turns "nothing" into a refusal naming the path, and a plausible value into a wrong answer.
    for (const char* path : {"bar", "bar.widht", "bar.height.px", "schema_version", "theme", ""}) {
        QVERIFY2(!quantum::config::configValueForPath(parsed.values, QString::fromLatin1(path)).has_value(), path);
    }
}

QTEST_GUILESS_MAIN(ConfigTest)

#include "config_test.moc"
