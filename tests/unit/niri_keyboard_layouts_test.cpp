// Unit tests for the configured keyboard layouts and the index niri reports into them.
#include "niri/NiriKeyboardLayouts.h"

#include "NiriProtocolTestData.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QTest>

using quantum::niri::NiriKeyboardLayouts;
using qstest::keyboardLayoutsObject;

class NiriKeyboardLayoutsTest : public QObject {
    Q_OBJECT

private slots:
    void parsesTheNamesAndTheActiveIndex();
    void anEmptyNameListIsParsedButNamesNoLayout();
    void refusesAnObjectWithNoNamesArray();
    void anIndexPastTheEndNamesNothing();
    void storesAnIndexRatherThanClampingIt();
    void comparesByValue();
    void describesWhatItHolds();
};

void NiriKeyboardLayoutsTest::parsesTheNamesAndTheActiveIndex() {
    const NiriKeyboardLayouts layouts = NiriKeyboardLayouts::fromJson(
        keyboardLayoutsObject({QStringLiteral("English (US)"), QStringLiteral("German")}, 1));

    QVERIFY(layouts.isValid());
    QCOMPARE(layouts.size(), 2);
    QCOMPARE(layouts.names(),
             QStringList({QStringLiteral("English (US)"), QStringLiteral("German")}));
    QCOMPARE(layouts.currentIndex(), 1);
    QCOMPARE(layouts.currentName(), QStringLiteral("German"));
    QVERIFY(!layouts.isEmpty());
}

void NiriKeyboardLayoutsTest::anEmptyNameListIsParsedButNamesNoLayout() {
    const NiriKeyboardLayouts layouts =
        NiriKeyboardLayouts::fromJson(keyboardLayoutsObject({}, 0));

    QVERIFY(layouts.isValid());
    QVERIFY(layouts.isEmpty());
    QCOMPARE(layouts.size(), 0);
    QVERIFY(layouts.currentName().isEmpty());
}

void NiriKeyboardLayoutsTest::refusesAnObjectWithNoNamesArray() {
    QVERIFY(!NiriKeyboardLayouts::fromJson(QJsonObject{}).isValid());
    QVERIFY(!NiriKeyboardLayouts::fromJson(
                 QJsonObject{{QStringLiteral("names"), QJsonValue(QStringLiteral("us"))}})
                 .isValid());

    // A name that is not a string makes the whole object unreadable rather than being skipped:
    // skipping it would shift every index after it, and current_idx would then point at a different
    // layout than the one the compositor reported.
    QVERIFY(!NiriKeyboardLayouts::fromJson(
                 QJsonObject{{QStringLiteral("names"), QJsonArray{QJsonValue(1), QStringLiteral("de")}},
                             {QStringLiteral("current_idx"), QJsonValue(1)}})
                 .isValid());
}

void NiriKeyboardLayoutsTest::anIndexPastTheEndNamesNothing() {
    const NiriKeyboardLayouts layouts = NiriKeyboardLayouts::fromJson(
        keyboardLayoutsObject({QStringLiteral("us"), QStringLiteral("de")}, 4));

    // The index is kept as reported and simply does not resolve: answering with `us` or `de` would be
    // a layout the compositor did not name.
    QCOMPARE(layouts.currentIndex(), 4);
    QVERIFY(layouts.currentName().isEmpty());
}

void NiriKeyboardLayoutsTest::storesAnIndexRatherThanClampingIt() {
    NiriKeyboardLayouts layouts = NiriKeyboardLayouts::fromJson(
        keyboardLayoutsObject({QStringLiteral("us"), QStringLiteral("de")}, 0));
    QCOMPARE(layouts.currentName(), QStringLiteral("us"));

    // This is what a KeyboardLayoutSwitched applies.
    layouts.setCurrentIndex(1);
    QCOMPARE(layouts.currentIndex(), 1);
    QCOMPARE(layouts.currentName(), QStringLiteral("de"));

    layouts.setCurrentIndex(7);
    QCOMPARE(layouts.currentIndex(), 7);
    QVERIFY(layouts.currentName().isEmpty());
}

void NiriKeyboardLayoutsTest::comparesByValue() {
    const NiriKeyboardLayouts first = NiriKeyboardLayouts::fromJson(
        keyboardLayoutsObject({QStringLiteral("us"), QStringLiteral("de")}, 0));
    const NiriKeyboardLayouts same = NiriKeyboardLayouts::fromJson(
        keyboardLayoutsObject({QStringLiteral("us"), QStringLiteral("de")}, 0));
    QVERIFY(first == same);
    QVERIFY(!(first != same));

    const NiriKeyboardLayouts switched = NiriKeyboardLayouts::fromJson(
        keyboardLayoutsObject({QStringLiteral("us"), QStringLiteral("de")}, 1));
    QVERIFY(first != switched);

    const NiriKeyboardLayouts otherNames =
        NiriKeyboardLayouts::fromJson(keyboardLayoutsObject({QStringLiteral("us")}, 0));
    QVERIFY(first != otherNames);

    QVERIFY(first != NiriKeyboardLayouts{});
}

void NiriKeyboardLayoutsTest::describesWhatItHolds() {
    const NiriKeyboardLayouts layouts = NiriKeyboardLayouts::fromJson(
        keyboardLayoutsObject({QStringLiteral("us"), QStringLiteral("de")}, 1));
    const QString description = layouts.describe();
    QVERIFY(description.contains(QStringLiteral("us,de")));
    QVERIFY(description.contains(QStringLiteral("de")));

    const NiriKeyboardLayouts past = NiriKeyboardLayouts::fromJson(
        keyboardLayoutsObject({QStringLiteral("us")}, 3));
    QVERIFY(past.describe().contains(QStringLiteral("index names no layout")));

    QVERIFY(NiriKeyboardLayouts{}.describe().contains(QStringLiteral("no keyboard layouts reported")));
}

QTEST_GUILESS_MAIN(NiriKeyboardLayoutsTest)
#include "niri_keyboard_layouts_test.moc"
