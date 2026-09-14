// The configuration watcher against real files. No compositor and no display: what this needs is a
// directory it owns, which is why `ConfigWatcher` takes its path as an argument — the alternative is a
// test that reads and writes whatever configuration the person running it happens to have.
//
// Each slot is given a fresh scratch directory, so nothing here depends on what a slot before it left
// on disk. That matters twice over: the shuffled slot-order check runs these slots in orders nobody
// wrote down, and a slot that passed only because an earlier one had already written the file would be
// exactly the bug that check exists to find.
#include "config/Config.h"
#include "config/ConfigWatcher.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using quantum::config::Config;
using quantum::config::ConfigBar;
using quantum::config::ConfigWatcher;

namespace {

constexpr int coveredDefaultHeight = 32;
constexpr auto coveredDefaultNamespace = "quantum-shell-bar";

QByteArray configFile(int height, const QByteArray& extra = QByteArray()) {
    return QByteArray("schema_version = 1\n[bar]\nheight = ") + QByteArray::number(height) + "\n" + extra;
}

}  // namespace

class ConfigWatcherTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aMissingFileIsNotAProblem();
    void appliesTheFileThatIsAlreadyThere();
    void appliesAFileWrittenWhileTheShellIsRunning();
    void appliesAFileInADirectoryThatDidNotExistYet();
    void survivesAnAtomicReplaceWithoutEverAnnouncingTheGap();
    void fallsBackToTheDefaultsWhenTheFileIsRemoved();
    void onlyThePropertyThatChangedEmits();
    void reportsAnUnknownKeyAndStillUsesTheRestOfTheFile();
    void keepsTheLastGoodValuesWhenTheFileCannotBeUsed();

private:
    QString configPath() const;
    // Writes the file the way an editor's atomic save does on a filesystem without an atomic replace:
    // a new file is written beside it and renamed over the top, so the file's inode — the thing a watch
    // on the file itself is attached to — is gone afterwards.
    bool replace(const QByteArray& text);
    // Writes it in place, truncating what was there.
    bool overwrite(const QByteArray& text);
    // Every height the bar was told about, in order, from now on.
    QList<int> recordHeights();

    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<Config> config_;
    std::unique_ptr<ConfigWatcher> watcher_;
    QList<int> announcedHeights_;
};

QString ConfigWatcherTest::configPath() const {
    return QDir(directory_->filePath(QStringLiteral("quantum-shell")))
        .filePath(QStringLiteral("config.toml"));
}

bool ConfigWatcherTest::replace(const QByteArray& text) {
    const QString directory = QFileInfo(configPath()).absolutePath();
    if (!QDir().mkpath(directory))
        return false;

    QFile staging(QDir(directory).filePath(QStringLiteral("config.toml.new")));
    if (!staging.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool written = staging.write(text) == text.size();
    staging.close();
    if (!written)
        return false;

    // Two syscalls with nothing between them, which is what makes the window in which the file does not
    // exist too short for the watcher's event loop to be run in it. That is not a property of this test:
    // it is the property the directory watch exists for, and this slot is what asserts it holds.
    QFile::remove(configPath());
    return staging.rename(configPath());
}

bool ConfigWatcherTest::overwrite(const QByteArray& text) {
    if (!QDir().mkpath(QFileInfo(configPath()).absolutePath()))
        return false;
    QFile file(configPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(text) == text.size();
}

QList<int> ConfigWatcherTest::recordHeights() {
    announcedHeights_.clear();
    return announcedHeights_;
}

void ConfigWatcherTest::init() {
    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    config_ = std::make_unique<Config>();
    // Connected once, in `init`, rather than by `recordHeights`: a slot that records twice would
    // otherwise have two lambdas appending to the same list and would see every announcement twice.
    QObject::connect(config_->bar(), &ConfigBar::heightChanged, config_.get(),
                     [this] { announcedHeights_.append(config_->bar()->height()); });
    // Constructed but not started: a slot that wants to observe the first read has to be able to connect
    // before it happens.
    watcher_ = std::make_unique<ConfigWatcher>(*config_, configPath());
}

void ConfigWatcherTest::cleanup() {
    watcher_.reset();
    config_.reset();
    directory_.reset();
}

void ConfigWatcherTest::aMissingFileIsNotAProblem() {
    // Defaults ship embedded, so a machine where nobody has written a config is a working shell. It is
    // also not news: the read reports nothing, on a signal that carries an empty list rather than being
    // silent, so "clean" and "has not read yet" are different things to a listener.
    QSignalSpy diagnosedSpy(watcher_.get(), &ConfigWatcher::diagnosed);
    watcher_->start();

    QCOMPARE(config_->bar()->height(), coveredDefaultHeight);
    QCOMPARE(config_->bar()->layerNamespace(), QString::fromLatin1(coveredDefaultNamespace));
    QCOMPARE(diagnosedSpy.count(), 1);
    QCOMPARE(diagnosedSpy.at(0).at(0).toStringList(), QStringList());
}

void ConfigWatcherTest::appliesTheFileThatIsAlreadyThere() {
    QVERIFY(replace(configFile(44)));
    QSignalSpy diagnosedSpy(watcher_.get(), &ConfigWatcher::diagnosed);
    watcher_->start();

    // Applied by the time start() returns, not a moment later: this read is the one that runs before the
    // QML engine loads, which is what keeps the bar from being built at the default height and resized
    // once the file has been read.
    QCOMPARE(config_->bar()->height(), 44);
    QCOMPARE(diagnosedSpy.count(), 1);
}

void ConfigWatcherTest::appliesAFileWrittenWhileTheShellIsRunning() {
    watcher_->start();
    QCOMPARE(config_->bar()->height(), coveredDefaultHeight);

    QVERIFY(overwrite(configFile(48)));
    QTRY_COMPARE(config_->bar()->height(), 48);
}

void ConfigWatcherTest::appliesAFileInADirectoryThatDidNotExistYet() {
    // Nothing under the scratch directory at all: `~/.config/quantum-shell` has never been created and
    // the only thing there is to watch is `~/.config`. The file the user creates later still has to
    // arrive, which is what re-deriving the watch set after every event is for.
    QVERIFY(!QFileInfo(configPath()).absoluteDir().exists());
    watcher_->start();
    QCOMPARE(config_->bar()->height(), coveredDefaultHeight);

    QVERIFY(replace(configFile(52)));
    QTRY_COMPARE(config_->bar()->height(), 52);
}

void ConfigWatcherTest::survivesAnAtomicReplaceWithoutEverAnnouncingTheGap() {
    QVERIFY(replace(configFile(40)));
    watcher_->start();
    QCOMPARE(config_->bar()->height(), 40);

    recordHeights();
    QVERIFY(replace(configFile(56)));
    QTRY_COMPARE(config_->bar()->height(), 56);

    // The claim is not just that the new value arrived. It is that nothing was announced in between: the
    // file was unlinked and recreated within one turn of the event loop, and a watcher that read on the
    // unlink would have found no file, fallen back to the defaults and told the bar it was 32 high on its
    // way to 56.
    QCOMPARE(announcedHeights_, QList<int>{56});

    // And the watch has to have survived the replace. Renaming a new file over the old one leaves the
    // inode the file watch was attached to unlinked, so a watcher that did not re-derive what to watch
    // would be watching nothing at all from here on: the next edit would be to the new file and would
    // never be noticed. An in-place write is the case that shows it, because a directory event is not
    // involved in one.
    recordHeights();
    QVERIFY(overwrite(configFile(64)));
    QTRY_COMPARE(config_->bar()->height(), 64);
    QCOMPARE(announcedHeights_, QList<int>{64});
}

void ConfigWatcherTest::fallsBackToTheDefaultsWhenTheFileIsRemoved() {
    QVERIFY(replace(configFile(44)));
    watcher_->start();
    QCOMPARE(config_->bar()->height(), 44);

    QVERIFY(QFile::remove(configPath()));
    // A file that is gone at rest is the honest empty state for a configuration: the file is not a
    // half-written one being replaced, it is not there, and the shell's own defaults are what it says.
    QTRY_COMPARE(config_->bar()->height(), coveredDefaultHeight);
}

void ConfigWatcherTest::onlyThePropertyThatChangedEmits() {
    QVERIFY(replace(configFile(coveredDefaultHeight)));
    watcher_->start();
    QCOMPARE(config_->bar()->height(), coveredDefaultHeight);

    QSignalSpy heightSpy(config_->bar(), &ConfigBar::heightChanged);
    QSignalSpy namespaceSpy(config_->bar(), &ConfigBar::layerNamespaceChanged);
    QVERIFY(replace(configFile(40)));
    QTRY_COMPARE(config_->bar()->height(), 40);

    QCOMPARE(heightSpy.count(), 1);
    QCOMPARE(namespaceSpy.count(), 0);

    // And an edit that changes nothing at all emits nothing at all, which is the half of the diff that
    // keeps a binding from re-evaluating because a file was saved without being edited.
    QVERIFY(replace(configFile(40)));
    QTest::qWait(150);
    QCOMPARE(heightSpy.count(), 1);
    QCOMPARE(namespaceSpy.count(), 0);
}

void ConfigWatcherTest::reportsAnUnknownKeyAndStillUsesTheRestOfTheFile() {
    QSignalSpy diagnosedSpy(watcher_.get(), &ConfigWatcher::diagnosed);
    QVERIFY(replace(configFile(44, "colour = \"#ffffff\"\n")));
    watcher_->start();

    QCOMPARE(diagnosedSpy.count(), 1);
    const QStringList messages = diagnosedSpy.at(0).at(0).toStringList();
    QCOMPARE(messages.size(), 1);

    // The message names the key and the file it is in. The prefix is the watcher's — the schema's
    // warnings are about a file's contents and know nothing about where that file is — and it is what
    // makes the line actionable when a user runs the shell in a terminal.
    QVERIFY(messages.first().contains(QStringLiteral("bar.colour")));
    QVERIFY(messages.first().contains(configPath()));

    // Warned about, not refused: the keys the shell does read were usable and have been applied.
    QCOMPARE(config_->bar()->height(), 44);
}

void ConfigWatcherTest::keepsTheLastGoodValuesWhenTheFileCannotBeUsed() {
    QVERIFY(replace(configFile(44)));
    watcher_->start();
    QCOMPARE(config_->bar()->height(), 44);

    recordHeights();
    QSignalSpy diagnosedSpy(watcher_.get(), &ConfigWatcher::diagnosed);
    QVERIFY(overwrite(QByteArray("this is not toml {{{\n")));

    QTRY_COMPARE(diagnosedSpy.count(), 1);
    QVERIFY(diagnosedSpy.at(0).at(0).toStringList().first().contains(QStringLiteral("TOML")));

    // A typo in one key must not reset a working bar to the defaults, so a file nothing can be read out
    // of is not applied at all. Waiting past the read is the point: the failure mode this guards against
    // is a reset that happens a moment after the warning, which an assertion taken immediately would miss.
    QTest::qWait(150);
    QCOMPARE(config_->bar()->height(), 44);
    QCOMPARE(announcedHeights_, QList<int>{});
}

QTEST_GUILESS_MAIN(ConfigWatcherTest)
#include "config_watcher_test.moc"
