# Runs each test binary's slots in a random order, so that a slot which depends on state an earlier slot
# left behind fails here instead of passing because the file happens to declare them in a working order.
#
# This is the coverage amplifier for run_reordered.cmake, not a replacement for it: that one is
# deterministic — every slot alone, and the whole list reversed — so it can be trusted run after run,
# while this one explores orders neither of those two happens to use. Reverse order catches the common
# case, where a slot depends on the one declared before it; a shuffle also catches the rarer ones, a
# three-slot chain or a dependency on a slot declared somewhere unrelated.
#
# Reproducible on purpose, because a test that fails at random and cannot be repeated is a test nobody
# acts on. One seed drives every binary of a run, it is derived into a seed per binary per pass and per
# shard, and every one of them is printed — on success at STATUS level, and in the failure text. Pinning
# it re-runs the same orders, in every shard at once:
#
#     QS_TEST_ORDER_SEED=123456 ctest --preset dev -R slot-order-randomised
#
# The order is a function of the seed, of the shard, and of the list of binaries, so reproduce a failure
# on the same revision. The shuffle is a linear congruential generator and a selection shuffle, both in
# CMake arithmetic: the same seed gives the same orders on any machine and in any CMake run, which
# shelling out to a language with a shuffle with its own version of it would not.
#
# --- why a run is several shards ---------------------------------------------------------------------
#
# The work is split by binary, not by pass: each shard is given a share of the binaries and orders each of
# them for the whole pass count. That split is the deliberate one. What this check measures — how the
# slots of one binary interact — is a property of that binary's own orderings, so all of a binary's
# passes belong in one process. Sharding by pass instead would put part of every measurement in one shard
# and part in another, leaving each shard able to report only a fraction of a figure whose whole is the
# union of shards, which no shard can see. Split by binary, every figure a shard prints is the figure for
# the binaries it holds, and the run's figures are those added up.
#
# The shards are separate ctest tests, so ctest runs them at once (`-j`, which the test presets set): four
# shards on four cores cost the wall clock of the slowest rather than the sum, and that saving is what
# pays for the pass count below. Which binaries land in which shard is decided in tests/CMakeLists.txt
# from a measured per-binary pass cost, because the binaries are far from equally expensive —
# niri-ipc-test takes about 0.9 s for a pass and niri-state-test about 0.01 s — and an unbalanced split
# gives the saved wall clock straight back.
#
# Nothing here needs to know how many shards there are. It needs its index and the count for one reason:
# so that it draws a sequence of its own instead of replaying a sibling's, and so that what it prints says
# which part of the run it is.
#
# --- how many passes, and what they buy --------------------------------------------------------------
#
# A binary of n slots has n! orderings, so the share of them any run visits is vanishing: the passes visit
# a fraction of the twenty-one-slot binary's orderings that is below what this report can write down. That
# number says nothing useful about whether the check is any good, and it is not the figure to decide with.
# What matters is the relative order of a few slots at once, in three measures:
#
#   pairs    An order dependence from one slot to another shows up only when the two land the wrong way
#            round, so what a pass buys is pairs exercised in *both* orders. A pair is covered once both
#            directions have appeared: 1 - 2^(1-k) after k uniform passes, so 75.0% at three, 99.2% at
#            eight and 99.95% at twelve.
#   triples  A dependency that runs through three slots — one sets up state, another reads it, a third
#            clears it — needs a particular one of the six relative orders of those three. Six orders are
#            far more than two, so this saturates much later: the coupon-collector sum below gives 43.8%
#            of triples with all six orders after twelve passes, 92.5% after twenty-four and 99.15% after
#            thirty-six. Triples are what makes this check expensive, and what the pass count below was
#            raised for: at ninety-six they come out complete, and at half that they would not.
#   four     A dependency that needs four slots in a particular relative order — one arranges state, one
#            reads it, one asserts on it and a fourth clears it up — needs one of twenty-four orders.
#            This is the measure the other two cannot give, and the reason it is worth having: those four
#            slots can hold any three of themselves in every order there is while the four are still
#            wrong, because which of the six orders a triple of them takes does not say which of the
#            twenty-four the four take. A run whose pairs and triples are both complete therefore says
#            nothing about it, and it is the one measure here that does **not** come out complete at the
#            default pass count — see the section below.
#
# Ninety-six is the count a run uses, and it is not written down anywhere: it is what the default request
# of QS_TEST_ORDER_COVERAGE=98.3 derives. The request is a share of the four-slot interaction space, because
# that measure keeps improving with every pass where the pair share is finished by twelve and the triple
# share by about ninety — so it is the figure that can say what another pass buys — and the smallest count
# whose *printed* model reaches the request is the count that runs. Every run prints the derivation, so it
# can be checked against the model lines rather than taken on trust: at the default, "the smallest count
# whose model reaches the requested 98.3% of the four-slot interaction space, 98.24% being what 95 passes
# give". QS_TEST_ORDER_PASSES still wins when it is given, because reproducing a printed seed or timing a
# change means wanting the count that was asked for, and the run then reports which count its request would
# have derived instead.
#
# At ninety-six the triples stop being the thing to read: the expected number of triples left short of
# their six orders, over every triple in the suite — 3545 of them, each short with probability about
# 6 * (5/6)^k — is roughly 268 at twenty-four passes, 3.4 at forty-eight, and 0.0005 at ninety-six. Measured
# at ninety-six over four seeds, every one of those 3545 triples came out with all six orders and every one
# of the 763 pairs in both, so the section that names the short triples is empty because there are none. The
# four-slot measure is the opposite case, and that is what it is for: no practical run completes it, so the
# names at the end of this report are four-slot subsets rather than triples, and the share it reaches is the
# figure the pass count is derived from rather than one it is compared against afterwards.
#
# Ninety-six is affordable because of the sharding above: each shard orders only its own binaries that many
# times, and the slowest of the four took 110 s here — 99 s before the four-slot measure was added — against
# 116 s for the twenty-four-pass check the sharding replaced, and against the 900 s timeout each shard is
# registered with. The request cannot be for the whole space, because the model floors its figure and so
# never returns 100.00%: the ceiling is 99.99%, reached at 264 passes — about 300 s a shard here, still
# inside the timeout — and the highest a request can be, at one decimal place, is 99.9%, which derives 163.
# What the request must not do is go below the pass count the enforced pair floor
# tolerates — at the default floor of 95% that is about 22 passes, a request of about 61% — and a request
# short of that is refused by the floor's own arithmetic, which names this knob and the override when it
# fires.
#
# --- what is asserted, and what is only reported -----------------------------------------------
#
# Pair coverage is **enforced**: a run whose binaries leave more than QS_MIN_PAIR_COVERAGE% of their pairs
# exercised in one order only fails. It can be enforced without flakiness, and the reason is arithmetic
# rather than optimism. A given pair is missed exactly when all k passes agree on its two slots, which
# happens with probability 2^(1-k) — far below any figure this report can write in billionths at the
# default pass count — and with the misses the floor permits, Markov's inequality bounds the chance that a
# correct run falls below it, at below one run in a billion. So at this pass count the pair and triple
# measures both sit at their ceiling in a correct run, and a failure is a broken mechanism rather than an
# unlucky seed.
#
# That bound depends on the pass count, and the check knows it: a combination whose bound is worse than
# one run in a hundred thousand is **refused before anything runs**, with the arithmetic in the message.
# At six passes a 95% floor would fail a correct run more than half the time, and refusing that is the
# whole difference between an enforced minimum and a flaky test. QS_MIN_PAIR_COVERAGE=0 reports coverage
# without asserting it, for a smoke run short on time.
#
# Triple completeness and the share of orderings stay **reported**. The triples are not the figure a
# floor would add anything to here: they are already at their ceiling in a correct run, and it is the floor
# above that catches the mechanism if they stop being. What a following run acts on from the triple side is
# the count and the names printed at the end, not a threshold. What is asserted about the shuffle itself is
# below, on fixed seeds.
#
# --- the four-slot measure, and why it is reported rather than enforced ------------------------------
#
# The suite's twelve binaries have 13,373 four-slot subsets between them — C(n,4) summed over their slot
# counts, 4 to 21 — and twenty-four relative orders each, which is 320,952 orderings. Both figures are
# printed per binary and as a total, with the model for a uniformly drawn shuffle of the same length beside
# them: at the default ninety-six passes that model says a run reaches 98.31% of the orders and exercises
# 65.63% of the subsets in all twenty-four of theirs (the exact expectation for the second is 65.6356%,
# truncated to what the report prints), and what a run on this checkout actually reached was 98.07% and
# 61.38%. An order is missed exactly when all k passes avoid it, which has probability (23/24)^k, and a
# subset has all twenty-four when it collects every order, which is the coupon-collector recursion below.
#
# Both measured figures land below that model, by a couple of tenths on the share and about four points on
# the count complete, and it is worth knowing why before reading the gap as a defect in the recording. The model is the expectation for passes that draw
# each of a subset's twenty-four orders uniformly; this shuffle's draws are close to uniform but not exactly
# so, and by Jensen's inequality any deviation from uniform lowers the expected number of distinct orders —
# 1 - (1-p)^k is concave in p, so spreading the same total probability over unequal orders can only lose.
# Measured by replaying this script's own shuffle outside it: over twenty-four seeds the twenty-one-slot
# binary came out at 141,044 distinct four-slot orders on average, spread 223 between seeds, against the
# uniform model's 141,225 — and the run recorded 141,054, inside that spread; over eight seeds its subsets
# complete came out 3,788 against the model's 3,928, and the run recorded 3,819. So the recording and
# reading sides are counting what they claim to, and the leftover gap belongs to the shuffle.
#
# Replaying it also shows how far from uniform the shuffle is, which no pair or triple figure above could
# reveal because both saturate whether the orders are uniform or not: for one four-slot subset the
# twenty-four orders came out with probabilities between 0.028 and 0.084 rather than 1/24 = 0.042 — a factor
# of three — and the pair directions of a subset between 0.451 and 0.549 rather than 0.5, the same whether
# the seeds are drawn independently or taken from the sequence this check itself uses. That is a finding
# about the shuffle rather than about this measure, and it is recorded as an open lead in QUANTUM_SHELL.md.
#
# Which subsets come out worst is mostly the seed rather than the shuffle: over those twenty-four seeds the
# per-subset mean orders seen ranged from 22.9 to 23.9 of 24, but the correlation between one half of the
# seeds and the other was 0.09 and none of the hundred worst subsets was short in all twenty-four runs. A
# name in the report below is therefore a lead for a following run, not a permanent property of that subset.
#
# The contrast with the two measures above is the reason to print it at all. Pairs and triples both sit at
# their ceiling in a correct run here, so a reader would otherwise take "interaction coverage: complete" to
# mean the interactions are covered. At four slots at a time they are not: about a third of the subsets are
# short of an order, a few hundred at the default are short of two, and the worst are short of three or
# four. The honest figures are the share reached, the count complete, and the names of the subsets that
# came off worst.
#
# It is reported and not enforced. A floor on the aggregate share would be met comfortably — the model
# predicts 98.31% and the measurement lands within a couple of tenths of it — but the pair floor is
# enforceable because Markov's inequality bounds the chance that a correct run falls below it, and that bound
# needs the misses counted against an allowance. Counting four-slot misses that way would need the correlation between them, since
# one permutation of twenty-one slots decides the order of every subset it contains at once, and the
# figure is a share of a space no practical pass count completes anyway: for the subsets to come out
# complete about half the time needs 85 passes, 95% of them needs 145, and 99.9% needs 237, against 96 for
# a check that already takes about 105 s. So the four-slot figure is a report, and QS_WORST_QUADS_SHOWN is
# what a following run reads out of it.
#
# What it costs: recording every four-slot order is the expensive part, because there are four times as
# many four-slot subsets as triples — 574,560 records for the twenty-one-slot binary over ninety-six
# passes, measured at 7.5 s, and reading them back enumerates each subset's twenty-four orders in under
# half a second. Across the suite that is about 8 s on the slowest shard.
#
# --- naming the triples the passes left short ---------------------------------------------------------
#
# A count of incomplete triples says how much of the three-slot interaction space was reached; a name says
# where to look, which is the part a following run can act on. So the run ends by writing the ones left
# short out: least explored first, so a triple that saw two of its six relative orders is read before one
# that saw five, up to QS_WORST_TRIPLES_SHOWN of them per binary (default 10; 0 keeps the counts and drops
# the names, for a report going somewhere other than a person's eyes). Each is named as the three slots in
# declared order — the order the pass lines above shuffle and the seed reproduces — on a line of its own,
# labelled with how many of the six orders it was exercised in, because a slot name in this suite is a
# sentence and a comma-joined list of them is a paragraph.
#
# The four-slot subsets come out the same way, up to QS_WORST_QUADS_SHOWN of them per binary, and unlike
# the triple names this section is never empty at the default: where the triples are complete, about a
# third of the four-slot subsets are short of all twenty-four of their orders. A four-slot name is four
# slots in declared order, which is long, and the count it carries is what says whether it is worth
# reading; the ones short by three or four orders are the leads, and the ones short by one are the bulk.
#
# Inputs: TESTS (the binaries this shard holds, joined with `|`), optional SEED, optional COVERAGE (the
# share of the four-slot interaction space to derive the pass count from, default 98.3%), optional PASSES
# (an explicit count, which wins over the derivation), optional SHARD_INDEX (default 1) and SHARD_COUNT
# (default 1), optional MIN_PAIR_COVERAGE (default 95, 0 to report without asserting), optional
# WORST_TRIPLES_SHOWN (default 10, 0 to name none), optional WORST_QUADS_SHOWN (default 10, 0 to name none).
if(NOT DEFINED TESTS)
    message(FATAL_ERROR "run_shuffled_order.cmake requires TESTS (the Qt test binaries to check)")
endif()

string(REPLACE "|" ";" TESTS "${TESTS}")
include("${CMAKE_CURRENT_LIST_DIR}/slot_list.cmake")

# A random default seed, printed either way, so the run is reproducible even when nobody chose it. It is
# drawn over the widest range the generator below can take rather than over the digits a person would
# type: that recurrence multiplies the state by 1103515245, and the product has to stay inside a signed
# 64-bit integer for CMake's arithmetic to be exact, which needs the state under 2^31 — 2^31 * 1103515245
# is about 2.4e18, comfortably inside 9.2e18. Ten random digits reduced modulo 2^31 - 1 uses that whole
# range, which is 23 times the space an eight-digit seed drew from.
#
# The draw is split in two because CMake reads a number with a leading zero as octal, so ten digits drawn
# from plain digits could turn "0123456789" into something else entirely. The leading digit comes from an
# alphabet with no zero in it, which keeps the literal decimal whatever follows it.
if(NOT DEFINED SEED OR SEED STREQUAL "")
    set(SEED "$ENV{QS_TEST_ORDER_SEED}")
endif()
if(NOT DEFINED SEED OR SEED STREQUAL "")
    string(RANDOM LENGTH 1 ALPHABET "123456789" leading)
    string(RANDOM LENGTH 9 ALPHABET "0123456789" trailing)
    math(EXPR SEED "(${leading}${trailing}) % 2147483647 + 1")
endif()

# A seed outside the range the generator is exact over is refused rather than quietly producing a
# different kind of sequence: the whole reproducibility story above rests on that arithmetic being exact.
# The shape is checked first, because a non-numeric seed would otherwise fail as a parse error rather than
# as this message.
if(NOT SEED MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "QS_TEST_ORDER_SEED must be a whole number written without a leading zero, got '${SEED}'")
endif()
if(SEED GREATER 2147483647)
    message(FATAL_ERROR
        "QS_TEST_ORDER_SEED must be at most 2147483647: the shuffle multiplies the seed by 1103515245, "
        "and above that the product leaves the range CMake arithmetic is exact in")
endif()

# Which shard of the run this is. These two numbers do not divide any work here — the shard's binaries
# arrive already chosen, and it orders all of them for the full pass count. They exist so that sibling
# shards draw different sequences rather than replaying one between them, and so that what this run prints
# says which part of the check it is. One of one is the default, and is exactly the run this script made
# before it was sharded.
if(NOT DEFINED SHARD_INDEX OR SHARD_INDEX STREQUAL "")
    set(SHARD_INDEX 1)
endif()
if(NOT DEFINED SHARD_COUNT OR SHARD_COUNT STREQUAL "")
    set(SHARD_COUNT 1)
endif()
if(NOT SHARD_INDEX MATCHES "^[1-9][0-9]*$" OR NOT SHARD_COUNT MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "SHARD_INDEX and SHARD_COUNT must be whole numbers of at least 1, got '${SHARD_INDEX}' of "
        "'${SHARD_COUNT}'")
endif()
if(SHARD_INDEX GREATER SHARD_COUNT)
    message(FATAL_ERROR
        "SHARD_INDEX is ${SHARD_INDEX}, which is past the end of a run of ${SHARD_COUNT} shards")
endif()
if(SHARD_COUNT GREATER 1)
    set(qs_shard_phrase " in shard ${SHARD_INDEX} of ${SHARD_COUNT}")
else()
    set(qs_shard_phrase "")
endif()

# The shard index is folded into the stream by taking that many steps of the same recurrence, so a shard's
# orders are still determined by the one seed the run was given — which is what makes a printed seed
# reproduce every shard rather than only the one it came from — while its sequence is not a sibling's.
# Shard one takes no steps, so a run of one shard is unchanged by any of this.
set(stream_start "${SEED}")
math(EXPR qs_shard_steps "${SHARD_INDEX} - 1")
math(EXPR qs_shard_step "1")
while(qs_shard_step LESS_EQUAL qs_shard_steps)
    math(EXPR stream_start "(${stream_start} * 1103515245 + 12345) % 2147483648")
    math(EXPR qs_shard_step "${qs_shard_step} + 1")
endwhile()

# The pass count is derived rather than fixed, and the thing it is derived from is a share of the
# four-slot interaction space: that measure improves with every pass, where the pair share is finished by
# twelve and the triple share by about ninety, so it is the one that can say what another pass buys. The
# count that runs is the smallest one whose *printed* model reaches the request — the same truncating
# arithmetic the report shows beside every measured figure — so the derivation is auditable by reading two
# lines of a run rather than by trusting an ideal no run ever prints. An explicit QS_TEST_ORDER_PASSES still
# wins, because a run reproducing a printed seed or timing a change wants the count it asked for, and the
# run says which count its request would have derived instead. The old fixed default of ninety-six is the
# request below that derives it.
if(NOT DEFINED PASSES OR PASSES STREQUAL "")
    set(PASSES "$ENV{QS_TEST_ORDER_PASSES}")
endif()
if(NOT PASSES STREQUAL "" AND NOT PASSES MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "QS_TEST_ORDER_PASSES must be a whole number of at least 1, got '${PASSES}'")
endif()

# The share of the four-slot interaction space the run is asked to reach, as a percentage with at most one
# decimal place. It is parsed by hand because math(EXPR) is integer-only and a percentage has a point in it.
if(NOT DEFINED COVERAGE OR COVERAGE STREQUAL "")
    set(COVERAGE "$ENV{QS_TEST_ORDER_COVERAGE}")
endif()
if(NOT DEFINED COVERAGE OR COVERAGE STREQUAL "")
    set(COVERAGE 98.3)
endif()
if(NOT COVERAGE MATCHES "^[0-9]+([.][0-9])?$")
    message(FATAL_ERROR
        "QS_TEST_ORDER_COVERAGE must be a percentage written with at most one decimal place, got "
        "'${COVERAGE}': it is the share of the four-slot interaction space the pass count is derived from")
endif()
string(FIND "${COVERAGE}" "." qs_cover_point)
if(qs_cover_point EQUAL -1)
    math(EXPR coverage_units "${COVERAGE} * 100")
else()
    string(SUBSTRING "${COVERAGE}" 0 ${qs_cover_point} qs_cover_whole)
    math(EXPR qs_cover_start "${qs_cover_point} + 1")
    string(SUBSTRING "${COVERAGE}" ${qs_cover_start} -1 qs_cover_tenth)
    math(EXPR coverage_units "${qs_cover_whole} * 100 + ${qs_cover_tenth} * 10")
endif()
if(coverage_units LESS_EQUAL 0 OR coverage_units GREATER 10000)
    message(FATAL_ERROR
        "QS_TEST_ORDER_COVERAGE is ${COVERAGE}%, which is not a share of a space: ask for something above "
        "0 and at most 100")
endif()

# The share of slot pairs that must have been exercised in both orders. Zero is allowed and means the
# coverage is reported without being asserted.
if(NOT DEFINED MIN_PAIR_COVERAGE OR MIN_PAIR_COVERAGE STREQUAL "")
    set(MIN_PAIR_COVERAGE "$ENV{QS_MIN_PAIR_COVERAGE}")
endif()
if(NOT DEFINED MIN_PAIR_COVERAGE OR MIN_PAIR_COVERAGE STREQUAL "")
    set(MIN_PAIR_COVERAGE 95)
endif()
if(NOT MIN_PAIR_COVERAGE MATCHES "^[0-9]+$" OR MIN_PAIR_COVERAGE GREATER 100)
    message(FATAL_ERROR
        "QS_MIN_PAIR_COVERAGE must be a whole number of percent from 0 to 100, got '${MIN_PAIR_COVERAGE}'")
endif()

# How many of the least-covered triples to name per binary in the report at the end of the run. Zero
# keeps the counts and drops the names.
if(NOT DEFINED WORST_TRIPLES_SHOWN OR WORST_TRIPLES_SHOWN STREQUAL "")
    set(WORST_TRIPLES_SHOWN "$ENV{QS_WORST_TRIPLES_SHOWN}")
endif()
if(NOT DEFINED WORST_TRIPLES_SHOWN OR WORST_TRIPLES_SHOWN STREQUAL "")
    set(WORST_TRIPLES_SHOWN 10)
endif()
if(NOT WORST_TRIPLES_SHOWN MATCHES "^[0-9]+$")
    message(FATAL_ERROR
        "QS_WORST_TRIPLES_SHOWN must be a whole number from 0 up, got '${WORST_TRIPLES_SHOWN}'")
endif()

# How many of the least-covered four-slot subsets to name per binary in the report at the end of the run.
# Zero keeps the counts and drops the names.
if(NOT DEFINED WORST_QUADS_SHOWN OR WORST_QUADS_SHOWN STREQUAL "")
    set(WORST_QUADS_SHOWN "$ENV{QS_WORST_QUADS_SHOWN}")
endif()
if(NOT DEFINED WORST_QUADS_SHOWN OR WORST_QUADS_SHOWN STREQUAL "")
    set(WORST_QUADS_SHOWN 10)
endif()
if(NOT WORST_QUADS_SHOWN MATCHES "^[0-9]+$")
    message(FATAL_ERROR
        "QS_WORST_QUADS_SHOWN must be a whole number from 0 up, got '${WORST_QUADS_SHOWN}'")
endif()

# The worst chance of a correct run failing the floor that will be accepted, in billionths: one run in a
# hundred thousand. A floor that cannot meet this is not enforced at all, it is a coin toss with a
# confident-sounding message, so the check refuses the combination instead.
set(QS_FLAKINESS_LIMIT_PER_BILLION 10000)

# x = (1103515245 * x + 12345) mod 2^31. Every product stays inside a signed 64-bit integer, so CMake's
# arithmetic is exact here — no floating point, and nothing that could differ between machines.
function(qs_next_random state_var out_var)
    math(EXPR next "(${${state_var}} * 1103515245 + 12345) % 2147483648")
    set("${state_var}" "${next}" PARENT_SCOPE)
    set("${out_var}" "${next}" PARENT_SCOPE)
endfunction()

# Selection shuffle: repeatedly take one of the remaining slots. Each slot lands in each position with
# equal probability, which is all a test order needs.
function(qs_shuffle list_var seed)
    set(items "${${list_var}}")
    list(LENGTH items count)
    set(state "${seed}")
    set(result "")
    while(count GREATER 0)
        qs_next_random(state draw)
        math(EXPR pick "${draw} % ${count}")
        list(GET items ${pick} chosen)
        list(REMOVE_AT items ${pick})
        list(APPEND result "${chosen}")
        math(EXPR count "${count} - 1")
    endwhile()
    set("${list_var}" "${result}" PARENT_SCOPE)
endfunction()

# numerator/denominator as a percentage in hundredths of a point, in integer arithmetic — CMake has no
# floating point, and a figure being reported rather than compared does not need more than that. Rounded
# to the nearest hundredth, which is as much as the reader can use. Sets `<prefix>_text`.
function(qs_percent numerator denominator prefix)
    if(denominator LESS_EQUAL 0)
        set("${prefix}_text" "n/a" PARENT_SCOPE)
        return()
    endif()
    math(EXPR units "(10000 * ${numerator} + ${denominator} / 2) / ${denominator}")
    math(EXPR whole "${units} / 100")
    math(EXPR fraction "${units} % 100")
    if(fraction LESS 10)
        set(fraction "0${fraction}")
    endif()
    set("${prefix}_text" "${whole}.${fraction}" PARENT_SCOPE)
endfunction()

# Hundredths of a percent — the unit every model below is computed in — as the `whole.fraction` text the
# report writes. Sets `<out_var>`.
function(qs_units_text units out_var)
    math(EXPR whole "${units} / 100")
    math(EXPR fraction "${units} % 100")
    if(fraction LESS 10)
        set(fraction "0${fraction}")
    endif()
    set("${out_var}" "${whole}.${fraction}" PARENT_SCOPE)
endfunction()

# numerator as a share of denominator, in ten-thousandths of a percent, or the honest words for a share
# too small to write that way. Sets `<prefix>_text`.
function(qs_share numerator denominator prefix)
    if(denominator LESS_EQUAL 0)
        set("${prefix}_text" "an unknown share of" PARENT_SCOPE)
        return()
    endif()
    math(EXPR units "1000000 * ${numerator} / ${denominator}")
    if(units LESS 100)
        set("${prefix}_text" "less than 0.01%" PARENT_SCOPE)
        return()
    endif()
    math(EXPR whole "${units} / 10000")
    math(EXPR fraction "${units} % 10000")
    if(fraction LESS 10)
        set(fraction "000${fraction}")
    elseif(fraction LESS 100)
        set(fraction "00${fraction}")
    elseif(fraction LESS 1000)
        set(fraction "0${fraction}")
    endif()
    set("${prefix}_text" "${whole}.${fraction}%" PARENT_SCOPE)
endfunction()

# The share of slot pairs k uniformly drawn passes are expected to cover, as a percentage in hundredths:
# `1 - 2^(1-k)`, because a given pair is missed exactly when all k passes happen to agree on its two
# slots, which has probability 2 * (1/2)^k. Rounded down for the same reason as `qs_percent`: a yardstick
# should not be the flattering one.
#
# The missed share is halved one pass at a time rather than shifted, because `1 << 95` — the exponent at the
# default pass count — does not fit the integer CMake computes arithmetic with: it warns that the shift
# exponent is too large and evaluates to `1 << 31`, which happens to give the same answer here and is not
# arithmetic to rely on. The repeated halving rounds up as it goes, so the missed share is never understated
# and never reaches zero: the model does not claim a pair is certainly covered, at any pass count. Sets
# `qs_model_text`.
function(qs_model_coverage passes)
    set(missed 10000)
    math(EXPR halvings "${passes} - 1")
    math(EXPR step "1")
    while(step LESS_EQUAL halvings)
        math(EXPR missed "(${missed} + 1) / 2")
        math(EXPR step "${step} + 1")
    endwhile()
    math(EXPR units "10000 - ${missed}")
    math(EXPR whole "${units} / 100")
    math(EXPR fraction "${units} % 100")
    if(fraction LESS 10)
        set(fraction "0${fraction}")
    endif()
    set(qs_model_text "${whole}.${fraction}" PARENT_SCOPE)
endfunction()

# The share of subsets expected to have all `orders` of their relative orders exercised after k uniformly
# drawn passes — coupon collecting — as a percentage in hundredths, carried as a distribution over how many
# orders have been seen after each pass.
#
# The inclusion-exclusion sum for this, sum over j of (-1)^j C(orders,j) ((orders-j)/orders)^k, is the short
# formula and is unusable here. Its terms are astronomically larger than its answer, so in integer
# arithmetic the truncation of each power swamps the result: for twenty-four orders it printed 718.67%,
# 327.74% and negative percentages, all of them plausible-looking numbers in a report nobody would think to
# question. Carrying the distribution forward has no cancellation — every term is added, not subtracted —
# and its error grows with the pass count rather than with the binomials, for twenty-five steps a pass.
# Sets `<out_var>`.
function(qs_coupon_model orders passes out_var)
    # dist[m] is the chance that exactly m of the orders have been seen so far, in billionths. Truncating at
    # a billionth keeps twenty-four orders' worth of steps honest to about a millionth of a percent.
    set(dist "1000000000")
    foreach(m RANGE 1 ${orders})
        list(APPEND dist 0)
    endforeach()
    math(EXPR draw "1")
    while(draw LESS_EQUAL passes)
        set(next "")
        set(m 0)
        while(m LESS_EQUAL orders)
            list(GET dist ${m} here)
            # A draw landing on one of the m orders already seen leaves the count where it is ...
            math(EXPR stays "${here} * ${m} / ${orders}")
            # ... and a draw landing on any of the others raises it by one.
            math(EXPR rises "0")
            if(m GREATER 0)
                math(EXPR previous "${m} - 1")
                list(GET dist ${previous} before)
                math(EXPR rises "${before} * (${orders} + 1 - ${m}) / ${orders}")
            endif()
            math(EXPR value "${stays} + ${rises}")
            list(APPEND next "${value}")
            math(EXPR m "${m} + 1")
        endwhile()
        set(dist "${next}")
        math(EXPR draw "${draw} + 1")
    endwhile()
    list(GET dist ${orders} complete)
    # A probability in billionths is hundredths of a percent after dividing by a hundred thousand; the
    # figures are truncated rather than rounded for the same reason as everywhere else here.
    math(EXPR whole "${complete} / 10000000")
    math(EXPR fraction "(${complete} % 10000000) / 100000")
    if(fraction LESS 10)
        set(fraction "0${fraction}")
    endif()
    set("${out_var}" "${whole}.${fraction}" PARENT_SCOPE)
endfunction()

# The share of slot triples expected to have all six of their relative orders exercised after k uniformly
# drawn passes. Sets `qs_triple_model_text`.
function(qs_triple_model passes)
    qs_coupon_model(6 ${passes} qs_triples_complete)
    set(qs_triple_model_text "${qs_triples_complete}" PARENT_SCOPE)
endfunction()

# The share of a four-slot subset's twenty-four relative orders that k uniformly drawn passes are expected
# to have exercised: an order is missed exactly when all k passes avoid it, so the expectation is
# 1 - (23/24)^k, as a percentage in hundredths of a point. Sets `<out_var>` — this is the figure the pass
# count is derived from, so it is a number as well as a piece of text.
#
# The missed share is carried in millionths, one multiplication a pass, and each is **rounded** rather than
# truncated. That is not a detail: rounded, each step's error is under half a unit and the multiplications
# that follow shrink it, so the figure stays within a dozen millionths of the exact one at any pass count,
# and the missed share converges to a fixed floor instead of reaching zero. Truncated, the error is under a
# unit but always in the same direction, and it compounds: the truncating version reached zero at 264 passes
# and reported 100.00% of the four-slot space there, where the exact figure is 99.9987%. That is the
# flattering kind of wrong — it says a space is covered when it is not — and it would have made a request for
# the whole space derivable, so the mistake would have propagated into the pass count rather than stopped at
# a line of output.
#
# Rounding is also what keeps the model honest at the top of its range: the missed share settles at a dozen
# millionths rather than at nothing, so the highest figure this can ever return is 99.99% and no pass count
# claims the space is fully exercised — the same reason the pair model floors its own figure.
function(qs_quad_missed passes out_var)
    set(value 1000000)
    math(EXPR step "1")
    while(step LESS_EQUAL passes)
        math(EXPR value "(${value} * 23 + 12) / 24")
        math(EXPR step "${step} + 1")
    endwhile()
    set("${out_var}" "${value}" PARENT_SCOPE)
endfunction()

# The share exercised, in hundredths of a percent, read off that missed share. Sets `<out_var>`.
function(qs_quad_share_units passes out_var)
    qs_quad_missed(${passes} qs_missed)
    math(EXPR units "10000 - (${qs_missed} + 99) / 100")
    set("${out_var}" "${units}" PARENT_SCOPE)
endfunction()

# The same figure as the report writes it. Sets `qs_quad_share_model_text`.
function(qs_quad_share_model passes)
    qs_quad_share_units(${passes} qs_share_units)
    qs_units_text(${qs_share_units} qs_share_text)
    set(qs_quad_share_model_text "${qs_share_text}" PARENT_SCOPE)
endfunction()

# The share of four-slot subsets expected to have all twenty-four of their relative orders exercised after
# k uniformly drawn passes. Sets `qs_quad_complete_model_text`.
function(qs_quad_complete_model passes)
    qs_coupon_model(24 ${passes} qs_quads_complete)
    set(qs_quad_complete_model_text "${qs_quads_complete}" PARENT_SCOPE)
endfunction()

# --- the pass count, which is the request's consequence rather than a constant -------------------------
#
# Two walks, both over the same model the report prints. The first finds the **ceiling**, the count where
# the missed share reaches its fixed point: that is the highest figure there is to ask for and the bound the
# second walk stops at, so there is no written-down bound here to go stale. The second finds the smallest
# count whose figure reaches the request, and that is the count that runs — the share rises with the pass
# count, since each pass can only add orders to the ones already seen, so the first count that clears the
# request is the smallest one that does, and the figure it was compared against is the figure the report
# prints beside what actually ran. Both walks together cost about a third of a second, once a run. The
# ceiling is 99.99% at 264 passes; the highest request a percentage with one decimal place can hold is
# 99.9%, which derives 163.
#
# The ceiling is found on the missed share rather than on the printed figure, and that distinction is load
# bearing: the figure counts hundredths while the missed share falls smoothly, so the figure is a staircase,
# and one that has not moved between two passes has often not finished moving. The missed share itself falls
# at every pass until it reaches its fixed point, where it stays for good, and the count where that happens
# is the ceiling — the last count at which another pass says anything new. It lands below 100 because the
# model floors its figure: the missed share settles at a dozen millionths instead of reaching nothing, so the
# model never claims the whole four-slot space is exercised — the same reason the pair model floors its own —
# and a request for all of it is refused here, with the ceiling's own figure and pass count in the message
# rather than a rule about percentages.
set(qs_ceiling_passes 0)
qs_quad_missed(0 qs_ceiling_missed)
while(1)
    math(EXPR qs_next_passes "${qs_ceiling_passes} + 1")
    qs_quad_missed(${qs_next_passes} qs_next_missed)
    if(qs_next_missed GREATER_EQUAL qs_ceiling_missed)
        break()
    endif()
    set(qs_ceiling_passes "${qs_next_passes}")
    set(qs_ceiling_missed "${qs_next_missed}")
endwhile()
qs_quad_share_units(${qs_ceiling_passes} qs_ceiling_units)
qs_units_text(${qs_ceiling_units} qs_ceiling_text)

# The count itself: the first pass whose figure reaches the request, up to the ceiling.
set(qs_derived_passes 0)
set(qs_below_passes 0)
set(qs_below_units 0)
math(EXPR qs_probe "1")
while(qs_probe LESS_EQUAL qs_ceiling_passes)
    qs_quad_share_units(${qs_probe} qs_at_units)
    if(qs_at_units GREATER_EQUAL coverage_units)
        set(qs_derived_passes "${qs_probe}")
        math(EXPR qs_below_passes "${qs_probe} - 1")
        qs_quad_share_units(${qs_below_passes} qs_below_units)
        break()
    endif()
    math(EXPR qs_probe "${qs_probe} + 1")
endwhile()
qs_units_text(${qs_below_units} qs_below_text)
if(qs_derived_passes EQUAL 0)
    message(FATAL_ERROR
        "QS_TEST_ORDER_COVERAGE is ${COVERAGE}%, and the four-slot model's ceiling is ${qs_ceiling_text}% at "
        "${qs_ceiling_passes} passes: the highest figure it returns, because it floors rather than rounding "
        "up and so never claims the whole space is exercised. Ask for less than ${qs_ceiling_text}")
endif()
# Joined with string(CONCAT) rather than written as adjacent arguments to set(): those are a list, and the
# report would read "...interaction ;space..." with a separator showing where the source line wrapped.
if(PASSES STREQUAL "")
    set(PASSES "${qs_derived_passes}")
    # The derivation held to itself before it is believed. The count that runs has to be one whose model
    # clears the request, and the count before it has to be one that does not: otherwise the walk has picked
    # a count the model does not justify and the run would report it beside figures that contradict it. Both
    # figures are the printed ones, so this is the arithmetic a reader would do by hand — and it is why the
    # derivation is a claim the run can fail on rather than a line of prose.
    qs_quad_share_units(${qs_derived_passes} qs_chosen_units)
    if(qs_chosen_units LESS coverage_units OR NOT qs_below_units LESS coverage_units)
        message(FATAL_ERROR
            "the derived pass count ${qs_derived_passes} is not the smallest whose model reaches the "
            "requested ${COVERAGE}%: the model stands at ${qs_chosen_units} hundredths of a point for that "
            "count and ${qs_below_units} for the count before it, against a request of ${coverage_units}")
    endif()
    string(CONCAT qs_pass_source
        "the smallest count whose model reaches the requested ${COVERAGE}% of the four-slot interaction "
        "space, ${qs_below_text}% being what ${qs_below_passes} passes give")
else()
    string(CONCAT qs_pass_source
        "given by QS_TEST_ORDER_PASSES, where the requested ${COVERAGE}% would have derived "
        "${qs_derived_passes} (${qs_below_text}% at ${qs_below_passes} passes)")
endif()

# The twenty-four relative orders of four slots, each naming which of the four comes first, second, third
# and fourth. The reading side below is built on this table, so a typo in it would quietly under-count: a
# subset exercised in all twenty-four orders would be read as short in whichever order the table omits, and
# the figure would look perfectly plausible. Checked before it is used, for exactly that reason.
set(QS_QUAD_PERMUTATIONS
    0123 0132 0213 0231 0312 0321 1023 1032 1203 1230 1302 1320
    2013 2031 2103 2130 2301 2310 3012 3021 3102 3120 3201 3210)

function(qs_check_quad_permutations)
    list(LENGTH QS_QUAD_PERMUTATIONS table_size)
    if(NOT table_size EQUAL 24)
        message(FATAL_ERROR
            "the four-slot permutation table has ${table_size} entries, but four slots have twenty-four "
            "relative orders, so the reading side would count the wrong ones")
    endif()
    foreach(permutation IN LISTS QS_QUAD_PERMUTATIONS)
        string(LENGTH "${permutation}" digits)
        if(NOT digits EQUAL 4)
            message(FATAL_ERROR "the four-slot permutation '${permutation}' is not four digits")
        endif()
        string(SUBSTRING "${permutation}" 0 1 d0)
        string(SUBSTRING "${permutation}" 1 1 d1)
        string(SUBSTRING "${permutation}" 2 1 d2)
        string(SUBSTRING "${permutation}" 3 1 d3)
        set(sorted "${d0};${d1};${d2};${d3}")
        list(SORT sorted)
        if(NOT sorted STREQUAL "0;1;2;3")
            message(FATAL_ERROR
                "the four-slot permutation '${permutation}' does not put each of four slots in exactly "
                "one position, so it does not name a relative order")
        endif()
    endforeach()
    set(copy "${QS_QUAD_PERMUTATIONS}")
    list(REMOVE_DUPLICATES copy)
    list(LENGTH copy unique)
    if(NOT unique EQUAL 24)
        message(FATAL_ERROR
            "the four-slot permutation table names ${unique} distinct orders rather than twenty-four, so "
            "a subset's orders would be counted twice or not at all")
    endif()
endfunction()

qs_check_quad_permutations()

# How many of the six relative orders of these three slots the passes exercised. The sets were recorded
# during the passes as one variable per observed order, which is a lookup rather than a scan: a binary of
# twenty-one slots has 1330 triples and 24 passes append 31,920 entries to any list that held them, so
# reading them back out of a list would be quadratic in a list that size. The binary is part of the key so
# that two binaries with same-named slots cannot be added together. Sets `<out_var>`.
function(qs_triple_orders_seen binary_name first second third out_var)
    set(seen 0)
    foreach(order_string IN ITEMS
            "${first}|${second}|${third}"
            "${first}|${third}|${second}"
            "${second}|${first}|${third}"
            "${second}|${third}|${first}"
            "${third}|${first}|${second}"
            "${third}|${second}|${first}")
        if(DEFINED "qs_triple|${binary_name}|${order_string}")
            math(EXPR seen "${seen} + 1")
        endif()
    endforeach()
    set("${out_var}" "${seen}" PARENT_SCOPE)
endfunction()

# How many of the twenty-four relative orders of these four slots the passes exercised, read off the sets
# the passes recorded the same way the triples are. That the keys are lookups rather than a list matters
# more here than there: a binary of twenty-one slots has 5985 four-slot subsets, and ninety-six passes
# append 574,560 entries to any list that held their orders. The binary is part of the key so two binaries
# with same-named slots cannot be added together. Sets `<out_var>`.
function(qs_quad_orders_seen binary_name first second third fourth out_var)
    set(four "${first};${second};${third};${fourth}")
    set(seen 0)
    foreach(permutation IN LISTS QS_QUAD_PERMUTATIONS)
        string(SUBSTRING "${permutation}" 0 1 p0)
        string(SUBSTRING "${permutation}" 1 1 p1)
        string(SUBSTRING "${permutation}" 2 1 p2)
        string(SUBSTRING "${permutation}" 3 1 p3)
        list(GET four ${p0} n0)
        list(GET four ${p1} n1)
        list(GET four ${p2} n2)
        list(GET four ${p3} n3)
        if(DEFINED "qs_quad|${binary_name}|${n0}|${n1}|${n2}|${n3}")
            math(EXPR seen "${seen} + 1")
        endif()
    endforeach()
    set("${out_var}" "${seen}" PARENT_SCOPE)
endfunction()

# What chance a correct run has of falling below a coverage floor, given the pair count, the pass count
# and the floor. A pair is missed exactly when every pass agrees on its two slots, which with k passes
# happens with probability 2^(1-k); the floor permits a certain number of misses; and Markov's inequality
# bounds the chance of exceeding that allowance at E[missed] / (allowance + 1). Nothing is assumed about
# the pairs being independent — they are not, since one permutation decides the direction of every pair at
# once — which is why Markov rather than a sharper bound. Sets `<prefix>_per_billion` (the bound) and
# `<prefix>_text` (the same figure in words).
function(qs_coverage_fail_chance pairs passes threshold prefix)
    # 2^(1-k) in billionths, by repeated halving, which cannot overflow however many passes are asked for.
    set(power 1000000000)
    math(EXPR halvings "${passes} - 1")
    math(EXPR step "1")
    while(step LESS_EQUAL halvings)
        math(EXPR power "${power} / 2")
        math(EXPR step "${step} + 1")
    endwhile()

    # The misses the floor leaves room for: the pairs past the threshold's share, rounded up, so that
    # rounding can only make the allowance smaller and the bound pessimistic.
    math(EXPR required "${pairs} * ${threshold}")
    math(EXPR allowed "${pairs} - (${required} + 99) / 100")
    math(EXPR expected "${pairs} * ${power}")
    math(EXPR bound "${expected} / (${allowed} + 1)")

    set("${prefix}_per_billion" "${bound}" PARENT_SCOPE)
    if(bound LESS 1)
        set("${prefix}_text" "far rarer than one run in a billion" PARENT_SCOPE)
    else()
        math(EXPR runs "1000000000 / ${bound}")
        set("${prefix}_text" "about one run in ${runs}" PARENT_SCOPE)
    endif()
endfunction()

# The shuffle checked against itself before it is trusted to judge anything else. Every coverage figure
# below is a statement about the orders this produces, so a shuffle that returned its input, or produced
# the same order whatever the seed, would have all of them reporting confidently on orders that never
# varied — worse than reporting nothing. These seeds are fixed and the outcome is a fact about them, so
# this cannot be flaky the way a statistical threshold would be.
function(qs_check_shuffle)
    set(subject alpha beta gamma delta epsilon)
    set(untouched "${subject}")
    list(SORT subject)

    set(first "${untouched}")
    qs_shuffle(first 1)
    set(second "${untouched}")
    qs_shuffle(second 2)

    set(sorted_first "${first}")
    set(sorted_second "${second}")
    list(SORT sorted_first)
    list(SORT sorted_second)

    if(NOT sorted_first STREQUAL subject OR NOT sorted_second STREQUAL subject)
        message(FATAL_ERROR
            "the shuffle changed which slots it was given: '${first}' and '${second}' are not both "
            "reorderings of '${untouched}'")
    endif()
    if(first STREQUAL untouched AND second STREQUAL untouched)
        message(FATAL_ERROR "the shuffle returned its input unchanged for two different seeds")
    endif()
    if(first STREQUAL second)
        message(FATAL_ERROR
            "two different seeds produced the same order '${first}', so the seed does not choose the "
            "order")
    endif()
endfunction()

qs_check_shuffle()

set(failures "")
set(runs 0)
# Counts across binaries and passes rather than restarting per binary, so no two runs in one pass over this
# script get the same order. It starts where the shard index above put it.
set(stream "${stream_start}")

set(total_pairs 0)
set(total_covered 0)
set(total_triples 0)
set(total_triples_complete 0)
set(total_quads 0)
set(total_quads_complete 0)
set(total_quad_seen 0)
# The named triples and four-slot subsets from every binary, printed together once the passes are over.
set(worst_lines "")
set(worst_quad_lines "")

# Every model depends only on the number of passes, so they are worked out once and printed beside every
# binary's measured figure.
qs_model_coverage(${PASSES})
qs_triple_model(${PASSES})
qs_quad_share_model(${PASSES})
qs_quad_complete_model(${PASSES})

# The floor is only worth having if a correct run has almost no chance of falling below it, and that
# chance is arithmetic on the pass count and the number of pairs — both known before a single test is run,
# and neither depending on the seed. So the combinations are settled first, and one too close to call is
# refused here rather than discovered as an intermittent failure months later.
if(MIN_PAIR_COVERAGE GREATER 0)
    set(qs_all_pairs 0)
    foreach(binary IN LISTS TESTS)
        qs_slots_of("${binary}" declared)
        list(LENGTH declared declared_count)
        math(EXPR binary_pairs "${declared_count} * (${declared_count} - 1) / 2")
        math(EXPR qs_all_pairs "${qs_all_pairs} + ${binary_pairs}")

        qs_coverage_fail_chance(${binary_pairs} ${PASSES} ${MIN_PAIR_COVERAGE} qs_chance)
        if(qs_chance_per_billion GREATER QS_FLAKINESS_LIMIT_PER_BILLION)
            message(FATAL_ERROR
                "a ${MIN_PAIR_COVERAGE}% floor over ${PASSES} passes would fail a correct run for "
                "${binary} ${qs_chance_text}, which is too often to be a check rather than a coin toss. A "
                "pair is missed with probability 2^(1-passes), and the misses a correct run produces are "
                "more than this floor leaves room for. Raise QS_TEST_ORDER_COVERAGE, which derives a "
                "larger pass count, or set QS_TEST_ORDER_PASSES directly; lower QS_MIN_PAIR_COVERAGE; or "
                "set that to 0 to report the coverage without asserting it.")
        endif()
    endforeach()

    qs_coverage_fail_chance(${qs_all_pairs} ${PASSES} ${MIN_PAIR_COVERAGE} qs_total_chance)
    if(qs_total_chance_per_billion GREATER QS_FLAKINESS_LIMIT_PER_BILLION)
        message(FATAL_ERROR
            "a ${MIN_PAIR_COVERAGE}% floor over ${PASSES} passes would fail a correct run over all "
            "${qs_all_pairs} pairs ${qs_total_chance_text}. Raise QS_TEST_ORDER_COVERAGE, which derives "
            "a larger pass count, or set QS_TEST_ORDER_PASSES directly; lower QS_MIN_PAIR_COVERAGE; or "
            "set that to 0 to report the coverage without asserting it.")
    endif()

    message(STATUS
        "enforced floor: ${MIN_PAIR_COVERAGE}% of slot pairs in both orders, per binary and over all "
        "${qs_all_pairs}; a correct run falls below it ${qs_total_chance_text}")
endif()

list(LENGTH TESTS qs_binary_count)
message(STATUS
    "shuffled slot order${qs_shard_phrase}: seed ${SEED}, ${PASSES} passes for each of the "
    "${qs_binary_count} binaries this run holds")
message(STATUS
    "the ${PASSES} passes are ${qs_pass_source}")
message(STATUS
    "reproduce this shard with QS_TEST_ORDER_SEED=${SEED}"
    "${qs_shard_phrase}")
if(SHARD_COUNT GREATER 1)
    message(STATUS
        "the whole check is ${SHARD_COUNT} shards, and QS_TEST_ORDER_SEED=${SEED} reproduces all of them")
endif()

foreach(binary IN LISTS TESTS)
    # Read once, before any pass: each pass shuffles a copy of this declared order rather than the result
    # of the previous one, so every line says what a straight shuffle of the declared list produced and
    # can be worked out by hand from its seed.
    qs_slots_of("${binary}" canonical)
    list(LENGTH canonical slot_count)
    # Part of the key the triple sets are recorded under, so two binaries that happen to name a slot the
    # same way cannot be read as one another.
    get_filename_component(binary_name "${binary}" NAME)

    set(directions "")
    # Distinct triple and four-slot orders recorded for this binary, counted as they appear, so that the
    # reading side below can be checked against the writing side instead of trusted.
    math(EXPR triples_recorded 0)
    math(EXPR quads_recorded 0)
    math(EXPR pass "1")
    while(pass LESS_EQUAL PASSES)
        math(EXPR stream "(${stream} * 1103515245 + 12345) % 2147483648")
        set(order "${canonical}")
        qs_shuffle(order "${stream}")
        # Printed for every pass, not only for a failing one: it is what makes the seed verifiable rather
        # than merely recorded — the same seed has to produce these same lines, or reproducing a failure
        # from the seed would be a hope rather than a method.
        message(STATUS "${binary} pass ${pass}: seed ${stream}, order: ${order}")

        # Every pair in this order, as "a|b" for a before b. This is the accounting the coverage report is
        # made of, and it is taken from the order that was actually run rather than from the one that was
        # asked for, so a shuffle that failed to shuffle shows up as coverage that never grows. The triples
        # and four-slot subsets are recorded underneath it, each as the slots in the order the pass put
        # them in — which is what makes the keys canonical: two passes write the same key for a subset
        # exactly when they put those same slots in the same relative order.
        math(EXPR i "0")
        while(i LESS slot_count)
            list(GET order ${i} earlier)
            math(EXPR j "${i} + 1")
            while(j LESS slot_count)
                list(GET order ${j} later)
                list(APPEND directions "${earlier}|${later}")
                math(EXPR k "${j} + 1")
                while(k LESS slot_count)
                    list(GET order ${k} last)
                    set(triple_key "qs_triple|${binary_name}|${earlier}|${later}|${last}")
                    if(NOT DEFINED "${triple_key}")
                        math(EXPR triples_recorded "${triples_recorded} + 1")
                    endif()
                    set("${triple_key}" 1)
                    math(EXPR l "${k} + 1")
                    while(l LESS slot_count)
                        list(GET order ${l} fourth)
                        set(quad_key "qs_quad|${binary_name}|${earlier}|${later}|${last}|${fourth}")
                        if(NOT DEFINED "${quad_key}")
                            math(EXPR quads_recorded "${quads_recorded} + 1")
                        endif()
                        set("${quad_key}" 1)
                        math(EXPR l "${l} + 1")
                    endwhile()
                    math(EXPR k "${k} + 1")
                endwhile()
                math(EXPR j "${j} + 1")
            endwhile()
            math(EXPR i "${i} + 1")
        endwhile()

        execute_process(
            COMMAND "${binary}" ${order}
            RESULT_VARIABLE code
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error_output
            TIMEOUT 300)
        math(EXPR runs "${runs} + 1")

        if(NOT code EQUAL 0)
            # The seed and the order it produced both go in the message: with the seed the order can be
            # worked out again, and printing it means it does not have to be.
            list(APPEND failures
                "${binary} pass ${pass} of ${PASSES}, order seed ${stream}, exit ${code}\n\
order: ${order}\n${output}${error_output}")
        endif()

        math(EXPR pass "${pass} + 1")
    endwhile()

    # What this binary's passes bought. `covered` counts the pairs seen in both orders; `pairs` is every
    # pair there is, which for a binary of n slots is n(n-1)/2.
    math(EXPR pairs 0)
    math(EXPR covered 0)
    math(EXPR left "0")
    while(left LESS slot_count)
        list(GET canonical ${left} first_slot)
        math(EXPR right "${left} + 1")
        while(right LESS slot_count)
            list(GET canonical ${right} second_slot)
            math(EXPR pairs "${pairs} + 1")
            if("${first_slot}|${second_slot}" IN_LIST directions AND
               "${second_slot}|${first_slot}" IN_LIST directions)
                math(EXPR covered "${covered} + 1")
            endif()
            math(EXPR right "${right} + 1")
        endwhile()
        math(EXPR left "${left} + 1")
    endwhile()

    # n! — how many orderings exist — counted only while the product stays inside the range CMake integers
    # are exact in, so a binary large enough to leave it reports a floor rather than a wrong number. The
    # multiply is guarded before it happens, because the overflow would be in the multiply.
    math(EXPR orderings 1)
    set(orderings_exact 1)
    math(EXPR n "1")
    while(n LESS_EQUAL slot_count)
        math(EXPR limit "4000000000000000000 / ${n}")
        if(orderings GREATER limit)
            set(orderings_exact 0)
            break()
        endif()
        math(EXPR orderings "${orderings} * ${n}")
        math(EXPR n "${n} + 1")
    endwhile()

    # n(n-1)/2 is what the pair loop above should have counted. Checked rather than assumed, because a
    # report whose denominator is wrong is worse than no report at all.
    math(EXPR expected_pairs "${slot_count} * (${slot_count} - 1) / 2")
    if(NOT pairs EQUAL expected_pairs)
        message(FATAL_ERROR
            "${binary}: counted ${pairs} slot pairs, but ${slot_count} slots make ${expected_pairs}")
    endif()

    # The recording side, checked against the same count from the other end: every pass records every pair
    # exactly once, so the list has PASSES copies of n(n-1)/2 entries. Without this, a recording loop that
    # quietly skipped pairs would lower the coverage it reported and explain nothing.
    list(LENGTH directions recorded_pairs)
    math(EXPR expected_recorded "${PASSES} * ${expected_pairs}")
    if(NOT recorded_pairs EQUAL expected_recorded)
        message(FATAL_ERROR
            "${binary}: the passes recorded ${recorded_pairs} pair orders, but ${PASSES} passes of "
            "${expected_pairs} pairs each is ${expected_recorded}")
    endif()

    # The triples, read back off the sets the passes recorded. `floor` is the worst case among them: the
    # fewest of the six relative orders any one triple was exercised in.
    math(EXPR triples 0)
    math(EXPR triples_complete 0)
    math(EXPR triples_at_least_five 0)
    math(EXPR triples_seen_total 0)
    set(triples_floor 6)
    math(EXPR x "0")
    while(x LESS slot_count)
        list(GET canonical ${x} first_slot)
        math(EXPR y "${x} + 1")
        while(y LESS slot_count)
            list(GET canonical ${y} second_slot)
            math(EXPR z "${y} + 1")
            while(z LESS slot_count)
                list(GET canonical ${z} third_slot)
                math(EXPR triples "${triples} + 1")
                qs_triple_orders_seen("${binary_name}" "${first_slot}" "${second_slot}" "${third_slot}"
                                      seen)
                math(EXPR triples_seen_total "${triples_seen_total} + ${seen}")
                if(seen EQUAL 6)
                    math(EXPR triples_complete "${triples_complete} + 1")
                endif()
                if(seen GREATER_EQUAL 5)
                    math(EXPR triples_at_least_five "${triples_at_least_five} + 1")
                endif()
                if(seen LESS triples_floor)
                    set(triples_floor "${seen}")
                endif()
                if(seen LESS 6)
                    # Named as it is counted, bucketed by how much of the triple was explored, because a
                    # triple short by two orders is a better lead than one short by one. The count goes in
                    # with the name so the report can label each one. Printed after the passes rather than
                    # here, where a pass line would bury it.
                    list(APPEND "qs_short|${binary_name}|${seen}"
                         "${seen}|${first_slot}|${second_slot}|${third_slot}")
                endif()
                math(EXPR z "${z} + 1")
            endwhile()
            math(EXPR y "${y} + 1")
        endwhile()
        math(EXPR x "${x} + 1")
    endwhile()

    math(EXPR expected_triples "${slot_count} * (${slot_count} - 1) * (${slot_count} - 2) / 6")
    if(NOT triples EQUAL expected_triples)
        message(FATAL_ERROR
            "${binary}: counted ${triples} slot triples, but ${slot_count} slots make ${expected_triples}")
    endif()

    # The same cross-check as for pairs, and the one that matters more here: every distinct triple order
    # the passes wrote down is one this reading side found, and vice versa. Counting them independently is
    # what keeps a recording loop that missed triples from showing up only as a smaller number.
    if(NOT triples_seen_total EQUAL triples_recorded)
        message(FATAL_ERROR
            "${binary}: the passes recorded ${triples_recorded} distinct triple orders, but reading the "
            "triples back found ${triples_seen_total} of them, so the two sides disagree")
    endif()

    # The four-slot subsets, read back the same way and for the same reason. `quads_floor` is the worst
    # case among them: the fewest of the twenty-four relative orders any one subset was exercised in, which
    # is the figure that says how bad the worst case is when the count complete does not.
    math(EXPR quads 0)
    math(EXPR quads_complete 0)
    math(EXPR quads_seen_total 0)
    set(quads_floor 24)
    math(EXPR x "0")
    while(x LESS slot_count)
        list(GET canonical ${x} first_slot)
        math(EXPR y "${x} + 1")
        while(y LESS slot_count)
            list(GET canonical ${y} second_slot)
            math(EXPR z "${y} + 1")
            while(z LESS slot_count)
                list(GET canonical ${z} third_slot)
                math(EXPR w "${z} + 1")
                while(w LESS slot_count)
                    list(GET canonical ${w} fourth_slot)
                    math(EXPR quads "${quads} + 1")
                    qs_quad_orders_seen("${binary_name}" "${first_slot}" "${second_slot}"
                                        "${third_slot}" "${fourth_slot}" seen)
                    math(EXPR quads_seen_total "${quads_seen_total} + ${seen}")
                    if(seen EQUAL 24)
                        math(EXPR quads_complete "${quads_complete} + 1")
                    endif()
                    if(seen LESS quads_floor)
                        set(quads_floor "${seen}")
                    endif()
                    if(seen LESS 24)
                        # Named as it is counted, bucketed by how much of the subset was explored, because
                        # a subset short by three orders is a better lead than one short by one — and at
                        # the default pass count most of them are short by one.
                        list(APPEND "qs_short_quad|${binary_name}|${seen}"
                             "${seen}|${first_slot}|${second_slot}|${third_slot}|${fourth_slot}")
                    endif()
                    math(EXPR w "${w} + 1")
                endwhile()
                math(EXPR z "${z} + 1")
            endwhile()
            math(EXPR y "${y} + 1")
        endwhile()
        math(EXPR x "${x} + 1")
    endwhile()

    # n(n-1)(n-2)(n-3)/24 — every way to choose four of the slots. The product is divisible by 24, so the
    # division is exact; checked rather than assumed, like the pair and triple denominators.
    math(EXPR expected_quads
        "${slot_count} * (${slot_count} - 1) * (${slot_count} - 2) * (${slot_count} - 3) / 24")
    if(NOT quads EQUAL expected_quads)
        message(FATAL_ERROR
            "${binary}: counted ${quads} four-slot subsets, but ${slot_count} slots make "
            "${expected_quads}")
    endif()

    # The same cross-check as for pairs and triples, and the one that matters most here: every distinct
    # four-slot order the passes wrote down is one this reading side found, and vice versa. Without it, a
    # recording loop that skipped subsets would show up only as a smaller figure with nothing to compare.
    if(NOT quads_seen_total EQUAL quads_recorded)
        message(FATAL_ERROR
            "${binary}: the passes recorded ${quads_recorded} distinct four-slot orders, but reading the "
            "subsets back found ${quads_seen_total} of them, so the two sides disagree")
    endif()

    qs_percent(${covered} ${pairs} qs_observed)
    qs_percent(${triples_complete} ${triples} qs_triples_observed)
    # The four-slot measure is two figures, because neither one says it alone: how much of the space of
    # four-slot orders the run reached, and how many subsets had every one of their twenty-four orders.
    math(EXPR quad_orders "${quads} * 24")
    qs_percent(${quads_seen_total} ${quad_orders} qs_quad_orders_observed)
    qs_percent(${quads_complete} ${quads} qs_quads_observed)

    # The floor, enforced. It is safe to enforce because the combination was settled before anything ran:
    # a correct run has less than one chance in a hundred thousand of being here.
    if(MIN_PAIR_COVERAGE GREATER 0 AND pairs GREATER 0)
        math(EXPR coverage_reached "${covered} * 100")
        math(EXPR coverage_required "${pairs} * ${MIN_PAIR_COVERAGE}")
        if(coverage_reached LESS coverage_required)
            qs_coverage_fail_chance(${pairs} ${PASSES} ${MIN_PAIR_COVERAGE} qs_here)
            message(FATAL_ERROR
                "${binary}: ${covered} of ${pairs} slot pairs were exercised in both orders "
                "(${qs_observed_text}%), below the ${MIN_PAIR_COVERAGE}% this run requires. A correct run "
                "would fall below it ${qs_here_text}, so this is a defect rather than an unlucky seed: "
                "expect a shuffle that is not shuffling, or a recording or reading side that has lost "
                "pairs. Repeat these orders with QS_TEST_ORDER_SEED=${SEED}.")
        endif()
    endif()

    math(EXPR total_pairs "${total_pairs} + ${pairs}")
    math(EXPR total_covered "${total_covered} + ${covered}")
    math(EXPR total_triples "${total_triples} + ${triples}")
    math(EXPR total_triples_complete "${total_triples_complete} + ${triples_complete}")
    math(EXPR total_quads "${total_quads} + ${quads}")
    math(EXPR total_quads_complete "${total_quads_complete} + ${quads_complete}")
    math(EXPR total_quad_seen "${total_quad_seen} + ${quads_seen_total}")

    if(pairs EQUAL 0)
        message(STATUS "${binary}: ${slot_count} slots, so there is no pair of slots to order")
    else()
        message(STATUS
            "${binary}: ${slot_count} slots — pairs exercised in both orders ${covered} of ${pairs} "
            "(${qs_observed_text}%), against ${qs_model_text}% for ${PASSES} uniformly drawn passes")
    endif()

    if(triples GREATER 0)
        message(STATUS
            "${binary}: triples ${triples} — all six relative orders exercised for ${triples_complete} "
            "(${qs_triples_observed_text}%), against ${qs_triple_model_text}% for ${PASSES} uniformly "
            "drawn passes; ${triples_at_least_five} of them have five or more, and the least any triple "
            "has seen is ${triples_floor} of 6")
    else()
        message(STATUS "${binary}: ${slot_count} slots, so there is no triple of slots to order")
    endif()

    if(quads GREATER 0)
        message(STATUS
            "${binary}: four-slot orders ${quads_seen_total} of ${quad_orders} exercised "
            "(${qs_quad_orders_observed_text}% of the four-slot interaction space, against "
            "${qs_quad_share_model_text}% for ${PASSES} uniformly drawn passes); ${quads_complete} of the "
            "${quads} subsets have all twenty-four orders (${qs_quads_observed_text}%, against "
            "${qs_quad_complete_model_text}% for the same passes), and the least any subset saw is "
            "${quads_floor} of 24")
    else()
        message(STATUS "${binary}: ${slot_count} slots, so there is no four-slot subset to order")
    endif()

    # The triples left short, by name, least explored first. Collected here rather than at the end so that
    # the buckets can be held to the counts they were built from, and so that a binary's figure and the
    # names behind it are read off the same walk.
    math(EXPR short_expected "${triples} - ${triples_complete}")
    math(EXPR short_total 0)
    set(worst_named "")
    math(EXPR band "1")
    while(band LESS 6)
        set(bucket "qs_short|${binary_name}|${band}")
        if(DEFINED "${bucket}")
            # The label each name carries has to be the band it was filed under, or the report would say a
            # triple was exercised in one number of orders while the bucket says another. Cheap to check
            # here and not derivable from the totals, which count names without reading them.
            foreach(item IN LISTS "${bucket}")
                string(SUBSTRING "${item}" 0 1 label)
                if(NOT label EQUAL band)
                    message(FATAL_ERROR
                        "${binary}: a triple filed under ${band} of six orders is labelled '${label}', so "
                        "the report would name it with the wrong count")
                endif()
            endforeach()
            list(LENGTH "${bucket}" at_this_seen)
            math(EXPR short_total "${short_total} + ${at_this_seen}")
            if(WORST_TRIPLES_SHOWN GREATER 0)
                list(LENGTH worst_named named_count)
                math(EXPR room "${WORST_TRIPLES_SHOWN} - ${named_count}")
                if(room GREATER 0)
                    # Truncates to what the bucket holds, so the last bucket read cannot run off its end.
                    list(SUBLIST "${bucket}" 0 ${room} take)
                    list(APPEND worst_named ${take})
                endif()
            endif()
        endif()
        math(EXPR band "${band} + 1")
    endwhile()

    # The buckets are a second count of the same triples, so they are held to the first: if naming them
    # walked a different set, the names would be of triples the figures above do not describe.
    if(NOT short_total EQUAL short_expected)
        message(FATAL_ERROR
            "${binary}: ${short_expected} slot triples are short of all six relative orders, but the "
            "buckets name ${short_total}, so the counts and the names disagree")
    endif()

    if(short_total GREATER 0)
        list(LENGTH worst_named named_count)
        if(named_count EQUAL 0)
            list(APPEND worst_lines
                "${binary_name}: ${short_total} of ${triples} short of all six orders, names suppressed "
                "by QS_WORST_TRIPLES_SHOWN=0")
        else()
            # One triple per line, because a slot name in this suite is a sentence and several of them
            # joined by commas is a paragraph nobody reads. Each line carries how many of the six orders
            # that triple was exercised in, which is what says how much of it is unexplored.
            # Short on purpose: message() wraps at the console width and breaks the line mid-phrase, which
            # turns a heading into three fragments.
            list(APPEND worst_lines
                "${binary_name}: ${short_total} of ${triples} short of all six orders, least first:")
            foreach(item IN LISTS worst_named)
                string(SUBSTRING "${item}" 0 1 orders_seen)
                string(SUBSTRING "${item}" 2 -1 triple_name)
                list(APPEND worst_lines "    ${orders_seen} of 6  ${triple_name}")
            endforeach()
            if(named_count LESS short_total)
                math(EXPR unnamed "${short_total} - ${named_count}")
                list(APPEND worst_lines
                    "    and ${unnamed} more, explored at least as thoroughly as those")
            endif()
        endif()
    endif()

    # The four-slot subsets left short, by name, the same way the triples are: least explored first, so a
    # subset that saw twenty of its twenty-four orders is read before one that saw twenty-three. At the
    # default pass count there is no band of these that comes out empty, which is the whole reason this
    # measure exists — the triples above it are complete and name nothing.
    math(EXPR quad_short_expected "${quads} - ${quads_complete}")
    math(EXPR quad_short_total 0)
    set(worst_quads_named "")
    math(EXPR band "1")
    while(band LESS 24)
        set(bucket "qs_short_quad|${binary_name}|${band}")
        if(DEFINED "${bucket}")
            # The label each name carries has to be the band it was filed under, or the report would say a
            # subset was exercised in one number of orders while the bucket says another. Read up to the
            # first separator rather than at a fixed width, because unlike the triple bands this one runs to
            # two digits.
            foreach(item IN LISTS "${bucket}")
                string(FIND "${item}" "|" bar)
                string(SUBSTRING "${item}" 0 ${bar} label)
                if(NOT label EQUAL band)
                    message(FATAL_ERROR
                        "${binary}: a four-slot subset filed under ${band} of twenty-four orders is "
                        "labelled '${label}', so the report would name it with the wrong count")
                endif()
            endforeach()
            list(LENGTH "${bucket}" at_this_seen)
            math(EXPR quad_short_total "${quad_short_total} + ${at_this_seen}")
            if(WORST_QUADS_SHOWN GREATER 0)
                list(LENGTH worst_quads_named named_count)
                math(EXPR room "${WORST_QUADS_SHOWN} - ${named_count}")
                if(room GREATER 0)
                    # Truncates to what the bucket holds, so the last bucket read cannot run off its end.
                    list(SUBLIST "${bucket}" 0 ${room} take)
                    list(APPEND worst_quads_named ${take})
                endif()
            endif()
        endif()
        math(EXPR band "${band} + 1")
    endwhile()

    # The buckets are a second count of the same subsets, held to the first, exactly as for the triples.
    if(NOT quad_short_total EQUAL quad_short_expected)
        message(FATAL_ERROR
            "${binary}: ${quad_short_expected} four-slot subsets are short of all twenty-four relative "
            "orders, but the buckets name ${quad_short_total}, so the counts and the names disagree")
    endif()

    if(quad_short_total GREATER 0)
        list(LENGTH worst_quads_named quad_named_count)
        if(quad_named_count EQUAL 0)
            list(APPEND worst_quad_lines
                "${binary_name}: ${quad_short_total} of ${quads} short of all twenty-four orders, names "
                "suppressed by QS_WORST_QUADS_SHOWN=0")
        else()
            # One subset per line, and the count it carries first, because a four-slot name is long and
            # the count is what says whether reading it is worth the line.
            list(APPEND worst_quad_lines
                "${binary_name}: ${quad_short_total} of ${quads} short of all 24, least first:")
            foreach(item IN LISTS worst_quads_named)
                string(FIND "${item}" "|" bar)
                string(SUBSTRING "${item}" 0 ${bar} orders_seen)
                math(EXPR name_start "${bar} + 1")
                string(SUBSTRING "${item}" ${name_start} -1 quad_name)
                list(APPEND worst_quad_lines "    ${orders_seen} of 24  ${quad_name}")
            endforeach()
            if(quad_named_count LESS quad_short_total)
                math(EXPR unnamed "${quad_short_total} - ${quad_named_count}")
                list(APPEND worst_quad_lines
                    "    and ${unnamed} more, explored at least as thoroughly as those")
            endif()
        endif()
    endif()

    # The share of all the orderings that this run visited, on its own line: it is the figure a reader
    # reaches for first, and for every binary but the smallest it is so close to nothing that the line
    # exists to say so rather than to be compared with anything.
    if(orderings_exact)
        qs_share(${PASSES} ${orderings} qs_visited)
        message(STATUS "${binary}: ordered ${PASSES} of its ${orderings} orderings (${qs_visited_text})")
    else()
        message(STATUS
            "${binary}: ordered ${PASSES} of more than ${orderings} orderings, which is below 0.0001% "
            "and is why the interaction coverage above is the figure reported")
    endif()
endforeach()

if(failures)
    string(REPLACE ";" "\n  - " readable "${failures}")
    message(FATAL_ERROR
        "${runs} shuffled runs${qs_shard_phrase}, and these did not hold:\n  - ${readable}\n\n\
Re-run one order with QS_TEST_ORDER_SEED=${SEED} on this revision.")
endif()

if(total_pairs GREATER 0)
    qs_percent(${total_covered} ${total_pairs} qs_total_observed)

    # The same floor over every binary at once, which is the figure the summary reports.
    if(MIN_PAIR_COVERAGE GREATER 0)
        math(EXPR total_reached "${total_covered} * 100")
        math(EXPR total_required "${total_pairs} * ${MIN_PAIR_COVERAGE}")
        if(total_reached LESS total_required)
            qs_coverage_fail_chance(${total_pairs} ${PASSES} ${MIN_PAIR_COVERAGE} qs_here)
            message(FATAL_ERROR
                "${total_covered} of ${total_pairs} slot pairs were exercised in both orders "
                "(${qs_total_observed_text}%), below the ${MIN_PAIR_COVERAGE}% this run requires. A "
                "correct run would fall below it ${qs_here_text}, so something in the pass, recording or "
                "reading of these orders is not doing its job. Repeat with QS_TEST_ORDER_SEED=${SEED}.")
        endif()
    endif()

    message(STATUS
        "shuffled interaction coverage: ${total_covered} of ${total_pairs} slot pairs exercised in both "
        "orders (${qs_total_observed_text}%, against ${qs_model_text}% for ${PASSES} uniformly drawn "
        "passes)")
endif()

if(total_triples GREATER 0)
    qs_percent(${total_triples_complete} ${total_triples} qs_total_triples)
    message(STATUS
        "shuffled interaction coverage: ${total_triples_complete} of ${total_triples} slot triples with "
        "all six relative orders exercised (${qs_total_triples_text}%, against ${qs_triple_model_text}% "
        "for ${PASSES} uniformly drawn passes)")
endif()

# The four-slot measure, in the two figures it takes to say it. The share of the space is the one a reader
# reaches for; the count of subsets complete is the one that shows the share is not the whole story, since
# at the default pass count most of the shortfall sits in subsets that saw twenty-three of their orders out
# of twenty-four. Stated as not complete, beside the two measures above that are, because that contrast is
# what the measure is for.
if(total_quads GREATER 0)
    math(EXPR total_quad_orders "${total_quads} * 24")
    qs_percent(${total_quad_seen} ${total_quad_orders} qs_total_quad_orders)
    qs_percent(${total_quads_complete} ${total_quads} qs_total_quads)
    message(STATUS
        "shuffled interaction coverage: ${total_quad_seen} of ${total_quad_orders} four-slot orders "
        "exercised (${qs_total_quad_orders_text}% of the four-slot interaction space, against "
        "${qs_quad_share_model_text}% for ${PASSES} uniformly drawn passes) — this is the measure the "
        "pair and triple figures above cannot give, and it is the one that is not complete")
    message(STATUS
        "shuffled interaction coverage: ${total_quads_complete} of ${total_quads} four-slot subsets with "
        "all twenty-four relative orders exercised (${qs_total_quads_text}%, against "
        "${qs_quad_complete_model_text}% for the same passes)")
endif()

# The named triples, at the end where a reader looks for the run's findings rather than per binary where
# the pass lines are. Only when there are any: at the default pass count a binary whose slots number in
# the single figures is usually complete, and a heading over nothing teaches a reader to skip the section.
if(worst_lines)
    message(STATUS
        "shuffled interaction coverage: the slot triples left short of their six relative orders — each "
        "name is three slots in declared order, and 6 of 6 is every order of those three exercised")
    foreach(line IN LISTS worst_lines)
        message(STATUS "  ${line}")
    endforeach()
endif()

# The named four-slot subsets, beside the triple names. Unlike that section this one is never empty at the
# default pass count, which is the finding rather than a defect: the triples are complete and these are
# not, so this is where a run's leads are.
if(worst_quad_lines)
    message(STATUS
        "shuffled interaction coverage: the four-slot subsets left short of their twenty-four relative "
        "orders — each name is four slots in declared order, and 24 of 24 is every order of those four")
    foreach(line IN LISTS worst_quad_lines)
        message(STATUS "  ${line}")
    endforeach()
endif()

message(STATUS "shuffled slot order${qs_shard_phrase}: ${runs} runs, every order held (seed ${SEED})")
