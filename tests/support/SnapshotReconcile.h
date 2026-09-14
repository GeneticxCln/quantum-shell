// Reconciling the event-fed model against a reading of the compositor taken at a different moment.
//
// The live tests compare two independent paths to one truth on the same running compositor: the model,
// built from the event stream, and a reply to a direct request. They agreeing is evidence and
// disagreeing is a real bug — but only when the desktop has held still between the two readings. A
// window renamed, a workspace focused or a layout switched between the model's last event and the
// request's reply makes them disagree while both are correct, and the assertion cannot tell that apart
// from a model that missed an event. That is not hypothetical: it is what failed this suite's shuffled
// slot-order check, which re-reads the desktop thousands of times and so met the window often enough to
// matter.
//
// Two ways the readings can straddle a change, and they are worth telling apart because only one of them
// says anything about the model:
//
//   * the compositor was overtaken by the model — the reply was composed before a change the model has
//     already been told about. The reply is simply an older reading than the model; asking again gives a
//     reply that agrees.
//   * the model is behind the compositor — the change is in the reply but its event has not been applied
//     yet. Waiting gives the stream time to deliver it, and the model catches up.
//
// Neither is a defect, and a defect — a model that has genuinely missed an event — looks like the second
// one and never resolves. What separates them is therefore not a guess about which side is stale but the
// only thing that can be established from outside: whether the two readings keep disagreeing no matter
// how often both sides are re-read. This is that test, applied to fresh readings until they agree:
//
//   * they agree — pass, and note whether that took more than one reading, because a run that needed
//     several was reading a desktop that was moving and that is worth seeing in a log.
//   * they never agree — fail, and say which of the two the compositor's own readings support, by
//     comparing what the compositor said on the last two attempts. That is why a disagreement always gets
//     a second reading whatever the budget, and why `readingsSettled()` means something whenever there was
//     anything to compare:
//       - the compositor's readings were *still changing* between attempts, so the desktop never settled
//         and there was no stable reading for the model to match. The model may have been keeping up
//         perfectly well; this is the desktop, and a run that ends here should be read as such.
//       - the compositor's readings were *identical* across attempts while the model disagreed, so a
//         stable reading existed and the model did not reach it. That is the signature of a model that
//         has missed an event and is the case worth failing loudly.
//
// This is deliberately not a retry-until-green. Exhausting the budget fails the test, and the failure
// names the first disagreement, the last, and which of the two situations above the readings support. A
// comparison that returned agreement to get out of trouble would show up here as a test that passes with
// the model's event handling removed, which is exactly what `snapshot-reconcile-test` and the live
// falsification of the window comparison check.
//
// What this does not fix, stated plainly because a later reader will otherwise assume it is closed: a
// live comparison can still be fooled by a change that is later superseded. A model that dropped one
// event but received a later event overwriting the same field will hold the compositor's current value
// and agree — the original, non-retrying comparison had the same hole, and no comparison against a
// moving desktop can close it. What this does change is that it is no longer possible to *pass* without
// the two sides agreeing at some point.
#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QTest>

#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace qstest {

// --- saying what a value is ------------------------------------------------------------------------
//
// A disagreement is only useful if it names both sides, so a value has to be rendered into the message.
// These cover the types the live comparisons hold, and the fallback is deliberately loud rather than
// blank: a value nobody taught this to render should not look like a value that is empty.

template <typename T>
QString describeValue(const T& value)
{
    using Plain = std::decay_t<T>;
    if constexpr (std::is_same_v<Plain, QString>) {
        return QStringLiteral("\"%1\"").arg(value);
    } else if constexpr (std::is_same_v<Plain, bool>) {
        return value ? QStringLiteral("true") : QStringLiteral("false");
    } else if constexpr (std::is_arithmetic_v<Plain>) {
        return QString::number(value);
    } else {
        return QStringLiteral("(a value with no rendering)");
    }
}

// A non-template overload for the one container the comparisons hold, chosen over the template above
// because an exact match beats a deduced one.
inline QString describeValue(const QStringList& value)
{
    QStringList rendered;
    for (const QString& each : value) {
        rendered.append(describeValue(each));
    }
    return QLatin1Char('[') + rendered.join(QStringLiteral(", ")) + QLatin1Char(']');
}

// niri spells "no workspace", "no output" and "no mode" as JSON null, and the model spells the same
// thing as an absent optional. Printing them as `absent (null on the wire)` says which of the two sides
// was null rather than leaving a bare "no value" that reads like a bug in the test.
template <typename T>
QString describeValue(const std::optional<T>& value)
{
    return value.has_value() ? describeValue(*value) : QStringLiteral("absent (null on the wire)");
}

// --- one comparison's result ----------------------------------------------------------------------
//
// Collects rather than fails, because a comparison that is going to be taken again must not mark the
// test failed on the first disagreement. The first disagreement is the one kept: a later attempt is
// reading a desktop that has moved again, and the first is the one that describes what was wrong when
// the two sides were first asked.
class Disagreement {
public:
    // The two sides are named as the failure text should name them, because what they are called depends on
    // who is comparing: a model fed by the event stream against the compositor, or the shell's own report of
    // that model read back over IPC against the compositor. The defaults are the first of those, so the common
    // case passes nothing.
    Disagreement(QString left = QStringLiteral("the model"),
                 QString right = QStringLiteral("the compositor"))
        : left_(std::move(left))
        , right_(std::move(right))
    {
    }

    bool empty() const { return text_.isEmpty(); }
    QString text() const { return text_; }

    // The field's name, and both sides of it.
    template <typename ModelValue, typename WireValue>
    void expectEqual(const QString& what, const ModelValue& model, const WireValue& wire)
    {
        if (!text_.isEmpty() || model == wire) {
            return;
        }
        text_ = QStringLiteral("%1: %2 holds %3, %4 reports %5")
                    .arg(what, left_, describeValue(model), right_, describeValue(wire));
    }

    // For the assertions that are not an equality: a count that must be exactly one, a set that must not
    // be empty, a value that must be present.
    void fail(const QString& what)
    {
        if (text_.isEmpty()) {
            text_ = what;
        }
    }

    void check(const QString& what, bool holds)
    {
        if (!holds) {
            fail(what);
        }
    }

private:
    QString left_;
    QString right_;
    QString text_;
};

// --- reading the compositor, and reconciling -------------------------------------------------------

// One reading of the compositor and what the model made of it. `stamp` fingerprints the reading itself —
// the compositor's own words, not the model's — so two attempts can be told apart without this header
// knowing what was read; two identical stamps mean the compositor answered the same thing twice.
struct Reading {
    QString stamp;
    QString mismatch;  // empty when the model agreed with this reading
    // Set when there was no reading to compare against at all: the request was never answered, or the
    // compositor refused it. That is not a race and taking it again would not be a second reading — a
    // timeout already waited — so the loop stops on it and the failure reports the request instead of
    // diagnosing a model it never managed to ask about.
    QString noReading;
};

using TakeReading = std::function<Reading()>;

// The two sides a run compares, as the failure text should name them. Defaulted so that the usual case — a
// model built from the event stream, read against the compositor — passes nothing.
struct Sides {
    QString left = QStringLiteral("the model");
    QString right = QStringLiteral("the compositor");
};

struct ReconcileOutcome {
    Sides sides;
    bool agreed = false;
    // Set when the loop stopped because it could not read the compositor, empty otherwise. When this is
    // set the other fields describe an absence rather than a disagreement.
    QString noReading;
    // How many readings were taken. One means the first agreed.
    int attempts = 0;
    // What the first attempt disagreed about, kept because a later attempt is reading a moved desktop.
    QString firstMismatch;
    QString lastMismatch;
    // The run of identical stamps ending at the last attempt. One or more means the compositor answered
    // the same thing on the last two reads, so a stable reading existed while the model disagreed.
    int identicalReadingsInARow = 0;
    qint64 elapsedMs = 0;

    bool reconciled() const { return attempts > 1; }
    bool readingsSettled() const { return identicalReadingsInARow >= 1; }
    bool readSomething() const { return noReading.isEmpty(); }
};

namespace detail {

// Waits without starving the event loop, because what is being waited for arrives on it: the stream
// applies events on the thread that runs the test, so a sleep that did not process events would stop the
// model from ever catching up and turn every genuine race into a failure. The waiting is the test
// framework's own, so this header introduces no timer of its own — a bounded settle is not one of the
// uses the project allows a timer for, and it does not need one.
inline void settleFor(int milliseconds)
{
    if (milliseconds <= 0) {
        return;
    }
    QTest::qWait(milliseconds);
}

}  // namespace detail

// Takes readings until the model and the compositor agree, or until the budget runs out.
//
// `giveUpAfterMs` bounds the whole thing, because a desktop that is never still must not be able to hang
// a test run — and a bounded loop that fails is what keeps this from being a way to pass without
// agreeing. `settleMs` is how long to let the event loop run between readings, so the model has a chance
// to apply an event that is already on its way.
inline ReconcileOutcome reconcile(const TakeReading& takeReading, int giveUpAfterMs = 2000,
                                  int settleMs = 25, Sides sides = {})
{
    ReconcileOutcome outcome;
    outcome.sides = std::move(sides);
    QElapsedTimer timer;
    timer.start();

    QString previousStamp;
    while (true) {
        Reading reading = takeReading();
        ++outcome.attempts;

        if (!reading.noReading.isEmpty()) {
            outcome.noReading = reading.noReading;
            break;
        }

        if (reading.stamp == previousStamp) {
            ++outcome.identicalReadingsInARow;
        } else {
            outcome.identicalReadingsInARow = 0;
        }
        previousStamp = reading.stamp;

        if (reading.mismatch.isEmpty()) {
            outcome.agreed = true;
            break;
        }
        if (outcome.firstMismatch.isEmpty()) {
            outcome.firstMismatch = reading.mismatch;
        }
        outcome.lastMismatch = reading.mismatch;

        // One reading cannot say which of the two situations this is, because both diagnoses are statements
        // about a pair of readings, so a second is always taken and only the readings after it are the
        // budget's to refuse. Without this, a first reading that disagreed could exhaust the budget on its
        // own — on a loaded machine, easily — and the failure would diagnose a moving desktop on the
        // strength of a single comparison, which is a claim one reading does not support.
        if (outcome.attempts == 1) {
            detail::settleFor(settleMs);
            continue;
        }

        // A further reading is worth taking only if the budget can pay for it: the request it needs costs
        // a round trip, and without one the loop would be waiting on nothing.
        if (timer.elapsed() + settleMs > giveUpAfterMs) {
            break;
        }
        detail::settleFor(settleMs);
        if (timer.elapsed() > giveUpAfterMs) {
            break;
        }
    }

    outcome.elapsedMs = timer.elapsed();
    return outcome;
}

// The failure text for an outcome that did not agree, written once so every comparison that uses this
// says the same thing and a reader does not have to work out which of the two situations it was.
inline QString describeFailure(const QString& what, const ReconcileOutcome& outcome)
{
    QStringList lines;
    if (!outcome.readSomething()) {
        // Said plainly rather than dressed as a disagreement: the comparison never happened, and the
        // transport failure is the whole of the finding.
        lines.append(QStringLiteral("%1 could not be compared: %2").arg(what, outcome.noReading));
        return lines.join(QStringLiteral("; "));
    }
    lines.append(QStringLiteral("%1 and %2 disagreed about %3 on all %4 reading(s) taken over %5 ms")
                     .arg(outcome.sides.left, outcome.sides.right, what)
                     .arg(outcome.attempts)
                     .arg(outcome.elapsedMs));
    if (outcome.readingsSettled()) {
        lines.append(QStringLiteral("%1 answered identically on the last %2 reading(s) while %3 "
                                    "disagreed, so a stable reading existed and %3 did not reach it: %3 "
                                    "has missed an event")
                         .arg(outcome.sides.right)
                         .arg(outcome.identicalReadingsInARow + 1)
                         .arg(outcome.sides.left));
    } else {
        lines.append(QStringLiteral("%1's own answer was still changing between readings, so the desktop "
                                    "never held still and there was no stable reading for %2 to match — "
                                    "read this as a moving desktop rather than as %2 having missed an event")
                         .arg(outcome.sides.right, outcome.sides.left));
    }
    lines.append(QStringLiteral("first disagreement: %1").arg(outcome.firstMismatch));
    lines.append(QStringLiteral("last disagreement: %1").arg(outcome.lastMismatch));
    return lines.join(QStringLiteral("; "));
}

// What a run that needed more than one reading should say for itself. Silent when the first reading
// agreed, so this costs nothing in the common case and names a race that did happen in the rare one.
inline QString describeReconciliation(const QString& what, const ReconcileOutcome& outcome)
{
    if (!outcome.agreed || !outcome.reconciled()) {
        return {};
    }
    return QStringLiteral("%1: %2 and %3 disagreed when first asked and agreed after %4 reading(s) over "
                          "%5 ms, which is the desktop changing between the two readings rather than a "
                          "defect; the first disagreement was: %6")
        .arg(what, outcome.sides.left, outcome.sides.right)
        .arg(outcome.attempts)
        .arg(outcome.elapsedMs)
        .arg(outcome.firstMismatch);
}

}  // namespace qstest
