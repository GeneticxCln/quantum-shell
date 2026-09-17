// The shell's logging: what a record looks like, and that the category names are the ones a person filters
// on.
//
// The format is the part worth testing, because it is the part that fails quietly. A pattern with no
// timestamp still produces records; they simply cannot be placed in time, and nothing about them looks
// wrong. The first slot below therefore asserts the four parts of a record rather than that a record was
// produced — and it goes through the same path the shell uses, so it fails if `install()` stops applying
// the pattern.
//
// What is deliberately not here is the destination. Qt picks it — a terminal when there is one, the systemd
// journal when there is not — and a shell started by niri has no terminal, so its records are read with
// `journalctl --user`. There is nothing in this process to assert about that; the live run in
// `niri-live-layershell-test` is where a record's arrival is checked, by asking the journal for it.
#include "app/Logging.h"

#include <QDate>
#include <QLoggingCategory>
#include <QStringList>
#include <QTest>

#include <array>

namespace {

// Every category the shell declares, so a category added without a name in the shell's namespace — the
// filter a person writes is `quantum.shell.*` — is caught here rather than in a bug report where the
// filter silently matched nothing.
using CategoryAccessor = const QLoggingCategory& (*)();
// Eight, and two of them arrived over time without being added here: the audio one with the volume readout and
// the network one with this module. That is exactly the hole this list is for — a category absent from it is a
// category whose name nothing checks, and the shell would go on printing it while `quantum.shell.*` filters
// everything else — so the list is read as the complete set and the slot below fails if a name here is a
// duplicate or outside the shell's namespace. What it cannot do is notice a category that *exists* and is not
// listed; the guard for that is that a category is added in the same change as its reader, and the diff that
// adds one is where this line gets its next entry.
constexpr std::array<CategoryAccessor, 8> categories{&quantum::app::shellLog, &quantum::app::niriLog,
                                                    &quantum::app::configLog, &quantum::app::ipcLog,
                                                    &quantum::app::waylandLog, &quantum::app::systemLog,
                                                    &quantum::app::audioLog, &quantum::app::networkLog};

}  // namespace

class AppLoggingTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup();

    void theRecordFormatCarriesTheTimeTheLevelTheCategoryAndTheMessage();
    void theEnvironmentKeepsItsOwnPattern();
    void everyCategoryIsInsideTheShellsNamespaceAndNamedOnce();
};

void AppLoggingTest::cleanup() {
    // The pattern is process state and one slot sets the environment to prove the user's choice wins, so it
    // is put back: a slot that left it set would decide the answer for whichever slot ran next, which is
    // exactly the kind of order dependence the order checks exist to catch.
    qunsetenv("QT_MESSAGE_PATTERN");
}

void AppLoggingTest::theRecordFormatCarriesTheTimeTheLevelTheCategoryAndTheMessage() {
    qunsetenv("QT_MESSAGE_PATTERN");
    quantum::app::Logging::install();

    const QMessageLogContext context(__FILE__, __LINE__, Q_FUNC_INFO,
                                     quantum::app::shellLog().categoryName());
    const QString record =
        quantum::app::Logging::format(QtWarningMsg, context, QStringLiteral("the bar has no namespace"));

    // The message, unchanged: a format that ate part of it would be a logger that loses the thing it logged.
    QVERIFY2(record.contains(QLatin1StringView("the bar has no namespace")), qPrintable(record));
    // The category, so `QT_LOGGING_RULES` and `journalctl` can both be used to find it.
    QVERIFY2(record.contains(QLatin1StringView("quantum.shell")), qPrintable(record));
    // The level, spelled as Qt spells it in a pattern.
    QVERIFY2(record.contains(QLatin1StringView("warning")), qPrintable(record));
    // And the time, which is the part whose absence is invisible: this is the assertion that fails if
    // `install()` stops applying the pattern.
    QVERIFY2(record.contains(QString::number(QDate::currentDate().year())), qPrintable(record));
}

void AppLoggingTest::theEnvironmentKeepsItsOwnPattern() {
    qputenv("QT_MESSAGE_PATTERN", QByteArrayLiteral("a pattern the user chose: %{message}"));
    QCOMPARE(quantum::app::Logging::pattern(), QStringLiteral("a pattern the user chose: %{message}"));

    // Unset, the shell's own pattern is what is applied: `QT_MESSAGE_PATTERN` is the user's way to make this
    // decision, and overriding it would be the shell taking that choice away.
    qunsetenv("QT_MESSAGE_PATTERN");
    QCOMPARE(quantum::app::Logging::pattern(), QString::fromLatin1(quantum::app::Logging::DefaultPattern));
}

void AppLoggingTest::everyCategoryIsInsideTheShellsNamespaceAndNamedOnce() {
    QStringList names;
    for (CategoryAccessor accessor : categories)
        names.append(QString::fromLatin1(accessor().categoryName()));

    QCOMPARE(names.size(), int(categories.size()));
    QVERIFY2(names.contains(QStringLiteral("quantum.shell")),
             "the shell's root category is missing, so `quantum.shell.*` filters nothing at its base");

    const QStringList sorted = [&names] {
        QStringList copy = names;
        copy.sort();
        return copy;
    }();
    for (qsizetype index = 1; index < sorted.size(); ++index) {
        QVERIFY2(sorted.at(index) != sorted.at(index - 1),
                 qPrintable(QStringLiteral("two categories share the name %1").arg(sorted.at(index))));
    }
    for (const QString& name : sorted) {
        // Every category is under the shell's own prefix, which is what makes one filter rule enough.
        QVERIFY2(name.startsWith(QStringLiteral("quantum.shell")), qPrintable(name));
    }
    QCOMPARE(sorted.first(), QStringLiteral("quantum.shell"));  // the root is the only name without a dot
    QCOMPARE(sorted.count(QStringLiteral("quantum.shell")), 1);
}

QTEST_GUILESS_MAIN(AppLoggingTest)
#include "app_logging_test.moc"
