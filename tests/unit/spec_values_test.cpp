// The engineering spec's numbers, against the code's.
//
// `ENGINEERING_SPEC.md` is a derived document: every value in it is a claim about code, and its own §9 says a
// value there that code contradicts is the "file that lies" failure the public-names check exists to prevent.
// Its *names* are guarded already — `public-names-test` reads that document like the other two — but nothing
// read its numbers: change a default in a header, widen a bound, move a version floor, and the document keeps
// stating the old one until somebody happens to notice. This is the other half.
//
// It is not a mirror of the constants. A mirror repeats a value and compares it, so it catches a change to
// the constant and nothing else; this reads the document and compares it with the value the libraries were
// *compiled* from, so a change on either side fails here. That is also why it links the libraries instead of
// parsing headers with regular expressions: a constant that was renamed or moved cannot be read out of a
// header by accident, it stops the build.
//
// Two kinds of fact, and the difference is stated rather than hidden. Most are constants declared in headers
// this binary links. A few — the /proc file cap, the audio module's retry pair — are declared in a `.cpp`
// file, where nothing can link them, so those are read from the source text with the exact declaration
// required: that catches a changed value, and it cannot catch a constant moved to another file. Each one says
// so where it is checked rather than being left to look like the others.
//
// The document is read from the source tree this binary was configured against, named at configure time
// (`QS_SPEC_ROOT`) for the reason `bar-interaction-test` takes the shipped QML the same way: a test that
// searched the filesystem for the document could pass by finding a different copy of it.
//
// What this cannot see, said plainly: a fact the document states that no code holds — a sentence about
// behaviour, a limitation, a budget that is prose. Those are read by people, and the ones that matter have a
// test of their own under §4 of the document.

#include "audio/AudioVolume.h"
#include "audio/PipeWireService.h"
#include "config/ConfigSchema.h"
#include "config/Config.h"
#include "ipc/IPCProtocol.h"
#include "niri/NiriService.h"
#include "dbus/NetworkService.h"
#include "dbus/BatteryService.h"
#include "dbus/MediaService.h"
#include "dbus/NotificationService.h"
#include "system/SysMonService.h"
#include "niri/NiriActions.h"
#include "niri/NiriProtocol.h"
#include "niri/NiriReconnect.h"
#include "niri/NiriVersion.h"

#include <QtTest>

#include <QChar>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <array>
#include <limits>
#include <vector>

#ifndef QS_SPEC_ROOT
#error "spec-values-test needs QS_SPEC_ROOT: the source tree whose document it reads is named at configure time"
#endif

namespace {

// The files the document is compared against, all of them read once. Every entry is a source of truth the
// document makes claims about, so a missing one would turn those claims into untested prose rather than a
// failure. The document itself is not on the list: it is the thing being read, and `initTestCase` opens it
// before these so a document that cannot be read fails everything rather than one section.
const std::array<const char*, 7> sourceFiles{
    "CMakeLists.txt",
    "tests/public_names.cmake",
    "tests/CMakeLists.txt",
    "src/system/SysMonService.cpp",
    "src/audio/PipeWireService.cpp",
    "src/niri/NiriVersion.h",
    // Read for §2.6's one row whose class this binary does not link, and for the reason the header gives
    // beside that row: the bar's window derives from `QQuickWindow`, and the link is what the sanitizer job
    // would then report as a leak of the font stack's own caches.
    "src/wayland/LayerShellWindow.h",
};

// The first number in `text`, as written: `32`, and `10` from `int \`10..INT_MAX\``. Returned as text rather
// than as a number so a comparison failure prints both spellings, which is what a person needs to see.
QString firstNumber(const QString& text)
{
    static const QRegularExpression digits(QStringLiteral("[0-9]+"));
    const QRegularExpressionMatch match = digits.match(text);
    return match.hasMatch() ? match.captured() : QString();
}

// The first version — one or more dot-separated numbers — in `text`. Separate from `firstNumber` because a
// version is not an integer: `>=1.6` read as a number is `1`, and `@v3.4.0` read as a number is `3`, so a
// comparison built on the wrong one would pass on values that differ.
QString firstVersion(const QString& text)
{
    static const QRegularExpression version(QStringLiteral("[0-9]+(?:\\.[0-9]+)*"));
    const QRegularExpressionMatch match = version.match(text);
    return match.hasMatch() ? match.captured() : QString();
}

// What follows the first `marker`, so a cell or a line that names two versions — `libpipewire-0.3 >= 1.6` —
// can be read for the one it is a floor of rather than for the first number in the whole string. Empty when
// the marker is absent, which every caller asserts by comparing what it gets.
QString after(const QString& text, const QString& marker)
{
    const qsizetype at = text.indexOf(marker);
    return at < 0 ? QString() : text.sliced(at + marker.size());
}

// `6.11.0` and `6.11` are the same floor: the patch a CI image installs is not what the build requires.
QString majorMinor(const QString& text)
{
    static const QRegularExpression pair(QStringLiteral("([0-9]+)\\.([0-9]+)"));
    const QRegularExpressionMatch match = pair.match(text);
    return match.hasMatch() ? match.captured() : QString();
}

// Every backticked token in a cell, which is how the document lists the values something may take: the four
// memory forms, the config paths. Compared as a set, so a token dropped from the document fails here and a
// token invented in it does too.
QStringList backtickedTokens(const QString& text)
{
    QStringList tokens;
    const QStringList parts = text.split(QLatin1Char('`'));
    for (qsizetype index = 1; index < parts.size(); index += 2)
        tokens.append(parts.at(index).trimmed());
    return tokens;
}

// A documentation cell names a call as `stepVolume(dir)` or `focusWorkspaceUp()`; the meta-object knows it as
// `stepVolume`. The argument list is the document's way of saying what a call takes, and it is prose about the
// signature, so it comes off before names are compared.
QString nameOfToken(const QString& token)
{
    const qsizetype parenthesis = token.indexOf(QLatin1Char('('));
    return parenthesis < 0 ? token : token.left(parenthesis);
}

// The properties a class declares itself, in declaration order. `propertyOffset` is where the superclasses'
// properties end, which is what keeps `objectName` out of a comparison about a service and keeps
// `QQuickWindow`'s several dozen out of one about the bar's window.
QStringList ownProperties(const QMetaObject& meta)
{
    QStringList names;
    for (int index = meta.propertyOffset(); index < meta.propertyCount(); ++index)
        names.append(QLatin1String(meta.property(index).name()));
    return names;
}

// The `Q_INVOKABLE` methods a class declares itself: not its slots, which Qt also exposes to QML but which
// this project does not use for a QML-facing call, and not its signals, which are the notify half of a
// property rather than something a widget calls.
QStringList ownInvokables(const QMetaObject& meta)
{
    QStringList names;
    for (int index = meta.methodOffset(); index < meta.methodCount(); ++index) {
        const QMetaMethod method = meta.method(index);
        if (method.access() == QMetaMethod::Public && method.methodType() == QMetaMethod::Method)
            names.append(QLatin1String(method.name()));
    }
    return names;
}

// Everything a class declares itself that a QML file can name: properties, invokable methods and signals. This
// is the set a documented name has to be found in, which is the direction that catches a token the code never
// had — a renamed property leaves its old name nowhere to resolve.
QStringList ownMembers(const QMetaObject& meta)
{
    QStringList names = ownProperties(meta);
    names += ownInvokables(meta);
    for (int index = meta.methodOffset(); index < meta.methodCount(); ++index) {
        const QMetaMethod method = meta.method(index);
        if (method.access() == QMetaMethod::Public && method.methodType() == QMetaMethod::Signal)
            names.append(QLatin1String(method.name()));
    }
    return names;
}

// The property and invokable names a header declares, read from its own `Q_PROPERTY` and `Q_INVOKABLE`
// declarations. This exists for the one class in §2.6 this binary cannot link (its header's comment says
// why), and it is the weaker source on purpose: it catches a name that was renamed, added or removed in the
// declaration text, and it cannot catch a disagreement between a declaration and what moc built from it —
// which is a thing moc does not permit. A property's name ends at the first access keyword, so the type and
// the accessors around it do not have to be parsed, and an invokable's ends at its opening parenthesis.
QStringList propertyNamesIn(const QString& header)
{
    QStringList names;
    static const QRegularExpression declaration(QStringLiteral(
        "Q_PROPERTY\\(\\s*[\\w:<>*&,\\s]*?[\\s*&](\\w+)\\s+(?:READ|WRITE|MEMBER|NOTIFY|CONSTANT|FINAL|REQUIRED)"));
    QRegularExpressionMatchIterator matches = declaration.globalMatch(header);
    while (matches.hasNext())
        names.append(matches.next().captured(1));
    return names;
}

QStringList invokableNamesIn(const QString& header)
{
    QStringList names;
    static const QRegularExpression declaration(
        QStringLiteral("Q_INVOKABLE\\s+[\\w:<>*&\\s]*?[\\s*&]?(\\w+)\\s*\\("));
    QRegularExpressionMatchIterator matches = declaration.globalMatch(header);
    while (matches.hasNext())
        names.append(matches.next().captured(1));
    return names;
}

// The names in `from` that `against` does not have, sorted. Both directions of a §2.6 row's comparison end
// with one of these, so a failure names the one name that is out of step instead of printing a documented list
// and a declared list for the reader to diff by eye.
QStringList onlyIn(const QStringList& from, const QStringList& against)
{
    QStringList result;
    for (const QString& name : from) {
        if (!against.contains(name))
            result.append(name);
    }
    result.sort();
    return result;
}

// The number a `name=<milliseconds>` entry of the shard cost table carries, or empty when the table does not
// mention that binary — which is a failure at the call site rather than an absence with no consequence.
QString costOf(const QString& testList, const QString& binary)
{
    static const QRegularExpression entry(QStringLiteral("(^|[^\\w-])([a-z0-9-]+)=(\\d+)"));
    QRegularExpressionMatchIterator matches = entry.globalMatch(testList);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        if (match.captured(2) == binary)
            return match.captured(3);
    }
    return QString();
}

}  // namespace

class SpecValuesTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void theToolchainFloorsInTheTableAreTheOnesTheBuildRequires();
    void everyConfigRowStatesTheDefaultAndTheFloorTheSchemaApplies();
    void theConfigTableNamesEveryConfigPathTheSchemaResolves();
    void theFrozenNamesInTheTableAreTheOnesTheHeadersDeclare();
    void theBoundsStatedForTheConnectionsAreTheOnesTheSourcesCarry();
    void everySingletonRowNamesExactlyWhatTheCodeExposes();
    void theConfigRowDescribesTheObjectTreeItNames();
    void theSuiteCountsAndTheMeasuredCostsAreTheOnesTheBuildDeclares();

private:
    // A table row, split on its pipes with the cells trimmed. `found` is false when no line of the document
    // contains the anchor, and every caller asserts it: a row deleted from the document must fail this test
    // rather than be read as an absence of disagreement, which is what a lookup returning an empty string
    // would turn it into.
    struct Row {
        bool found = false;
        QStringList cells;
    };

    // A documented singleton row's names, and whether the row was there at all. Returned rather than asserted
    // here because `QVERIFY` inside a helper returns from the helper: a missing row would then let the slot
    // carry on comparing an empty list, which is a check that passes while asserting nothing. Every call site
    // asserts `found` itself.
    struct DocumentedRow {
        bool found = false;
        QStringList names;
    };
    DocumentedRow documentedNames(const QString& anchor) const;

    Row row(const QString& anchor) const;
    QString lineWith(const QString& anchor) const;
    QString source(const QString& relativePath) const;
    // A slice of the document from one marker to the next. Used wherever a claim is about what a paragraph
    // says rather than about one line of it: the document wraps its sentences, and a check that required a
    // number and a unit to be on the same physical line would fail on a re-wrap rather than on a change.
    QString region(const QString& from, const QString& to) const;

    QString document_;
    QHash<QString, QString> sources_;
};

void SpecValuesTest::initTestCase()
{
    const QString root = QString::fromUtf8(QS_SPEC_ROOT);
    QFile document(root + QStringLiteral("/ENGINEERING_SPEC.md"));
    QVERIFY2(document.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(QStringLiteral("cannot read %1: %2")
                            .arg(document.fileName(), document.errorString())));
    document_ = QString::fromUtf8(document.readAll());
    // Length rather than emptiness: a document truncated to a heading would answer every lookup with nothing,
    // and a test that compared nothing with the code would pass while checking nothing at all.
    QVERIFY2(document_.size() > 10000,
             qPrintable(QStringLiteral("ENGINEERING_SPEC.md is %1 bytes, which is too short to be the "
                                       "document this test reads")
                            .arg(document_.size())));

    for (const char* relative : sourceFiles) {
        QFile file(root + QLatin1Char('/') + QLatin1String(relative));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(QStringLiteral("cannot read %1: %2").arg(file.fileName(), file.errorString())));
        sources_.insert(QLatin1String(relative), QString::fromUtf8(file.readAll()));
    }
}

SpecValuesTest::Row SpecValuesTest::row(const QString& anchor) const
{
    Row result;
    for (const QString& line : document_.split(QLatin1Char('\n'))) {
        if (!line.contains(anchor))
            continue;
        result.found = true;
        QStringList pieces = line.split(QLatin1Char('|'));
        if (!pieces.isEmpty() && pieces.first().trimmed().isEmpty())
            pieces.removeFirst();
        if (!pieces.isEmpty() && pieces.last().trimmed().isEmpty())
            pieces.removeLast();
        for (const QString& piece : pieces)
            result.cells.append(piece.trimmed());
        break;
    }
    return result;
}

// The names a row of §2.6 states, in the order the document writes them sorted so the comparison is about
// names rather than about prose order. `nameOfToken` takes the argument list off a documented call
// (`present()`, `stepVolume(dir)`), because the argument list describes a signature and the name is what QML
// resolves.
SpecValuesTest::DocumentedRow SpecValuesTest::documentedNames(const QString& anchor) const
{
    DocumentedRow result;
    const Row row_ = row(anchor);
    result.found = row_.found;
    if (!row_.found)
        return result;
    for (const QString& token : backtickedTokens(row_.cells.value(1)))
        result.names.append(nameOfToken(token));
    result.names.sort();
    return result;
}

QString SpecValuesTest::lineWith(const QString& anchor) const
{
    for (const QString& line : document_.split(QLatin1Char('\n'))) {
        if (line.contains(anchor))
            return line.trimmed();
    }
    return QString();
}

QString SpecValuesTest::source(const QString& relativePath) const
{
    return sources_.value(relativePath);
}

QString SpecValuesTest::region(const QString& from, const QString& to) const
{
    const qsizetype start = document_.indexOf(from);
    if (start < 0)
        return QString();
    const qsizetype end = document_.indexOf(to, start + from.size());
    return document_.sliced(start, end < 0 ? -1 : end - start);
}

// §1. The table states a floor per component and the build enforces one; they are the same floor.
void SpecValuesTest::theToolchainFloorsInTheTableAreTheOnesTheBuildRequires()
{
    const QString build = source(QStringLiteral("CMakeLists.txt"));
    QVERIFY2(!build.isEmpty(), "CMakeLists.txt was not read");

    // The table's three columns are the component, its floor, and what the floor is pinned to in practice:
    // an index into the cells is a column number minus one, and the index is what these checks read.
    const Row qt = row(QStringLiteral("| Qt |"));
    QVERIFY2(qt.found, "the toolchain table has no Qt row");
    // The marker carries the trailing space so the `6` of `Qt6` is not read as the version: the number sought
    // is the one the package name is followed by, and the patch level is not part of the floor.
    QCOMPARE(majorMinor(qt.cells.value(1)),
             majorMinor(firstVersion(after(build, QStringLiteral("find_package(Qt6 ")))));

    // niri's floor is not a build file's: it is the pair of integers the version parser compares against, and
    // the document states it as `26.04`. Built from the constants so a change to either fails here.
    const Row niri = row(QStringLiteral("| niri |"));
    QVERIFY2(niri.found, "the toolchain table has no niri row");
    QCOMPARE(niri.cells.value(1),
             QStringLiteral("%1.%2")
                 .arg(quantum::niri::NiriVersion::minimumYear, 2, 10, QLatin1Char('0'))
                 .arg(quantum::niri::NiriVersion::minimumMonth, 2, 10, QLatin1Char('0')));
    // The same claim from the other side, because the pinning cell names the constants: a rename shows up as
    // a stale document rather than as a missing comparison.
    QVERIFY2(niri.cells.value(2).contains(QStringLiteral("minimumYear")),
             qPrintable(QStringLiteral("the niri row no longer names the constants it is pinned to: %1")
                            .arg(niri.cells.value(2))));

    const Row cpp = row(QStringLiteral("| C++ |"));
    QVERIFY2(cpp.found, "the toolchain table has no row for the language standard");
    const QString standard = firstNumber(after(build, QStringLiteral("CMAKE_CXX_STANDARD")));
    QVERIFY2(cpp.cells.value(1).contains(QStringLiteral("C++%1").arg(standard)),
             qPrintable(QStringLiteral("the document's floor says %1 while the build sets %2")
                            .arg(cpp.cells.value(1), standard)));
    // "No extensions" is part of the floor rather than a detail of it: a GNU extension in the tree is what
    // this setting exists to refuse, so the document stating the floor states this too.
    QVERIFY2(cpp.cells.value(1).contains(QStringLiteral("no extensions")),
             "the C++ row no longer states that extensions are off");
    QVERIFY2(build.contains(QStringLiteral("CXX_EXTENSIONS OFF")),
             "the build no longer turns extensions off, and the document says it does");

    const Row cmake = row(QStringLiteral("| CMake |"));
    QVERIFY2(cmake.found, "the toolchain table has no CMake row");
    QCOMPARE(cmake.cells.value(1), firstVersion(after(build, QStringLiteral("cmake_minimum_required(VERSION"))));

    const Row toml = row(QStringLiteral("| toml++ |"));
    QVERIFY2(toml.found, "the toolchain table has no toml++ row");
    QCOMPARE(toml.cells.value(1), firstVersion(after(build, QStringLiteral("find_package(tomlplusplus"))));

    // PipeWire's row names the pkg-config module and then its floor, so the marker is the comparison itself:
    // `libpipewire-0.3 ≥ 1.6` in the document, `libpipewire-0.3>=1.6` in the build.
    const Row pipewire = row(QStringLiteral("| PipeWire |"));
    QVERIFY2(pipewire.found, "the toolchain table has no PipeWire row");
    QCOMPARE(firstVersion(after(pipewire.cells.value(1), QStringLiteral("≥"))),
             firstVersion(after(build, QStringLiteral("libpipewire-0.3"))));
    QVERIFY2(pipewire.cells.value(1).contains(QStringLiteral("libpipewire-0.3")),
             "the PipeWire row no longer names the pkg-config module the build requires");

    // WirePlumber has a row and no comparison: the document calls it a design dependency that is not linked,
    // and there is nothing in the build to hold it against. Stated here so the missing check is a decision.
}

// §2.5. Every row of the config table states a default and a bound, and both are the ones the schema applies.
void SpecValuesTest::everyConfigRowStatesTheDefaultAndTheFloorTheSchemaApplies()
{
    const quantum::config::ConfigValues defaults;

    // A row's bound is either a floor number or a rule stated in words — the namespace's is a prefix, not a
    // value — so each check names whichever kind its row states, and a row whose bound is prose rather than a
    // number says so instead of having an empty comparison quietly skipped.
    struct RowCheck {
        const char* anchor;
        QString defaultValue;
        QString floor;
        QString floorText;
        // The memory form's rule is a list of four tokens rather than a bound or a prefix, and it is compared
        // with the schema's own list below — as a set, from both sides. A row says so here rather than being
        // left with a bound nothing reads.
        bool boundCheckedSeparately = false;
    };
    const std::vector<RowCheck> checks{
        {"| `[bar] height` |", QString::number(defaults.bar.height),
         QString::number(quantum::config::MinBarHeight), {}},
        {"| `[bar] namespace` |", defaults.bar.layerNamespace, {},
         QString::fromUtf8(quantum::config::LayerNamespacePrefix)},
        {"| `[bar.system] sample_interval_ms` |", QString::number(defaults.bar.system.sampleIntervalMs),
         QString::number(quantum::config::MinSampleIntervalMs), {}},
        // The three flags' bound is their type rather than a range: a value that is not a boolean is refused
        // rather than coerced, so the rule the row has to keep stating is that it is one. `config-test` is
        // what proves the coercion does not happen; this is what keeps the document saying so.
        {"| `[bar.system] show_cpu` |",
         defaults.bar.system.showCpu ? QStringLiteral("true") : QStringLiteral("false"), {},
         QStringLiteral("boolean")},
        {"| `[bar.system] show_memory` |",
         defaults.bar.system.showMemory ? QStringLiteral("true") : QStringLiteral("false"), {},
         QStringLiteral("boolean")},
        {"| `[bar.system] memory_format` |", defaults.bar.system.memoryFormat, {}, {}, true},
        {"| `[bar.audio] show_volume` |",
         defaults.bar.audio.showVolume ? QStringLiteral("true") : QStringLiteral("false"), {},
         QStringLiteral("boolean")},
        // The step's two numbers are the *service's*, which is the document's own claim about them: the
        // widget does not decide how far a notch moves, so the row is compared with the constants the module
        // that writes the volume carries rather than with this struct's copy of them.
        {"| `[bar.audio] step_percent` |",
         QString::number(quantum::audio::PipeWireService::DefaultStepPercent),
         QString::number(quantum::audio::PipeWireService::MinimumStepPercent), {}},
        // The version's bound is what a mismatch does rather than a number: the whole file is rejected and
        // the previous configuration stands. `config-test` is the proof; this keeps the row saying it.
        {"| `schema_version` |", QString::number(quantum::config::SchemaVersion), {},
         QStringLiteral("rejected")},
    };

    for (const RowCheck& check : checks) {
        const Row found = row(QLatin1String(check.anchor));
        QVERIFY2(found.found, qPrintable(QStringLiteral("the config table has no row for %1")
                                             .arg(QLatin1String(check.anchor))));
        const QString statedDefault = found.cells.value(2);
        QVERIFY2(statedDefault.contains(check.defaultValue),
                 qPrintable(QStringLiteral("%1 states the default %2 while the schema applies %3")
                                .arg(QLatin1String(check.anchor), statedDefault, check.defaultValue)));
        const QString statedFloor = found.cells.value(3);
        if (!check.floor.isEmpty()) {
            QVERIFY2(firstNumber(statedFloor) == check.floor,
                     qPrintable(QStringLiteral("%1 states the floor %2 while the schema applies %3")
                                    .arg(QLatin1String(check.anchor), statedFloor, check.floor)));
        }
        if (!check.floorText.isEmpty()) {
            QVERIFY2(statedFloor.contains(check.floorText),
                     qPrintable(QStringLiteral("%1 states the rule %2 while the schema applies %3")
                                    .arg(QLatin1String(check.anchor), statedFloor, check.floorText)));
        }
        // Every row has one or the other: a row whose bound went missing entirely would otherwise be read as
        // agreeing with everything.
        QVERIFY2(!check.floor.isEmpty() || !check.floorText.isEmpty() || check.boundCheckedSeparately,
                 qPrintable(QStringLiteral("%1 states neither a floor nor a rule")
                                .arg(QLatin1String(check.anchor))));
    }

    // The four memory forms are a list the schema refuses everything outside of, and the document lists the
    // same four. Compared as a set, from both sides, because a token dropped from either is a form the
    // widget can be handed and does not draw.
    const Row memoryFormat = row(QStringLiteral("| `[bar.system] memory_format` |"));
    QVERIFY2(memoryFormat.found, "the config table has no row for the memory form");
    QStringList statedForms = backtickedTokens(memoryFormat.cells.value(3));
    statedForms.sort();
    QStringList declaredForms;
    for (const char* form : quantum::config::MemoryFormats)
        declaredForms.append(QLatin1String(form));
    declaredForms.sort();
    QCOMPARE(statedForms, declaredForms);

    // And the default the row states is the one the schema's own default is spelled from.
    QVERIFY2(memoryFormat.cells.value(2).contains(QLatin1String(quantum::config::MemoryFormatUsedOfTotal)),
             qPrintable(QStringLiteral("the memory form's default row says %1")
                            .arg(memoryFormat.cells.value(2))));
}

// §2.5's other column, and §2.3's table: the paths and verbs the document lists are the ones that resolve.
void SpecValuesTest::theConfigTableNamesEveryConfigPathTheSchemaResolves()
{
    // Only the config table, sliced out of the document: other tables name `bar.*` values too — the singleton
    // table says `Config` carries `bar.system.*` — and a check that read those as key paths would fail on a
    // table that is not about key paths at all.
    const QString configTable = region(QStringLiteral("### 2.5 Configuration keys"), QStringLiteral("### 2.6"));
    QVERIFY2(!configTable.isEmpty(), "the document has no section for the configuration keys");
    QStringList stated;
    for (const QString& line : configTable.split(QLatin1Char('\n'))) {
        if (!line.startsWith(QStringLiteral("| `")))
            continue;
        const QStringList pieces = line.split(QLatin1Char('|'));
        if (pieces.size() < 3)
            continue;
        // The key-path cell of a row: either `—` for a key with no path, or one backticked path.
        for (const QString& token : backtickedTokens(pieces.at(2).trimmed())) {
            if (token.startsWith(QStringLiteral("bar.")))
                stated.append(token);
        }
    }
    QStringList declared;
    for (const char* path : quantum::config::KeyPaths)
        declared.append(QLatin1String(path));
    stated.sort();
    declared.sort();
    QCOMPARE(stated, declared);

    // The verb table, the same way: the four names on the wire, each as a row, and the count the document
    // states in its heading.
    for (const char* verb : quantum::ipc::verb::All) {
        const QString anchor = QStringLiteral("| `%1").arg(QLatin1String(verb));
        QVERIFY2(row(anchor).found,
                 qPrintable(QStringLiteral("the verb table has no row for `%1`").arg(QLatin1String(verb))));
    }
    QVERIFY2(quantum::ipc::verb::All.size() == 4,
             "the verb list is no longer the four the document calls its whole surface");
    const QString verbHeading = lineWith(QStringLiteral("IPC verbs"));
    QVERIFY2(verbHeading.contains(QStringLiteral("4 entries")),
             qPrintable(QStringLiteral("the verb table's heading no longer states the count: %1").arg(verbHeading)));
}

// §2.1, §2.2, §3.1, §3.4. The names and the caps a client depends on, each read from the constant behind it.
void SpecValuesTest::theFrozenNamesInTheTableAreTheOnesTheHeadersDeclare()
{
    const QString protocolLine = lineWith(QStringLiteral("ProtocolVersion = "));
    QVERIFY2(!protocolLine.isEmpty(), "the IPC section no longer states the protocol version");
    QCOMPARE(firstNumber(protocolLine), QString::number(quantum::ipc::ProtocolVersion));

    const QString socketLine = lineWith(QStringLiteral("`SocketName = "));
    QVERIFY2(!socketLine.isEmpty(), "the IPC section no longer states the socket name");
    QVERIFY2(socketLine.contains(QLatin1String(quantum::ipc::SocketName)),
             qPrintable(QStringLiteral("the socket name in the document is not the one in IPCProtocol.h: %1")
                            .arg(socketLine)));

    // The line cap is stated in KiB and declared in bytes, so this compares the two spellings of one bound
    // rather than two numbers: a change to either fails, and so does a change to the unit the document uses.
    const QString lineCap = lineWith(QStringLiteral("MaxLineBytes = "));
    QVERIFY2(!lineCap.isEmpty(), "the IPC section no longer states the line cap");
    QVERIFY2(lineCap.contains(QStringLiteral("KiB")),
             qPrintable(QStringLiteral("the line cap's unit is no longer the one this comparison assumes: %1")
                            .arg(lineCap)));
    QCOMPARE(firstNumber(lineCap), QString::number(quantum::ipc::MaxLineBytes / 1024));

    // The namespace prefix and the one surface that exists, both read out of the section that names them
    // rather than off one line of it: the paragraph explains itself across a line break.
    const QString namespaces = region(QStringLiteral("### 2.1 Layer-shell namespaces"), QStringLiteral("### 2.2"));
    QVERIFY2(!namespaces.isEmpty(), "the document has no section for the layer-shell namespaces");
    QVERIFY2(namespaces.contains(QLatin1String(quantum::config::LayerNamespacePrefix)),
             "the namespace prefix in the document is not the one the schema enforces");
    QVERIFY2(namespaces.contains(QStringLiteral("`%1bar`").arg(QLatin1String(quantum::config::LayerNamespacePrefix))),
             "the document no longer names the one surface that exists");
    QVERIFY2(namespaces.contains(QStringLiteral("`LayerNamespacePrefix`")),
             "the document no longer names the constant the prefix is declared in");

    // Ids and the writer's cap: two bounds a client can be refused by, each stated in the document and held
    // by a constant.
    const QString maximumId = lineWith(QStringLiteral("`maximumId` = "));
    QVERIFY2(!maximumId.isEmpty(), "the niri section no longer states the id cap");
    QVERIFY2(maximumId.contains(QStringLiteral("INT64_MAX")),
             qPrintable(QStringLiteral("the id cap is no longer stated as INT64_MAX: %1").arg(maximumId)));
    QVERIFY2(quantum::niri::NiriActions::maximumId == std::numeric_limits<qint64>::max(),
             "NiriActions' id cap is no longer the largest 64-bit signed integer INT64_MAX names");

    const QString writer = region(QStringLiteral("### 3.4 Audio"), QStringLiteral("### 3.5"));
    QVERIFY2(!writer.isEmpty(), "the document has no section for the audio module");
    QVERIFY2(writer.contains(QStringLiteral("≤%1 channels").arg(quantum::audio::PropsWrite::MaxChannels)),
             qPrintable(QStringLiteral("the audio section no longer states the writer's %1-channel cap")
                            .arg(quantum::audio::PropsWrite::MaxChannels)));
    QVERIFY2(writer.contains(QStringLiteral("1 KiB")),
             "the audio section no longer describes the writer's buffer in KiB");
    QCOMPARE(quantum::audio::PropsWrite::BufferBytes, std::size_t{1024});
}

// §3.1, §3.3, §3.4. The retry schedules, the line cap and the file cap: bounds a person reads to know what a
// shell under a broken compositor will do, and the ones the document states numerically.
void SpecValuesTest::theBoundsStatedForTheConnectionsAreTheOnesTheSourcesCarry()
{
    const quantum::niri::NiriReconnect::Policy policy;
    const QString niri = region(QStringLiteral("### 3.1 niri connection"), QStringLiteral("### 3.2"));
    QVERIFY2(!niri.isEmpty(), "the document has no section for the niri connection");
    for (const int stated : {policy.firstDelayMs, policy.maximumDelayMs, policy.jitterMs, policy.attemptTimeoutMs}) {
        QVERIFY2(niri.contains(QStringLiteral("%1 ms").arg(stated)),
                 qPrintable(QStringLiteral("the niri section no longer states a %1 ms delay").arg(stated)));
    }
    QVERIFY2(niri.contains(QStringLiteral("×%1").arg(policy.factor)),
             qPrintable(QStringLiteral("the niri section no longer states the growth factor %1")
                            .arg(policy.factor)));

    QVERIFY2(niri.contains(QStringLiteral("Line cap")), "the niri section no longer states the stream's line cap");
    QCOMPARE(quantum::niri::maximumLineBytes, 4 * 1024 * 1024);
    QVERIFY2(niri.contains(QStringLiteral("4 MB")),
             "the niri section no longer states the line cap in MB, the unit this comparison assumes");

    // Two facts that live in a `.cpp` file, where nothing can link them: the declaration itself is required
    // in the source text, so a changed value fails here and a constant moved elsewhere stops being checked.
    // Named as the weaker of the two kinds, at the point where it is weaker.
    const QString procSource = source(QStringLiteral("src/system/SysMonService.cpp"));
    QVERIFY2(procSource.contains(QStringLiteral("constexpr qint64 maxFileBytes = 1024 * 1024;")),
             "the /proc file cap is no longer declared as the 1 MB the document states");
    const QString procLine = lineWith(QStringLiteral("files capped at "));
    QVERIFY2(!procLine.isEmpty(), "the system section no longer states the /proc file cap");
    QVERIFY2(procLine.contains(QStringLiteral("1 MB")),
             qPrintable(QStringLiteral("the /proc file cap is no longer stated in MB: %1").arg(procLine)));

    const QString audioSource = source(QStringLiteral("src/audio/PipeWireService.cpp"));
    QVERIFY2(audioSource.contains(QStringLiteral("constexpr int FirstRetryMs = 250;"))
                 && audioSource.contains(QStringLiteral("constexpr int MaxRetryMs = 8000;")),
             "the audio retry pair is no longer declared as the 250 ms and 8 s the document states");
    const QString audio = region(QStringLiteral("### 3.4 Audio"), QStringLiteral("### 3.5"));
    QVERIFY2(audio.contains(QStringLiteral("250 ms")) && audio.contains(QStringLiteral("8 s")),
             "the audio section no longer states the retry schedule the module's own pair describes");

    // The clamp the wheel's arithmetic ends at, which is the ceiling the config table deliberately does not
    // set: a step larger than the range is a person asking for one notch to reach the end of it.
    const QString clampLine = lineWith(QStringLiteral("wheel math clamps"));
    QVERIFY2(!clampLine.isEmpty(), "the audio section no longer states what the wheel's arithmetic clamps at");
    QCOMPARE(firstNumber(after(clampLine, QStringLiteral(".."))),
             QString::number(quantum::audio::PipeWireService::MaxPercent));
}

// §2.8 and §7. The two counts and the two measured pass costs: the figures that were wrong in this document
// when it was first derived, and the ones a person quotes when deciding what a run costs.
void SpecValuesTest::theSuiteCountsAndTheMeasuredCostsAreTheOnesTheBuildDeclares()
{
    const QString names = source(QStringLiteral("tests/public_names.cmake"));
    const QString testList = source(QStringLiteral("tests/CMakeLists.txt"));

    // Counted from the declarations rather than from a list of names repeated here, so a new test joins the
    // count the moment it is declared and the document has to follow it.
    const auto countIn = [&names](const QString& setter) -> qsizetype {
        const qsizetype start = names.indexOf(setter);
        if (start < 0)
            return -1;
        const qsizetype end = names.indexOf(QStringLiteral(")\n"), start);
        const QString body = names.sliced(start, end < 0 ? -1 : end - start);
        // The setter's own name is the first word of the body, so it comes off the count.
        return body.split(QRegularExpression(QStringLiteral("[\\s]+")), Qt::SkipEmptyParts).size() - 1;
    };
    const qsizetype declaredTests = countIn(QStringLiteral("set(QS_TEST_NAMES"));
    const qsizetype declaredVariables = countIn(QStringLiteral("set(QS_ENVIRONMENT_NAMES"));
    QVERIFY2(declaredTests > 0 && declaredVariables > 0,
             "the declarations of the project's public names could not be read");

    static const QRegularExpression statedTests(QStringLiteral("\\b(\\d+) tests: "));
    const QRegularExpressionMatch testsMatch = statedTests.match(document_);
    QVERIFY2(testsMatch.hasMatch(), "§2.8 no longer opens with a count of the tests");
    QCOMPARE(testsMatch.captured(1).toInt(), static_cast<int>(declaredTests));

    static const QRegularExpression statedVariables(QStringLiteral("\\b(\\d+) env vars: "));
    const QRegularExpressionMatch variablesMatch = statedVariables.match(document_);
    QVERIFY2(variablesMatch.hasMatch(), "§2.8 no longer states a count of the environment variables");
    QCOMPARE(variablesMatch.captured(1).toInt(), static_cast<int>(declaredVariables));

    // And the list itself: every declared name is named in the section that calls itself the list, so the
    // document's enumeration is the enumeration rather than a sample of it.
    const QString listing = region(QStringLiteral("### 2.8 Test and environment names"), QStringLiteral("\n---"));
    QVERIFY2(!listing.isEmpty(), "the document has no section for the test and environment names");
    for (const QString& name : names.split(QRegularExpression(QStringLiteral("[\\s]+")), Qt::SkipEmptyParts)) {
        if (!name.startsWith(QStringLiteral("niri-")) && !name.endsWith(QStringLiteral("-test")))
            continue;
        QVERIFY2(listing.contains(name),
                 qPrintable(QStringLiteral("§2.8 no longer names the test %1").arg(name)));
    }

    // §7's two figures are the shard cost table's, which is where a measured pass cost is written down. A
    // document quoting an older number than the table is the staleness this test exists for, and these two
    // are the entries the document quotes by name.
    for (const char* binary : {"bar-interaction-test", "sysmon-test"}) {
        const QString cost = costOf(testList, QLatin1String(binary));
        QVERIFY2(!cost.isEmpty(),
                 qPrintable(QStringLiteral("the shard cost table has no entry for %1, so the figure §7 quotes "
                                           "for it is unverified")
                                .arg(QLatin1String(binary))));
        const QString budgets = region(QStringLiteral("## 7. Budgets"), QStringLiteral("## 8."));
        QVERIFY2(!budgets.isEmpty(), "the document has no budgets section");
        const int quotedAt = budgets.indexOf(QStringLiteral("`%1` ≈").arg(QLatin1String(binary)));
        QVERIFY2(quotedAt >= 0,
                 qPrintable(QStringLiteral("§7 no longer quotes a cost for %1").arg(QLatin1String(binary))));
        const QString quote = budgets.sliced(quotedAt).split(QLatin1Char(';')).first();
        QVERIFY2(quote.contains(cost),
                 qPrintable(QStringLiteral("§7 says %1 while the cost table measures %2 ms")
                                .arg(quote.trimmed(), cost)));
    }

    // The other figure §7 quotes is the shard wall clock, and the document is explicit that it is a
    // measurement on one machine rather than a guarantee, so it is stated rather than compared.
}

// §2.6. A singleton's row is the list of names a QML file may write, so it is compared with what the class
// actually exposes: the meta-object, because that is what the engine resolves a binding against — a property
// renamed in the header and a moc-visible declaration that still carries the old name are two spellings of the
// same file, and only this sees the difference. The bar's window is the exception and its own block below
// says so.
void SpecValuesTest::everySingletonRowNamesExactlyWhatTheCodeExposes()
{
    struct SingletonCheck {
        const char* row;
        const QMetaObject* meta;
    };
    // Every one of these is checked in both directions: every name the row states is one the class declares,
    // and every property and invokable the class declares is named by the row. The second direction is the
    // one that catches an addition — a property that arrived in the code and not in the document is a name
    // nobody is told about — which is why the rows are exhaustive rather than illustrative.
    const std::array<SingletonCheck, 8> singletons{{
        {"| `NiriService` |", &quantum::niri::NiriService::staticMetaObject},
        {"| `NiriActions` |", &quantum::niri::NiriActions::staticMetaObject},
        {"| `SysMonService` |", &quantum::system::SysMonService::staticMetaObject},
        {"| `PipeWireService` |", &quantum::audio::PipeWireService::staticMetaObject},
        {"| `NetworkService` |", &quantum::dbus::NetworkService::staticMetaObject},
        {"| `BatteryService` |", &quantum::dbus::BatteryService::staticMetaObject},
        {"| `NotificationService` |", &quantum::dbus::NotificationService::staticMetaObject},
        {"| `MediaService` |", &quantum::dbus::MediaService::staticMetaObject},
    }};

    for (const SingletonCheck& check : singletons) {
        const DocumentedRow documented = documentedNames(QLatin1String(check.row));
        QVERIFY2(documented.found, qPrintable(QStringLiteral("the singleton table has no row for %1")
                                                  .arg(QLatin1String(check.row))));
        const QStringList& names = documented.names;
        // Every documented name is a real name: a property, an invokable or a signal the class declares. Signals
        // are counted here because a row names one where it has one (`NiriActions`'s `actionFailed`), and the
        // set they are matched against is everything QML can resolve.
        const QStringList members = ownMembers(*check.meta);
        const QStringList invented = onlyIn(names, members);
        QVERIFY2(invented.isEmpty(),
                 qPrintable(QStringLiteral("%1 documents `%2`, which the class does not declare; the row and "
                                           "the code have come apart")
                                .arg(QLatin1String(check.row), invented.join(QStringLiteral("`, `")))));
        // And the other direction, which is the one that catches an addition: a property or an invokable that
        // arrived in the code and not in the document is a name a widget is never told about. Invokables and
        // properties are the names a widget writes, so they are the ones the row has to list; signals are the
        // notify half of a property and are listed only where the row says it is listing them, which is why
        // `members` above is the wider set and this one is not.
        QStringList declared = ownProperties(*check.meta);
        declared += ownInvokables(*check.meta);
        const QStringList unlisted = onlyIn(declared, names);
        QVERIFY2(unlisted.isEmpty(),
                 qPrintable(QStringLiteral("%1 does not name `%2`, which the class declares")
                                .arg(QLatin1String(check.row), unlisted.join(QStringLiteral("`, `")))));
    }

    // `LayerShellWindow` is the one row whose class this binary does not link, so its names are read from its
    // header's own declarations rather than from a meta-object. The reason is a measurement and it is written
    // beside the target in tests/unit/CMakeLists.txt: the class derives from `QQuickWindow`, and linking it
    // here makes the sanitizer job report the font stack's process-lifetime caches as leaks of this binary.
    // What that gives up is stated rather than hidden: a rename or an addition in the `Q_PROPERTY` and
    // `Q_INVOKABLE` declarations is caught, and a disagreement between a declaration and the meta-object moc
    // built from it is not — a thing moc does not permit, but this half cannot prove it.
    {
        const DocumentedRow documented = documentedNames(QStringLiteral("| `LayerShellWindow` |"));
        QVERIFY2(documented.found, "the singleton table has no row for LayerShellWindow");
        const QString header = source(QStringLiteral("src/wayland/LayerShellWindow.h"));
        QVERIFY2(!header.isEmpty(), "the bar window's header was not read");
        QStringList declared = propertyNamesIn(header);
        declared += invokableNamesIn(header);
        declared.sort();
        // An extraction that found nothing would make both comparisons below vacuous, so its size is asserted
        // before it is trusted. A single declaration the patterns miss does not hide here — it comes out as
        // `invented` below — but a pattern that stopped matching this header's spelling altogether would
        // otherwise read as a header with nothing to check.
        QVERIFY2(declared.size() >= documented.names.size(),
                 qPrintable(QStringLiteral("only %1 declarations were read out of LayerShellWindow.h, while "
                                           "the document's row names %2")
                                .arg(declared.size())
                                .arg(documented.names.size())));
        const QStringList invented = onlyIn(documented.names, declared);
        QVERIFY2(invented.isEmpty(),
                 qPrintable(QStringLiteral("the LayerShellWindow row documents `%1`, which the header does not "
                                           "declare")
                                .arg(invented.join(QStringLiteral("`, `")))));
        const QStringList unlisted = onlyIn(declared, documented.names);
        QVERIFY2(unlisted.isEmpty(),
                 qPrintable(QStringLiteral("LayerShellWindow.h declares `%1`, which the bar window's row does "
                                           "not name")
                                .arg(unlisted.join(QStringLiteral("`, `")))));
    }

    // The NiriService row makes a claim about its own shape in words — `each with NOTIFY` — and it is a claim
    // with a consequence: a binding cannot follow a value whose notify signal is missing, and the document
    // would be describing a service QML cannot bind to.
    const QMetaObject& service = quantum::niri::NiriService::staticMetaObject;
    for (const QString& name : ownProperties(service)) {
        const int index = service.indexOfProperty(name.toUtf8().constData());
        QVERIFY2(index >= 0 && service.property(index).hasNotifySignal(),
                 qPrintable(QStringLiteral("NiriService's `%1` has no notify signal, and the row says every "
                                           "one of its properties notifies")
                                .arg(name)));
    }
}

// §2.6's row for `Config`, which is not a list of properties but a description of a tree: two named leaves, two
// wildcards with counts, and the claim that the three nested objects are CONSTANT while every leaf notifies.
void SpecValuesTest::theConfigRowDescribesTheObjectTreeItNames()
{
    const Row row_ = row(QStringLiteral("| `Config` |"));
    QVERIFY2(row_.found, "the singleton table has no row for Config");
    const QString cell = row_.cells.value(1);
    const QStringList tokens = backtickedTokens(cell);
    QVERIFY2(!tokens.isEmpty(), "the Config row names nothing");

    // The named leaves resolve through the tree the row describes: `bar.height` is a property of the object
    // `bar`, and the object is reached from the type the engine looks the singleton up by.
    const QMetaObject* bar = &quantum::config::ConfigBar::staticMetaObject;
    const auto ownIndexOf = [](const QMetaObject& meta, const QString& name) {
        for (int index = meta.propertyOffset(); index < meta.propertyCount(); ++index) {
            if (name == QLatin1String(meta.property(index).name()))
                return index;
        }
        return -1;
    };
    const auto resolves = [&ownIndexOf](const QMetaObject& meta, const QString& path) {
        const QStringList parts = path.split(QLatin1Char('.'));
        if (parts.size() != 2)
            return false;
        return ownIndexOf(meta, parts.at(1)) >= 0;
    };
    QVERIFY2(resolves(*bar, QStringLiteral("bar.height")), "the Config row's `bar.height` is not a property of bar");
    QVERIFY2(resolves(*bar, QStringLiteral("bar.layerNamespace")),
             "the Config row's `bar.layerNamespace` is not a property of bar");

    // The row's `bar.<name>` and `bar.<name>.*` spellings against the object's own properties, both ways: every
    // property `bar` declares appears in the row, and every `bar.<name>` the row names is one it declares. This
    // is the direction that catches a table: `[bar.network]` landing in the configuration with a `ConfigNetwork`
    // object behind it would otherwise be a set of keys nobody is told about.
    QStringList documentedTables;
    for (const QString& token : tokens) {
        if (!token.startsWith(QStringLiteral("bar.")))
            continue;
        QString name = token.sliced(4);
        if (name.endsWith(QStringLiteral(".*")))
            name.chop(2);
        documentedTables.append(name);
    }
    documentedTables.sort();
    const QStringList declaredTables = ownProperties(*bar);
    const QStringList unlistedTables = onlyIn(declaredTables, documentedTables);
    QVERIFY2(unlistedTables.isEmpty(),
             qPrintable(QStringLiteral("the Config row does not name `bar.%1`, which the object bar declares")
                            .arg(unlistedTables.join(QStringLiteral("`, `bar.")))));
    const QStringList inventedTables = onlyIn(documentedTables, declaredTables);
    QVERIFY2(inventedTables.isEmpty(),
             qPrintable(QStringLiteral("the Config row names `bar.%1`, which the object bar does not declare")
                            .arg(inventedTables.join(QStringLiteral("`, `bar.")))));

    // The wildcards, and the count each one states in parentheses: `bar.system.*` (4) is a claim that the table
    // has four leaves, and a fifth key added to it makes the row wrong rather than merely short.
    struct Wildcard {
        const char* token;
        const QMetaObject* meta;
    };
    const std::array<Wildcard, 2> wildcards{{
        {"bar.system.*", &quantum::config::ConfigSystem::staticMetaObject},
        {"bar.audio.*", &quantum::config::ConfigAudio::staticMetaObject},
    }};
    for (const Wildcard& wildcard : wildcards) {
        const qsizetype at = cell.indexOf(QLatin1String(wildcard.token));
        QVERIFY2(at >= 0, qPrintable(QStringLiteral("the Config row no longer names `%1`")
                                         .arg(QLatin1String(wildcard.token))));
        const QString statedCount = firstNumber(cell.sliced(at + qstrlen(wildcard.token)));
        const QStringList leaves = ownProperties(*wildcard.meta);
        QVERIFY2(!statedCount.isEmpty(),
                 qPrintable(QStringLiteral("the Config row states no count for `%1`")
                                .arg(QLatin1String(wildcard.token))));
        QCOMPARE(statedCount.toInt(), leaves.size());
        // And each of those leaves is a leaf: a property a widget binds to has to notify, which is the row's
        // other claim about the tree.
        for (const QString& leaf : leaves) {
            const int index = ownIndexOf(*wildcard.meta, leaf);
            QVERIFY2(index >= 0 && wildcard.meta->property(index).hasNotifySignal(),
                     qPrintable(QStringLiteral("%1's `%2` has no notify signal")
                                    .arg(QLatin1String(wildcard.token), leaf)));
        }
    }

    // `bar`/`system`/`audio` are CONSTANT in the document, and that is the difference between a nested object
    // and a leaf: opening a new object per notification would be a second value in the tree, so the tree is
    // built once and only its leaves move.
    const auto isConstantObject = [&ownIndexOf](const QMetaObject& meta, const QString& name) {
        const int index = ownIndexOf(meta, name);
        if (index < 0)
            return false;
        const QMetaProperty property = meta.property(index);
        return property.isConstant() && !property.hasNotifySignal();
    };
    QVERIFY2(isConstantObject(quantum::config::Config::staticMetaObject, QStringLiteral("bar")),
             "`bar` is documented as a CONSTANT object and is not one");
    QVERIFY2(isConstantObject(*bar, QStringLiteral("system")),
             "`system` is documented as a CONSTANT object and is not one");
    QVERIFY2(isConstantObject(*bar, QStringLiteral("audio")),
             "`audio` is documented as a CONSTANT object and is not one");

}

QTEST_MAIN(SpecValuesTest)

#include "spec_values_test.moc"
