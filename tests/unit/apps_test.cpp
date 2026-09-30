// The launcher's reading of the desktop: what a `.desktop` file says, what an `Exec` line means, how a query
// ranks a name, what a scan of real directories offers, and that starting an entry starts a real process.
//
// The first three are functions of strings and are driven with the shapes the Desktop Entry Specification
// defines *and* the ones a healthy desktop never contains. The scan and the launch are driven against a
// directory this test writes: the entries in it are the answer for files the test made, so nothing here can be
// moved by what is installed on the machine that runs it.
#include "apps/DesktopEntry.h"
#include "apps/LaunchHistory.h"
#include "apps/LauncherService.h"
#include "config/ConfigSchema.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using quantum::apps::DesktopEntry;
using quantum::apps::LaunchHistory;
using quantum::apps::LaunchRecord;
using quantum::apps::LauncherService;
using quantum::apps::isOffered;
using quantum::apps::matchScore;
using quantum::apps::parseDesktopEntry;
using quantum::apps::splitExec;

namespace {

QString entryText(const QString& name, const QString& exec, const QString& extra = QString())
{
    return QStringLiteral("[Desktop Entry]\nType=Application\nName=%1\nExec=%2\n%3").arg(name, exec, extra);
}

bool write(const QString& path, const QString& text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(text.toUtf8());
    return true;
}

DesktopEntry make(const QString& id, const QString& name, const QString& program,
                  const QString& comment = QString())
{
    DesktopEntry e;
    e.id = id;
    e.name = name;
    e.arguments = {program};
    e.comment = comment;
    return e;
}

// The launcher's list length is the file's `[launcher] max_results` and the service's own bound, and the two
// spellings are compared here, at compile time, so the file and the service cannot disagree about what a list
// length may be — the same rule the sampling cadence and the volume steps are held to.
static_assert(quantum::config::LauncherConfig{}.maxResults == LauncherService::DefaultMaxResults,
              "the schema's default and the service's default for max_results differ");
static_assert(quantum::config::MinLauncherMaxResults == LauncherService::MinMaxResults,
              "the schema's floor and the service's floor for max_results differ");
static_assert(quantum::config::MaxLauncherMaxResults == LauncherService::MaxMaxResults,
              "the schema's ceiling and the service's ceiling for max_results differ");

}  // namespace

class AppsTest : public QObject {
    Q_OBJECT

private slots:
    void anApplicationEntryIsParsed();
    void onlyTheDesktopEntryGroupIsRead();
    void whatIsNotAnApplicationIsRefused();
    void aLocalisedNameIsPreferredInTheSpecsOrder();
    void valueEscapesAndListsFollowTheSpecification();
    void execIsSplitAndItsFieldCodesResolved();
    void anUnterminatedQuoteIsNotAnExec();
    void whatIsOfferedDependsOnTheDesktopAndTheEntry();
    void aQueryRanksPrefixThenWordThenSubstringThenSubsequence();
    void rankingIsBestFirstThenByNameAndBounded();
    void aScanResolvesShadowingAndDropsWhatIsNotOffered();
    void theServiceScansOnOpenAndFiltersByQuery();
    void theSelectionWrapsAndResetsWithTheResults();
    void aLaunchStartsARealProcessWithItsArgumentsAndClosesThePanel();
    void aProgramThatCannotStartLeavesThePanelOpen();
    void maxResultsIsBoundedAndCapsTheList();
    void frecencyWeighsTheCountByHowRecentTheLastLaunchWas();
    void theHistoryFileRoundTripsAndRefusesWhatIsNotItsFormat();
    void frecencyOrdersTiesAndNeverOutranksABetterMatch();
    void aLaunchIsRememberedAndOrdersTheNextOpen();
};

void AppsTest::anApplicationEntryIsParsed()
{
    const auto entry = parseDesktopEntry(
        entryText(QStringLiteral("Files"), QStringLiteral("/usr/bin/files --new-window %U"),
                  QStringLiteral("Comment=Browse files\nTryExec=files\n")),
        QStringLiteral("C"));
    QVERIFY(entry.has_value());
    QCOMPARE(entry->name, QStringLiteral("Files"));
    QCOMPARE(entry->comment, QStringLiteral("Browse files"));
    QCOMPARE(entry->arguments, (QStringList{QStringLiteral("/usr/bin/files"), QStringLiteral("--new-window")}));
    QCOMPARE(entry->tryExec, QStringLiteral("files"));
    QVERIFY(!entry->hidden);
    QVERIFY(!entry->terminal);
}

void AppsTest::onlyTheDesktopEntryGroupIsRead()
{
    // An action group has its own Name and Exec; reading them as the entry's would start the wrong program.
    const QString text = QStringLiteral(
        "[Desktop Entry]\nType=Application\nName=Browser\nExec=browser\n"
        "[Desktop Action private]\nName=Private window\nExec=browser --private\n");
    const auto entry = parseDesktopEntry(text, QString());
    QVERIFY(entry.has_value());
    QCOMPARE(entry->name, QStringLiteral("Browser"));
    QCOMPARE(entry->arguments, (QStringList{QStringLiteral("browser")}));

    // The first occurrence of a key wins, whatever order a reader happens to take.
    const auto twice = parseDesktopEntry(
        QStringLiteral("[Desktop Entry]\nType=Application\nName=One\nName=Two\nExec=a\nExec=b\n"), QString());
    QVERIFY(twice.has_value());
    QCOMPARE(twice->name, QStringLiteral("One"));
    QCOMPARE(twice->arguments, (QStringList{QStringLiteral("a")}));
}

void AppsTest::whatIsNotAnApplicationIsRefused()
{
    QVERIFY(!parseDesktopEntry(QString(), QString()).has_value());
    QVERIFY(!parseDesktopEntry(QStringLiteral("Type=Application\nName=X\nExec=x\n"), QString()).has_value());
    QVERIFY(!parseDesktopEntry(QStringLiteral("[Desktop Entry]\nType=Link\nName=X\nURL=http://x\n"), QString())
                 .has_value());
    QVERIFY(!parseDesktopEntry(QStringLiteral("[Desktop Entry]\nType=Directory\nName=X\n"), QString()).has_value());
    QVERIFY(!parseDesktopEntry(QStringLiteral("[Desktop Entry]\nType=Application\nExec=x\n"), QString()).has_value());
    QVERIFY(!parseDesktopEntry(QStringLiteral("[Desktop Entry]\nType=Application\nName=X\n"), QString()).has_value());
    // An Exec that is only field codes has no program.
    QVERIFY(!parseDesktopEntry(entryText(QStringLiteral("X"), QStringLiteral("%U")), QString()).has_value());
    // Hidden and NoDisplay are entries marked hidden, so a scan can let a user's copy shadow the system's.
    const auto hidden = parseDesktopEntry(
        entryText(QStringLiteral("X"), QStringLiteral("x"), QStringLiteral("NoDisplay=true\n")), QString());
    QVERIFY(hidden.has_value());
    QVERIFY(hidden->hidden);
}

void AppsTest::aLocalisedNameIsPreferredInTheSpecsOrder()
{
    const QString text = QStringLiteral(
        "[Desktop Entry]\nType=Application\nExec=x\nName=Plain\nName[de]=Deutsch\nName[de_AT]=Oesterreich\n"
        "Name[sr@latin]=Srpski\nComment=c\nComment[de]=k\n");
    QCOMPARE(parseDesktopEntry(text, QStringLiteral("de_AT.UTF-8"))->name, QStringLiteral("Oesterreich"));
    QCOMPARE(parseDesktopEntry(text, QStringLiteral("de_DE.UTF-8"))->name, QStringLiteral("Deutsch"));
    QCOMPARE(parseDesktopEntry(text, QStringLiteral("sr@latin"))->name, QStringLiteral("Srpski"));
    QCOMPARE(parseDesktopEntry(text, QStringLiteral("fr_FR"))->name, QStringLiteral("Plain"));
    QCOMPARE(parseDesktopEntry(text, QStringLiteral("C"))->name, QStringLiteral("Plain"));
    QCOMPARE(parseDesktopEntry(text, QString())->name, QStringLiteral("Plain"));
    QCOMPARE(parseDesktopEntry(text, QStringLiteral("de_DE"))->comment, QStringLiteral("k"));
}

void AppsTest::valueEscapesAndListsFollowTheSpecification()
{
    const auto entry = parseDesktopEntry(
        QStringLiteral("[Desktop Entry]\nType=Application\nName=A\\sB\\\\C\nExec=x\n"
                       "OnlyShowIn=niri;GNOME;\nNotShowIn=KDE\\;X;Sway\n"),
        QString());
    QVERIFY(entry.has_value());
    QCOMPARE(entry->name, QStringLiteral("A B\\C"));
    QCOMPARE(entry->onlyShowIn, (QStringList{QStringLiteral("niri"), QStringLiteral("GNOME")}));
    QCOMPARE(entry->notShowIn, (QStringList{QStringLiteral("KDE;X"), QStringLiteral("Sway")}));
}

void AppsTest::execIsSplitAndItsFieldCodesResolved()
{
    const auto split = [](const QString& exec) { return splitExec(exec, QStringLiteral("App Name")); };
    QCOMPARE(*split(QStringLiteral("prog a  b\tc")),
             (QStringList{QStringLiteral("prog"), QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}));
    // Quotes keep whitespace and hold the four escapes.
    QCOMPARE(*split(QStringLiteral("\"/opt/my app/run\" \"say \\\"hi\\\" \\$HOME\"")),
             (QStringList{QStringLiteral("/opt/my app/run"), QStringLiteral("say \"hi\" $HOME")}));
    // Field codes: removed, `%%` a percent sign, `%c` the name; an argument that was only a code disappears,
    // an explicitly empty one does not.
    QCOMPARE(*split(QStringLiteral("prog %f %F %u %U %d %D %n %N %v %m %i %k")),
             (QStringList{QStringLiteral("prog")}));
    QCOMPARE(*split(QStringLiteral("prog --name=%c 100%% %U")),
             (QStringList{QStringLiteral("prog"), QStringLiteral("--name=App Name"), QStringLiteral("100%")}));
    QCOMPARE(*split(QStringLiteral("prog \"\" x")),
             (QStringList{QStringLiteral("prog"), QString(), QStringLiteral("x")}));
    // No shell: metacharacters are ordinary characters of an argument.
    QCOMPARE(*split(QStringLiteral("prog ; rm -rf /")),
             (QStringList{QStringLiteral("prog"), QStringLiteral(";"), QStringLiteral("rm"), QStringLiteral("-rf"),
                          QStringLiteral("/")}));
    QVERIFY(split(QString())->isEmpty());
}

void AppsTest::anUnterminatedQuoteIsNotAnExec()
{
    QVERIFY(!splitExec(QStringLiteral("prog \"unclosed"), QStringLiteral("n")).has_value());
    QVERIFY(!parseDesktopEntry(entryText(QStringLiteral("X"), QStringLiteral("prog \"unclosed")), QString())
                 .has_value());
}

void AppsTest::whatIsOfferedDependsOnTheDesktopAndTheEntry()
{
    DesktopEntry entry;
    entry.name = QStringLiteral("X");
    entry.arguments = {QStringLiteral("x")};
    const QStringList niri{QStringLiteral("niri")};
    QVERIFY(isOffered(entry, niri));
    QVERIFY(isOffered(entry, {}));

    DesktopEntry hidden = entry;
    hidden.hidden = true;
    QVERIFY(!isOffered(hidden, niri));

    // A terminal application cannot be started without choosing a terminal, so it is not offered.
    DesktopEntry terminal = entry;
    terminal.terminal = true;
    QVERIFY(!isOffered(terminal, niri));

    DesktopEntry only = entry;
    only.onlyShowIn = {QStringLiteral("GNOME")};
    QVERIFY(!isOffered(only, niri));
    QVERIFY(isOffered(only, {QStringLiteral("GNOME"), QStringLiteral("niri")}));
    QVERIFY(!isOffered(only, {}));

    DesktopEntry excluded = entry;
    excluded.notShowIn = {QStringLiteral("niri")};
    QVERIFY(!isOffered(excluded, niri));
    QVERIFY(isOffered(excluded, {QStringLiteral("GNOME")}));
}

void AppsTest::aQueryRanksPrefixThenWordThenSubstringThenSubsequence()
{
    QCOMPARE(matchScore(QString(), QStringLiteral("anything")), 0);
    QCOMPARE(matchScore(QStringLiteral("fire"), QStringLiteral("Firefox")), 300);
    QCOMPARE(matchScore(QStringLiteral("FIRE"), QStringLiteral("firefox")), 300);
    QCOMPARE(matchScore(QStringLiteral("fox"), QStringLiteral("Fire Fox")), 200);
    QCOMPARE(matchScore(QStringLiteral("fox"), QStringLiteral("fire-fox")), 200);
    QCOMPARE(matchScore(QStringLiteral("ref"), QStringLiteral("Firefox")), 100);
    const int loose = matchScore(QStringLiteral("frx"), QStringLiteral("Firefox"));
    QVERIFY2(loose >= 10 && loose <= 60, qPrintable(QString::number(loose)));
    QCOMPARE(matchScore(QStringLiteral("zzz"), QStringLiteral("Firefox")), -1);
    // Order matters for a subsequence: the letters have to occur in the order typed.
    QCOMPARE(matchScore(QStringLiteral("xrf"), QStringLiteral("Firefox")), -1);
    // A tighter run outranks a looser one.
    QVERIFY(matchScore(QStringLiteral("abc"), QStringLiteral("xxaxbxc"))
            > matchScore(QStringLiteral("abc"), QStringLiteral("xaxxxxbxxxxc")));
    // Unicode simple case folding, not ASCII lowering: an accented capital finds its lower-case form. (Full
    // folding, `\u00df` to `ss`, is not what Qt's `toCaseFolded` does, and the test does not claim it.)
    QCOMPARE(matchScore(QStringLiteral("\u00c9cole"), QStringLiteral("\u00e9cole normale")), 300);
}

void AppsTest::rankingIsBestFirstThenByNameAndBounded()
{
    const QList<DesktopEntry> entries{
        make(QStringLiteral("c"), QStringLiteral("Zed Browser"), QStringLiteral("/usr/bin/zed")),
        make(QStringLiteral("a"), QStringLiteral("Firefox"), QStringLiteral("/usr/bin/firefox")),
        make(QStringLiteral("b"), QStringLiteral("Web Browser"), QStringLiteral("/usr/bin/firefox-esr")),
        make(QStringLiteral("d"), QStringLiteral("Calculator"), QStringLiteral("/usr/bin/calc"),
             QStringLiteral("Does arithmetic"))};

    // An empty query is everything, by name.
    const auto all = LauncherService::rank(entries, QString(), 10);
    QCOMPARE(all.size(), 4);
    QCOMPARE(all.at(0).name, QStringLiteral("Calculator"));
    QCOMPARE(all.at(3).name, QStringLiteral("Zed Browser"));

    // `fire`: the name Firefox is a prefix (300); "Web Browser" matches only through its program's name,
    // which is a weaker claim (300 - 50), so it ranks below and still appears.
    const auto fire = LauncherService::rank(entries, QStringLiteral("fire"), 10);
    QCOMPARE(fire.size(), 2);
    QCOMPARE(fire.at(0).name, QStringLiteral("Firefox"));
    QCOMPARE(fire.at(1).name, QStringLiteral("Web Browser"));

    // A word match in the comment finds an entry whose name does not match, below every name match.
    const auto arithmetic = LauncherService::rank(entries, QStringLiteral("arith"), 10);
    QCOMPARE(arithmetic.size(), 1);
    QCOMPARE(arithmetic.at(0).name, QStringLiteral("Calculator"));

    QCOMPARE(LauncherService::rank(entries, QStringLiteral("nothing-matches"), 10).size(), 0);
    QCOMPARE(LauncherService::rank(entries, QString(), 2).size(), 2);
}

void AppsTest::aScanResolvesShadowingAndDropsWhatIsNotOffered()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString user = root.filePath(QStringLiteral("user"));
    const QString system = root.filePath(QStringLiteral("system"));

    QVERIFY(write(system + QStringLiteral("/applications/editor.desktop"),
                  entryText(QStringLiteral("System Editor"), QStringLiteral("sysedit"))));
    QVERIFY(write(user + QStringLiteral("/applications/editor.desktop"),
                  entryText(QStringLiteral("User Editor"), QStringLiteral("useredit"))));
    // A user's Hidden copy deletes the system's entry.
    QVERIFY(write(system + QStringLiteral("/applications/removed.desktop"),
                  entryText(QStringLiteral("Removed"), QStringLiteral("removed"))));
    QVERIFY(write(user + QStringLiteral("/applications/removed.desktop"),
                  entryText(QStringLiteral("Removed"), QStringLiteral("removed"), QStringLiteral("Hidden=true\n"))));
    // A subdirectory's separator becomes a dash in the ID.
    QVERIFY(write(system + QStringLiteral("/applications/vendor/tool.desktop"),
                  entryText(QStringLiteral("Vendor Tool"), QStringLiteral("tool"))));
    // Dropped: a terminal application, a NoDisplay entry, a TryExec that does not exist, another desktop's,
    // a file that is not an application and one that is not a desktop entry at all.
    QVERIFY(write(system + QStringLiteral("/applications/term.desktop"),
                  entryText(QStringLiteral("Term"), QStringLiteral("term"), QStringLiteral("Terminal=true\n"))));
    QVERIFY(write(system + QStringLiteral("/applications/nodisplay.desktop"),
                  entryText(QStringLiteral("NoDisplay"), QStringLiteral("nd"), QStringLiteral("NoDisplay=true\n"))));
    QVERIFY(write(system + QStringLiteral("/applications/missing.desktop"),
                  entryText(QStringLiteral("Missing"), QStringLiteral("missing"),
                            QStringLiteral("TryExec=/nonexistent/qs-apps-test-binary\n"))));
    QVERIFY(write(system + QStringLiteral("/applications/gnome.desktop"),
                  entryText(QStringLiteral("Gnome Only"), QStringLiteral("g"),
                            QStringLiteral("OnlyShowIn=GNOME;\n"))));
    QVERIFY(write(system + QStringLiteral("/applications/link.desktop"),
                  QStringLiteral("[Desktop Entry]\nType=Link\nName=L\nURL=x\n")));
    QVERIFY(write(system + QStringLiteral("/applications/readme.txt"), QStringLiteral("not an entry")));

    int refused = 0;
    const QList<DesktopEntry> found =
        LauncherService::scan({user, system}, {QStringLiteral("niri")}, QString(), &refused);
    QStringList ids;
    for (const DesktopEntry& e : found)
        ids.append(e.id);
    ids.sort();
    QCOMPARE(ids, (QStringList{QStringLiteral("editor.desktop"), QStringLiteral("vendor-tool.desktop")}));
    for (const DesktopEntry& e : found) {
        if (e.id == QLatin1String("editor.desktop"))
            QCOMPARE(e.name, QStringLiteral("User Editor"));
    }
    // Refused are the files that are not applications this shell can offer: the link.
    QCOMPARE(refused, 1);

    // A directory that does not exist is not an error.
    QCOMPARE(LauncherService::scan({root.filePath(QStringLiteral("none"))}, {}, QString()).size(), 0);
}

void AppsTest::theServiceScansOnOpenAndFiltersByQuery()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(write(root.filePath(QStringLiteral("applications/a.desktop")),
                  entryText(QStringLiteral("Alpha Editor"), QStringLiteral("alpha"))));
    QVERIFY(write(root.filePath(QStringLiteral("applications/b.desktop")),
                  entryText(QStringLiteral("Beta Player"), QStringLiteral("beta"))));

    LauncherService service({root.path()}, {QStringLiteral("niri")}, QString());
    QCOMPARE(service.applicationCount(), 0);
    QVERIFY(!service.isOpen());
    QCOMPARE(service.selectedIndex(), -1);

    QSignalSpy opened(&service, &LauncherService::openChanged);
    QVERIFY(service.toggle());
    QCOMPARE(opened.count(), 1);
    // The scan is on a worker thread: waited for as the event it is.
    QTRY_COMPARE_WITH_TIMEOUT(service.applicationCount(), 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!service.scanning(), 5000);
    QCOMPARE(service.results().size(), 2);
    QCOMPARE(service.selectedIndex(), 0);
    QCOMPARE(service.results().at(0).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("Alpha Editor"));

    service.setQuery(QStringLiteral("play"));
    QCOMPARE(service.results().size(), 1);
    QCOMPARE(service.results().at(0).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("b.desktop"));
    service.setQuery(QStringLiteral("zzz"));
    QCOMPARE(service.results().size(), 0);
    QCOMPARE(service.selectedIndex(), -1);

    // Opening again rescans and starts from an empty query; a file written since is found.
    QVERIFY(!service.toggle());
    QVERIFY(write(root.filePath(QStringLiteral("applications/c.desktop")),
                  entryText(QStringLiteral("Gamma Tool"), QStringLiteral("gamma"))));
    QVERIFY(service.toggle());
    QCOMPARE(service.query(), QString());
    QTRY_COMPARE_WITH_TIMEOUT(service.applicationCount(), 3, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(service.results().size(), 3, 5000);
}

void AppsTest::theSelectionWrapsAndResetsWithTheResults()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    for (const char* name : {"a", "b", "c"})
        QVERIFY(write(root.filePath(QStringLiteral("applications/%1.desktop").arg(QLatin1String(name))),
                      entryText(QStringLiteral("App %1").arg(QLatin1String(name)), QStringLiteral("x"))));
    LauncherService service({root.path()}, {}, QString());
    service.setOpen(true);
    QTRY_COMPARE_WITH_TIMEOUT(service.results().size(), 3, 5000);
    QCOMPARE(service.selectedIndex(), 0);
    service.moveSelection(1);
    QCOMPARE(service.selectedIndex(), 1);
    service.moveSelection(1);
    service.moveSelection(1);
    QCOMPARE(service.selectedIndex(), 0);  // wrapped past the last
    service.moveSelection(-1);
    QCOMPARE(service.selectedIndex(), 2);  // and back past the first
    service.moveSelection(1);
    service.setQuery(QStringLiteral("b"));
    QCOMPARE(service.selectedIndex(), 0);  // a new list starts at its first result
    service.setQuery(QStringLiteral("zzz"));
    service.moveSelection(1);
    QCOMPARE(service.selectedIndex(), -1);  // nothing to move through
}

void AppsTest::aLaunchStartsARealProcessWithItsArgumentsAndClosesThePanel()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString marker = root.filePath(QStringLiteral("started with spaces.txt"));
    // `touch` with a path containing spaces, quoted: the marker's existence is the process having run with the
    // exact argument the entry named, and no shell was there to split it.
    const QString touch = QStandardPaths::findExecutable(QStringLiteral("touch"));
    if (touch.isEmpty())
        QSKIP("no `touch` on this machine to start");
    QVERIFY(write(root.filePath(QStringLiteral("applications/touch.desktop")),
                  entryText(QStringLiteral("Touch"), QStringLiteral("%1 \"%2\" %U").arg(touch, marker))));

    LauncherService service({root.path()}, {}, QString());
    service.setOpen(true);
    QTRY_COMPARE_WITH_TIMEOUT(service.results().size(), 1, 5000);
    QVERIFY(!QFileInfo::exists(marker));

    QVERIFY(service.launchSelected());
    QVERIFY(!service.isOpen());
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);

    // Out-of-range asks start nothing.
    QVERIFY(!service.launch(-1));
    QVERIFY(!service.launch(99));
}

void AppsTest::aProgramThatCannotStartLeavesThePanelOpen()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(write(root.filePath(QStringLiteral("applications/broken.desktop")),
                  entryText(QStringLiteral("Broken"), QStringLiteral("/nonexistent/qs-apps-test-binary"))));
    LauncherService service({root.path()}, {}, QString());
    service.setOpen(true);
    QTRY_COMPARE_WITH_TIMEOUT(service.results().size(), 1, 5000);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("could not start")));
    QVERIFY(!service.launchSelected());
    QVERIFY2(service.isOpen(), "the panel closed on a launch that did not start anything");
}

void AppsTest::maxResultsIsBoundedAndCapsTheList()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    for (int i = 0; i < 5; ++i)
        QVERIFY(write(root.filePath(QStringLiteral("applications/app%1.desktop").arg(i)),
                      entryText(QStringLiteral("App %1").arg(i), QStringLiteral("x"))));
    LauncherService service({root.path()}, {}, QString());
    service.setOpen(true);
    QTRY_COMPARE_WITH_TIMEOUT(service.results().size(), 5, 5000);
    QSignalSpy changed(&service, &LauncherService::maxResultsChanged);
    service.setMaxResults(2);
    QCOMPARE(service.results().size(), 2);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(service.applicationCount(), 5);  // the cap is on what is shown, not on what was found

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("outside")));
    service.setMaxResults(0);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("outside")));
    service.setMaxResults(LauncherService::MaxMaxResults + 1);
    QCOMPARE(service.maxResults(), 2);
    QCOMPARE(changed.count(), 1);
}

void AppsTest::frecencyWeighsTheCountByHowRecentTheLastLaunchWas()
{
    constexpr qint64 day = 24LL * 60 * 60 * 1000;
    const qint64 now = 1'000'000LL * day;
    QCOMPARE(quantum::apps::frecency({3, now - 1 * day}, now), qint64{300});
    QCOMPARE(quantum::apps::frecency({3, now - 5 * day}, now), qint64{210});
    QCOMPARE(quantum::apps::frecency({3, now - 20 * day}, now), qint64{150});
    QCOMPARE(quantum::apps::frecency({3, now - 60 * day}, now), qint64{90});
    QCOMPARE(quantum::apps::frecency({3, now - 400 * day}, now), qint64{30});
    // The edges: exactly four days is the second bucket, one millisecond short of it the first.
    QCOMPARE(quantum::apps::frecency({1, now - 4 * day + 1}, now), qint64{100});
    QCOMPARE(quantum::apps::frecency({1, now - 4 * day}, now), qint64{70});
    // Often long ago yields to a few times lately.
    QVERIFY(quantum::apps::frecency({2, now - 1 * day}, now) > quantum::apps::frecency({15, now - 400 * day}, now));
    // Nothing launched is nothing; a clock set back is the newest bucket and not a negative age.
    QCOMPARE(quantum::apps::frecency({0, now}, now), qint64{0});
    QCOMPARE(quantum::apps::frecency({2, now + 10 * day}, now), qint64{200});

    LaunchHistory history;
    quantum::apps::recordLaunch(history, QStringLiteral("a.desktop"), 5);
    quantum::apps::recordLaunch(history, QStringLiteral("a.desktop"), 9);
    QCOMPARE(history.value(QStringLiteral("a.desktop")), (LaunchRecord{2, 9}));
}

void AppsTest::theHistoryFileRoundTripsAndRefusesWhatIsNotItsFormat()
{
    LaunchHistory history;
    history.insert(QStringLiteral("a.desktop"), {4, 1'700'000'000'123LL});
    history.insert(QStringLiteral("org.x.B.desktop"), {1, 5});
    QString error;
    const auto parsed = quantum::apps::parseHistory(quantum::apps::serializeHistory(history), &error);
    QVERIFY2(parsed.has_value(), qPrintable(error));
    QCOMPARE(*parsed, history);

    for (const char* bad : {"", "not json", "[1, 2]", "3", "{\"a\": "}) {
        QString why;
        QVERIFY2(!quantum::apps::parseHistory(bad, &why).has_value(), bad);
        QVERIFY2(!why.isEmpty(), bad);
    }
    // One bad entry is dropped and the rest are kept: a non-object, a missing field, a count that is zero,
    // negative or fractional, a time that is text or fractional.
    const auto partial = quantum::apps::parseHistory(
        R"({"ok.desktop": {"count": 2, "last": 7}, "s": 4, "nolast": {"count": 1},
            "zero": {"count": 0, "last": 1}, "neg": {"count": -3, "last": 1}, "frac": {"count": 1.5, "last": 1},
            "text": {"count": "2", "last": 1}, "ftime": {"count": 1, "last": 1.5}})");
    QVERIFY(partial.has_value());
    QCOMPARE(partial->size(), 1);
    QCOMPARE(partial->value(QStringLiteral("ok.desktop")), (LaunchRecord{2, 7}));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("state/deeper/launcher-history.json"));
    // Not there yet: an empty history and no error, because nothing has been launched.
    QString readError;
    QVERIFY(quantum::apps::readHistoryFile(path, &readError).isEmpty());
    QVERIFY2(readError.isEmpty(), qPrintable(readError));
    // Written into a directory that did not exist, and read back.
    QString writeError;
    QVERIFY2(quantum::apps::writeHistoryFile(path, history, &writeError), qPrintable(writeError));
    QCOMPARE(quantum::apps::readHistoryFile(path, &readError), history);
    // A file that exists and is not the format is an empty history *and* an error, and is replaced by the next write.
    QVERIFY(write(path, QStringLiteral("garbage")));
    readError.clear();
    QVERIFY(quantum::apps::readHistoryFile(path, &readError).isEmpty());
    QVERIFY(!readError.isEmpty());
    QVERIFY2(quantum::apps::writeHistoryFile(path, history, &writeError), qPrintable(writeError));
    QCOMPARE(quantum::apps::readHistoryFile(path, &readError), history);
}

void AppsTest::frecencyOrdersTiesAndNeverOutranksABetterMatch()
{
    constexpr qint64 day = 24LL * 60 * 60 * 1000;
    const qint64 now = 1'000'000LL * day;
    const QList<DesktopEntry> entries{
        make(QStringLiteral("a"), QStringLiteral("Alpha"), QStringLiteral("/usr/bin/alpha")),
        make(QStringLiteral("b"), QStringLiteral("Beta"), QStringLiteral("/usr/bin/beta")),
        make(QStringLiteral("g"), QStringLiteral("Gamma"), QStringLiteral("/usr/bin/gamma")),
        make(QStringLiteral("f"), QStringLiteral("Firefox"), QStringLiteral("/usr/bin/firefox")),
        make(QStringLiteral("w"), QStringLiteral("Web Browser"), QStringLiteral("/usr/bin/firefox-esr"))};
    LaunchHistory history;
    history.insert(QStringLiteral("g"), {5, now - 1 * day});
    history.insert(QStringLiteral("b"), {1, now - 1 * day});
    history.insert(QStringLiteral("w"), {50, now - 1 * day});

    // The empty query: every entry ties at zero, so history decides, then the name.
    const auto opened = LauncherService::rank(entries, QString(), 10, history, now);
    QCOMPARE(opened.at(0).id, QStringLiteral("w"));
    QCOMPARE(opened.at(1).id, QStringLiteral("g"));
    QCOMPARE(opened.at(2).id, QStringLiteral("b"));
    QCOMPARE(opened.at(3).id, QStringLiteral("a"));
    QCOMPARE(opened.at(4).id, QStringLiteral("f"));
    // Without a history it is by name, as before.
    QCOMPARE(LauncherService::rank(entries, QString(), 10).at(0).id, QStringLiteral("a"));

    // `fire`: Firefox is a prefix of the name and "Web Browser" matches only through its program, which is weaker;
    // fifty launches of the latter do not lift it over the former.
    const auto fire = LauncherService::rank(entries, QStringLiteral("fire"), 10, history, now);
    QCOMPARE(fire.at(0).id, QStringLiteral("f"));
    QCOMPARE(fire.at(1).id, QStringLiteral("w"));
}

void AppsTest::aLaunchIsRememberedAndOrdersTheNextOpen()
{
    const QString touch = QStandardPaths::findExecutable(QStringLiteral("touch"));
    if (touch.isEmpty())
        QSKIP("no `touch` on this machine to start");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString marker = root.filePath(QStringLiteral("ran"));
    QVERIFY(write(root.filePath(QStringLiteral("applications/a.desktop")),
                  entryText(QStringLiteral("Alpha"), QStringLiteral("/nonexistent-alpha"))));
    QVERIFY(write(root.filePath(QStringLiteral("applications/b.desktop")),
                  entryText(QStringLiteral("Beta"), QStringLiteral("%1 \"%2\"").arg(touch, marker))));
    const QString history = root.filePath(QStringLiteral("state/launcher-history.json"));

    {
        LauncherService service({root.path()}, {}, QString());
        service.setHistoryPath(history);
        service.setOpen(true);
        QTRY_COMPARE_WITH_TIMEOUT(service.results().size(), 2, 5000);
        // By name, nothing launched yet.
        QCOMPARE(service.shown().at(0).id, QStringLiteral("a.desktop"));
        QVERIFY(!QFileInfo::exists(history));

        service.setQuery(QStringLiteral("beta"));
        QVERIFY(service.launchSelected());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);
        // The history is written off the GUI thread; the destructor waits for it.
    }
    QVERIFY(QFileInfo::exists(history));
    QString error;
    const LaunchHistory stored = quantum::apps::readHistoryFile(history, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(stored.size(), 1);
    QCOMPARE(stored.value(QStringLiteral("b.desktop")).count, 1);

    // A new service — a new shell — with the same file opens with the launched entry first, though "Alpha" is
    // first by name.
    LauncherService again({root.path()}, {}, QString());
    again.setHistoryPath(history);
    again.setOpen(true);
    QTRY_COMPARE_WITH_TIMEOUT(again.results().size(), 2, 5000);
    QCOMPARE(again.shown().at(0).id, QStringLiteral("b.desktop"));

    // Without a path there is no history and no file: the order is by name.
    LauncherService none({root.path()}, {}, QString());
    none.setOpen(true);
    QTRY_COMPARE_WITH_TIMEOUT(none.results().size(), 2, 5000);
    QCOMPARE(none.shown().at(0).id, QStringLiteral("a.desktop"));
}

QTEST_MAIN(AppsTest)
#include "apps_test.moc"
