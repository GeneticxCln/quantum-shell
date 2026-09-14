// Unit tests for the reconciliation the live snapshot comparisons run on.
//
// The live test takes these comparisons against a real compositor, where the race they exist to survive
// cannot be produced on demand: it needs a window renamed, a workspace focused or a layout switched
// between two readings of the desktop. What can be pinned here is the contract — that a reading which
// disagrees and then agrees is reconciled rather than failed, that the budget is bounded, that exhaustion
// *fails* rather than passing quietly, and that the failure tells a model which missed an event apart
// from a desktop that never held still. Those are the properties that make the live comparisons worth
// trusting, and none of them needs a compositor. It is also deliberately shaped so that the retry cannot
// be mistaken for a way to pass without agreeing: slot 4 fails if the loop ever reports agreement it was
// not given.
#include "SnapshotReconcile.h"

#include <QTest>
#include <QVariant>

using qstest::Disagreement;
using qstest::Reading;
using qstest::ReconcileOutcome;
using qstest::describeFailure;
using qstest::describeReconciliation;
using qstest::describeValue;
using qstest::reconcile;

namespace {

// A reading of a compositor that does not exist. Every slot drives the loop with a script of readings, so
// each of the two ways a comparison can straddle a change is a case here rather than a hope.
class ScriptedCompositor {
public:
    // `mismatches` is what comparing the model against a reading finds, in order; the last entry is
    // repeated once the script runs out, which is what a compositor does when the desktop stops changing.
    //
    // `stamps` is what the compositor's own answer fingerprints as. Empty means every reading is of the
    // same state — a settled desktop, the case where a model that still disagrees has missed an event.
    // `aNewStampEveryRead` is the opposite and has to be said explicitly rather than scripted, because a
    // finite script of distinct stamps would exhaust itself and start repeating, which is a settled
    // desktop again after all: a desktop genuinely changing faster than it can be read never answers the
    // same thing twice, however many readings are taken.
    explicit ScriptedCompositor(QList<QString> mismatches, QList<QString> stamps = {},
                                bool aNewStampEveryRead = false)
        : mismatches_(std::move(mismatches))
        , stamps_(std::move(stamps))
        , aNewStampEveryRead_(aNewStampEveryRead)
    {
        Q_ASSERT(!(aNewStampEveryRead_ && !stamps_.isEmpty()));
    }

    qstest::TakeReading reader()
    {
        return [this]() {
            const int at = calls_;
            ++calls_;
            Reading reading;
            if (aNewStampEveryRead_) {
                reading.stamp = QStringLiteral("reading %1").arg(at);
            } else if (stamps_.isEmpty()) {
                reading.stamp = QStringLiteral("unchanged");
            } else {
                reading.stamp = stamps_.value(qMin(at, stamps_.size() - 1));
            }
            reading.mismatch = mismatches_.value(qMin(at, mismatches_.size() - 1));
            return reading;
        };
    }

    int calls() const { return calls_; }

private:
    QList<QString> mismatches_;
    QList<QString> stamps_;
    bool aNewStampEveryRead_ = false;
    int calls_ = 0;
};

// A budget long enough to take several readings, short enough that the slots which exhaust it stay quick.
// The settle is a good fraction of it on purpose: how many readings a slot gets is then the ratio of the
// two numbers — a handful, either way — rather than a function of how fast or how loaded the machine is.
// No slot here asserts a wall-clock figure for the same reason: a budget that bounds the readings is what
// is being tested, and the count is the part that can be asserted without racing the machine.
constexpr int impatientBudgetMs = 80;
constexpr int quickSettleMs = 20;

}  // namespace

class SnapshotReconcileTest : public QObject {
    Q_OBJECT

private slots:
    void agreesOnTheFirstReading();
    void reconcilesAReadingTheCompositorOvertook();
    void reconcilesAModelThatWasBehind();
    void failsWhenTheModelNeverReachesASettledReading();
    void failsWhenTheCompositorNeverHoldsStill();
    void theBudgetBoundsHowManyReadingsAreTaken();
    void keepsTheFirstDisagreement();
    void stopsImmediatelyWhenThereIsNoReadingToCompare();
    void aReconciledRunSaysWhatItReconciledAbout();
    void describesBothSidesOfAFieldAndTheAbsentOne();
    void namesTheTwoSidesTheCallerNamed();
};

void SnapshotReconcileTest::agreesOnTheFirstReading() {
    ScriptedCompositor compositor({QString()});
    const ReconcileOutcome outcome = reconcile(compositor.reader(), impatientBudgetMs, quickSettleMs);

    QVERIFY(outcome.agreed);
    QCOMPARE(outcome.attempts, 1);
    QVERIFY(!outcome.reconciled());
    QVERIFY(outcome.firstMismatch.isEmpty());
    // One reading, so nothing was said about reconciling: a first-read agreement is the common case and
    // must not look like a race in the log.
    QVERIFY(describeReconciliation(QStringLiteral("windows"), outcome).isEmpty());
}

void SnapshotReconcileTest::reconcilesAReadingTheCompositorOvertook() {
    // The compositor answered something the model had already moved past, and answers fresh on the second
    // read: this is a window renamed between the model's event and the reply.
    ScriptedCompositor compositor({QStringLiteral("window 3 title: model \"B\", compositor \"A\""),
                                   QString(),
                                   QString()},
                                  {QStringLiteral("before the rename"), QStringLiteral("after it")});
    const ReconcileOutcome outcome = reconcile(compositor.reader(), impatientBudgetMs, quickSettleMs);

    QVERIFY2(outcome.agreed, qPrintable(describeFailure(QStringLiteral("the window title"), outcome)));
    QCOMPARE(outcome.attempts, 2);
    QVERIFY(outcome.reconciled());
    // The reading that disagreed is still named, because it is what a reader investigating the run needs.
    QVERIFY(outcome.firstMismatch.contains(QStringLiteral("B")));
    const QString reported = describeReconciliation(QStringLiteral("windows"), outcome);
    QVERIFY2(!reported.isEmpty(), "a run that needed a second reading said nothing about it");
    QVERIFY(reported.contains(QStringLiteral("2 reading")));
}

void SnapshotReconcileTest::reconcilesAModelThatWasBehind() {
    // The compositor's answer never changed and the model caught up to it: the event was already on its
    // way. The stamp script makes that explicit — the same stamp on both reads.
    ScriptedCompositor compositor({QStringLiteral("workspace 7 is_active: model false, compositor true"),
                                   QString(),
                                   QString()});
    const ReconcileOutcome outcome = reconcile(compositor.reader(), impatientBudgetMs, quickSettleMs);

    QVERIFY(outcome.agreed);
    QCOMPARE(outcome.attempts, 2);
    QVERIFY(outcome.reconciled());
}

void SnapshotReconcileTest::failsWhenTheModelNeverReachesASettledReading() {
    // A model that has genuinely missed an event: the compositor answers the same thing every time and the
    // model never reaches it. This slot is what stops the loop being a way to pass without agreeing, and
    // it is the case that must be reported as the model's fault.
    ScriptedCompositor compositor({QStringLiteral("window 3 title: model \"A\", compositor \"B\"")});
    const ReconcileOutcome outcome = reconcile(compositor.reader(), impatientBudgetMs, quickSettleMs);

    QVERIFY(!outcome.agreed);
    QVERIFY2(outcome.attempts >= 2, "a disagreement was judged on a single reading");
    QVERIFY(outcome.readingsSettled());
    const QString failure = describeFailure(QStringLiteral("the window title"), outcome);
    // The stable answer is what makes this the model's fault, and the failure says so. The two diagnoses
    // are mutually exclusive in the text: each names its own and not the other's.
    QVERIFY2(failure.contains(QStringLiteral("stable reading existed")), qPrintable(failure));
    QVERIFY(!failure.contains(QStringLiteral("moving desktop")));
    // And it names both sides of what was wrong.
    QVERIFY(failure.contains(QStringLiteral("first disagreement")));
    QVERIFY(failure.contains(QStringLiteral("last disagreement")));
}

void SnapshotReconcileTest::failsWhenTheCompositorNeverHoldsStill() {
    // Every reading different: a desktop changing faster than it can be read. Still a failure — nothing
    // agreed — but it is not evidence against the model, and the two must not read the same.
    ScriptedCompositor compositor({QStringLiteral("workspace 1 idx: model 1, compositor 2")}, {},
                                 /* aNewStampEveryRead= */ true);
    const ReconcileOutcome outcome = reconcile(compositor.reader(), impatientBudgetMs, quickSettleMs);

    QVERIFY(!outcome.agreed);
    // The diagnosis this slot checks is a statement about a pair of readings, so a second is required for
    // it to be a statement at all — which the loop guarantees whatever the budget.
    QVERIFY2(outcome.attempts >= 2, "a disagreement was judged on a single reading");
    QVERIFY(!outcome.readingsSettled());
    const QString failure = describeFailure(QStringLiteral("the workspace order"), outcome);
    QVERIFY2(failure.contains(QStringLiteral("moving desktop")), qPrintable(failure));
    QVERIFY2(!failure.contains(QStringLiteral("stable reading existed")),
             "a desktop that never held still was reported as a stable reading the model did not reach");
}

void SnapshotReconcileTest::theBudgetBoundsHowManyReadingsAreTaken() {
    // A loop with no bound would hang a test run on a desktop that is never still, and would also be a way
    // of waiting for agreement indefinitely. Both are checked: readings stop, and they cost about what was
    // asked for rather than as long as the compositor keeps answering.
    ScriptedCompositor compositor({QStringLiteral("never agrees")});
    const ReconcileOutcome outcome = reconcile(compositor.reader(), impatientBudgetMs, quickSettleMs);

    QVERIFY(!outcome.agreed);
    QVERIFY2(outcome.attempts >= 2,
             "the loop took one reading, so its budget bounded nothing and the script was not retried");
    // Bounded by an order of magnitude: an unbounded loop would take thousands of readings here, so a
    // count in the tens is the budget doing its job and a count in the thousands is it not.
    QVERIFY2(outcome.attempts <= 12,
             qPrintable(QStringLiteral("the loop took %1 readings against an %2 ms budget and a %3 ms "
                                       "settle, so something other than the budget stopped it")
                            .arg(outcome.attempts)
                            .arg(impatientBudgetMs)
                            .arg(quickSettleMs)));
}

void SnapshotReconcileTest::keepsTheFirstDisagreement() {
    // The readings disagree about something different each time, because the desktop keeps moving. The two
    // the failure keeps are pinned here: the first, which describes what was wrong when the two sides were
    // first asked, and the last, which describes the reading the loop gave up on. The script never agrees,
    // so this needs no agreement to end it and holds however many readings the machine gets through.
    ScriptedCompositor compositor({QStringLiteral("first: model A, compositor B"),
                                   QStringLiteral("then: model C, compositor D")},
                                  {QStringLiteral("read 1"), QStringLiteral("read 2")});
    const ReconcileOutcome outcome = reconcile(compositor.reader(), impatientBudgetMs, quickSettleMs);

    QVERIFY(!outcome.agreed);
    QVERIFY2(outcome.attempts >= 2, "a disagreement was judged on a single reading");
    QCOMPARE(outcome.firstMismatch, QStringLiteral("first: model A, compositor B"));
    QCOMPARE(outcome.lastMismatch, QStringLiteral("then: model C, compositor D"));
    // And the failure names both rather than only the one it gave up on.
    const QString failure = describeFailure(QStringLiteral("the snapshot"), outcome);
    QVERIFY(failure.contains(QStringLiteral("model A")));
    QVERIFY(failure.contains(QStringLiteral("model C")));
}

void SnapshotReconcileTest::stopsImmediatelyWhenThereIsNoReadingToCompare() {
    // A request that was never answered, or was refused, is not a race: a timeout has already waited, and
    // taking it again is not a second reading of anything. The loop must stop on it at once and report the
    // request, because diagnosing the model over a comparison that never happened would send a reader to
    // the wrong place entirely.
    int calls = 0;
    const ReconcileOutcome outcome = reconcile(
        [&calls] {
            ++calls;
            Reading reading;
            reading.stamp = QStringLiteral("no reading");
            reading.noReading = QStringLiteral("the compositor never answered a Windows request");
            return reading;
        },
        impatientBudgetMs, quickSettleMs);

    QVERIFY(!outcome.agreed);
    QCOMPARE(calls, 1);
    QCOMPARE(outcome.attempts, 1);
    QVERIFY(!outcome.readSomething());
    QVERIFY(!outcome.reconciled());
    const QString failure = describeFailure(QStringLiteral("the window snapshot"), outcome);
    QVERIFY2(failure.contains(QStringLiteral("never answered")), qPrintable(failure));
    QVERIFY2(!failure.contains(QStringLiteral("stable reading existed")), qPrintable(failure));
    QVERIFY2(!failure.contains(QStringLiteral("moving desktop")), qPrintable(failure));
    // And a failed request is not announced as a reconciliation either.
    QVERIFY(describeReconciliation(QStringLiteral("the window snapshot"), outcome).isEmpty());
}

void SnapshotReconcileTest::aReconciledRunSaysWhatItReconciledAbout() {
    // The report is the only evidence that a race happened rather than being suspected, so its shape is
    // pinned: silent when the first reading agreed, and naming the count and the disagreement when it did
    // not. A failure must not be described as a reconciliation.
    ScriptedCompositor agreement({QString()});
    const ReconcileOutcome quiet = reconcile(agreement.reader(), impatientBudgetMs, quickSettleMs);
    QVERIFY(describeReconciliation(QStringLiteral("windows"), quiet).isEmpty());

    ScriptedCompositor failure({QStringLiteral("still wrong")});
    const ReconcileOutcome failed = reconcile(failure.reader(), impatientBudgetMs, quickSettleMs);
    QVERIFY(!failed.agreed);
    // A failure is described by describeFailure and must not also be announced as a reconciliation: a log
    // that said both would leave a reader unable to tell a race that was survived from a test that failed.
    QVERIFY2(describeReconciliation(QStringLiteral("windows"), failed).isEmpty(),
             "a comparison that never agreed reported itself as a reconciliation");
    QVERIFY(!describeFailure(QStringLiteral("windows"), failed).isEmpty());
}

void SnapshotReconcileTest::describesBothSidesOfAFieldAndTheAbsentOne() {
    // A disagreement that did not name both sides would send a reader to the compositor to find out what
    // the test was comparing, so both are rendered — and a null on the wire is told apart from a value
    // that merely looks empty.
    Disagreement disagreement;
    disagreement.expectEqual(QStringLiteral("window 3 is_focused"), false, true);
    QVERIFY(!disagreement.empty());
    QVERIFY2(disagreement.text().contains(QStringLiteral("false")), qPrintable(disagreement.text()));
    QVERIFY2(disagreement.text().contains(QStringLiteral("true")), qPrintable(disagreement.text()));
    QVERIFY(disagreement.text().contains(QStringLiteral("is_focused")));

    // Equal values are not a disagreement at all, and the first one found is not replaced by a later one.
    Disagreement agreeing;
    agreeing.expectEqual(QStringLiteral("title"), QStringLiteral("Terminal"), QStringLiteral("Terminal"));
    QVERIFY(agreeing.empty());
    agreeing.expectEqual(QStringLiteral("app_id"), QStringLiteral("kitty"), QStringLiteral("foot"));
    agreeing.expectEqual(QStringLiteral("pid"), 1, 2);
    QVERIFY(agreeing.text().contains(QStringLiteral("kitty")));
    QVERIFY(!agreeing.text().contains(QStringLiteral("pid")));

    QCOMPARE(describeValue(QStringLiteral("")), QStringLiteral("\"\""));
    QCOMPARE(describeValue(QStringList{QStringLiteral("us"), QStringLiteral("de")}),
             QStringLiteral("[\"us\", \"de\"]"));
    QCOMPARE(describeValue(std::optional<quint64>{}), QStringLiteral("absent (null on the wire)"));
    QCOMPARE(describeValue(std::optional<quint64>(7)), QStringLiteral("7"));
    // The fallback is loud rather than blank: a value nobody taught this to render must not look empty.
    QCOMPARE(describeValue(QVariant(1)), QStringLiteral("(a value with no rendering)"));
}

void SnapshotReconcileTest::namesTheTwoSidesTheCallerNamed() {
    // What the two sides are called depends on who is comparing — a model fed by the event stream, or the
    // shell's own report of that model read back over IPC — and a failure that called the second of those "the
    // model" would send a reader to the wrong process. So the names travel with the run, and the default is
    // the event-stream case, which is what every earlier slot's message is written in.
    Disagreement defaulted;
    defaulted.expectEqual(QStringLiteral("the title"), QStringLiteral("A"), QStringLiteral("B"));
    QVERIFY2(defaulted.text().startsWith(QStringLiteral("the title: the model holds")),
             qPrintable(defaulted.text()));
    QVERIFY(defaulted.text().contains(QStringLiteral("the compositor reports")));

    Disagreement named(QStringLiteral("the shell"), QStringLiteral("the compositor"));
    named.expectEqual(QStringLiteral("the workspaces"), QStringList{QStringLiteral("3")},
                      QStringList{QStringLiteral("4")});
    QVERIFY2(named.text().contains(QStringLiteral("the shell holds")), qPrintable(named.text()));
    QVERIFY(!named.text().contains(QStringLiteral("the model")));

    // The names reach the reconciliation's own text too, which is where a reader of a failed run meets them.
    ScriptedCompositor compositor({QStringLiteral("the workspaces: 3 against 4")});
    const ReconcileOutcome outcome = reconcile(compositor.reader(), impatientBudgetMs, quickSettleMs,
                                               qstest::Sides{QStringLiteral("the shell"),
                                                             QStringLiteral("the compositor")});
    const QString failure = describeFailure(QStringLiteral("the workspaces"), outcome);
    QVERIFY2(failure.startsWith(QStringLiteral("the shell and the compositor disagreed")),
             qPrintable(failure));
    QVERIFY(!failure.contains(QStringLiteral("the model")));

    // And the defaulted text is unchanged: this is a renaming, not a rewording of the usual case.
    ScriptedCompositor usual({QStringLiteral("still wrong")});
    const ReconcileOutcome defaultedRun = reconcile(usual.reader(), impatientBudgetMs, quickSettleMs);
    const QString defaultFailure = describeFailure(QStringLiteral("the title"), defaultedRun);
    QVERIFY2(defaultFailure.startsWith(QStringLiteral("the model and the compositor disagreed")),
             qPrintable(defaultFailure));
    QVERIFY(defaultFailure.contains(QStringLiteral("the model has missed an event")));
}

QTEST_GUILESS_MAIN(SnapshotReconcileTest)
#include "snapshot_reconcile_test.moc"
