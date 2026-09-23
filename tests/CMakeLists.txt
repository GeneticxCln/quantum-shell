# Test targets, kept under tests/ so that a file naming a test double is always inside the tree where
# SYSTEM_PROMPT.md § Testing Rules allows one.

# Support more than one test binary needs. It is an interface target rather than a compiled one, because
# the only thing in it is a header: the reconciliation of an event-fed model against a reading of the
# compositor taken at a different moment, which the live stream test runs against a real compositor and
# whose own unit test drives with scripted readings and no compositor at all. Both include it from here,
# so the mechanism they agree about is one copy.
add_library(qs-test-support INTERFACE)
target_include_directories(qs-test-support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")

add_subdirectory(unit)

# The one test that talks to the real compositor. It is registered only when niri names a socket for
# this process: with no socket there is nothing to talk to, and saying which case this is beats a test
# that reports a failure it cannot act on.
add_executable(niri-live-test integration/niri_live_test.cpp)
target_link_libraries(niri-live-test PRIVATE Qt6::Test quantum-shell-niri)

# The same, for the event stream and the state model it feeds. Its comparisons against a fresh reading of
# the compositor go through qs-test-support: the desktop can change between the model's last event and the
# reply, and that has to be told apart from a model that missed an event rather than failing the run.
add_executable(niri-live-stream-test integration/niri_live_stream_test.cpp)
target_link_libraries(niri-live-stream-test PRIVATE Qt6::Test quantum-shell-niri qs-test-support)

# The one test that acts on the session: it opens and closes the overview, which is visible on screen,
# and asks niri to switch the keyboard layout. It is built always and registered only when explicitly
# asked for, because a test run must not rearrange the desktop of whoever started it.
add_executable(niri-live-action-test integration/niri_live_action_test.cpp)
target_link_libraries(niri-live-action-test PRIVATE Qt6::Test quantum-shell-niri)

# The proof that a compositor restart is survived. It starts a niri of its own, nested in the session,
# kills it and starts another, so it needs a session to nest in and puts a window on screen: built
# always, registered only when explicitly asked for.
add_executable(niri-live-restart-test integration/niri_live_restart_test.cpp)
target_link_libraries(niri-live-restart-test PRIVATE Qt6::Test quantum-shell-niri qs-test-support)

# The proof that the bar is a real layer surface: it starts the built shell, waits for its surface to
# reach the compositor, and checks the surface against the compositor's own layer list and against the
# protocol traffic the shell sends. It maps a bar on screen, which reserves the top of the output while it
# runs, so it needs the same opt-in as the other test that changes the desktop.
#
# It is given the shell's path and its plugin directory as compile definitions rather than finding them
# relative to itself, so the test runs against the binary this build produced and not against whatever
# happens to be installed.
add_executable(niri-live-layershell-test integration/niri_live_layershell_test.cpp)
# `quantum-shell-ipc` for the two names this test asserts against a running shell: the abstract socket name it
# reads back out of the kernel's own table, and the protocol revision `qsctl version` is expected to print.
# Linking it is not the test checking the code against itself — the name is compared with what /proc reports,
# and the revision with what the built client prints.
target_link_libraries(niri-live-layershell-test PRIVATE Qt6::Test quantum-shell-niri quantum-shell-ipc
                                                       qs-test-support)
target_compile_definitions(niri-live-layershell-test PRIVATE
    QS_SHELL_BINARY="$<TARGET_FILE:quantum-shell>"
    QS_SHELL_PLUGIN_PATH="${CMAKE_BINARY_DIR}/plugins"
    QS_QSCTL_BINARY="$<TARGET_FILE:qsctl>")

# The proof that restart survival reaches the shell itself: a nested niri is started, the built shell
# runs against it and its bar is confirmed in that compositor; the compositor is stopped and the
# shell's actual death is observed; a second compositor comes up on the socket path niri's naming rule
# guarantees to differ; and the shell is started again, with its bar in the new compositor and its IPC
# answering about the new session. Same opt-ins as the restart test: it starts windows on screen.
add_executable(niri-live-shell-restart-test integration/niri_live_shell_restart_test.cpp)
target_link_libraries(niri-live-shell-restart-test PRIVATE Qt6::Test quantum-shell-niri quantum-shell-ipc
                                                           qs-test-support)
target_compile_definitions(niri-live-shell-restart-test PRIVATE
    QS_SHELL_BINARY="$<TARGET_FILE:quantum-shell>"
    QS_SHELL_PLUGIN_PATH="${CMAKE_BINARY_DIR}/plugins"
    QS_QSCTL_BINARY="$<TARGET_FILE:qsctl>")

# The network module against the daemon the desktop is really using. Read-only, and it needs no compositor and
# no display: it asks NetworkManager — through `busctl`, a different program from the Qt D-Bus the module reads
# it with — what the manager, the active connection, the device and its access point say, and compares the
# module's reading with those answers. A machine with no system bus or no NetworkManager skips it with the
# reason; there is no opt-in because there is nothing here to consent to.
#
# It is deliberately not in the slot-order checks: a pass is a handful of process launches and a walk of the real
# bus, and what it proves is a property of the machine rather than of the module's slot interactions.
add_executable(network-live-test integration/network_live_test.cpp)
target_link_libraries(network-live-test PRIVATE Qt6::Test quantum-shell-network)

# The audio module against a real PipeWire daemon: the test writes the daemon's configuration into a scratch
# directory, starts a second daemon under its own core name so its socket can never be the session's, and
# drives the module through the three things only a daemon can show — that an external volume change arrives
# as an event, that the shell's own write reaches the sink, and that a daemon going away and coming back is
# survived. It needs no compositor and no display, so its opt-in is its own rather than niri's.
#
# It is deliberately not in the slot-order checks. Those walk the unit binaries and the two read-only niri
# live tests, whose whole pass costs a fraction of a second; this one starts a daemon, and ninety-six passes
# of it would be ninety-six daemons. `QS_AUDIO_TESTS` is what makes it run at all, and a plain test run
# leaves no second audio daemon on the machine of whoever started it.
add_executable(audio-live-test integration/audio_live_test.cpp)
target_link_libraries(audio-live-test PRIVATE Qt6::Test quantum-shell-audio)

# --- slot order -----------------------------------------------------------------------------------
#
# Two checks over the same binaries, because they catch the same bug — a slot whose wait is already
# satisfied by state an earlier slot left behind, which passes in declaration order while testing nothing
# — from opposite sides. Three cases of it were found in niri-service-test by hand before either existed.
#
#   slot-order-independence      deterministic: every slot on its own in a fresh process, and each
#                                binary's whole list once in reverse declaration order. Trustworthy run
#                                after run.
#   slot-order-randomised-shard-* a seeded shuffle, different orders from run to run, which reaches
#                                orders neither of the deterministic ones uses. Reproducible from the seed
#                                it prints; see tests/run_shuffled_order.cmake.
#
# The shuffled check is several ctest tests rather than one, because that is what pays for its pass count:
# ctest runs them at once when it is asked to (the test presets set execution.jobs), so N shards on N cores
# cost the wall clock of the slowest shard instead of the sum, and the check orders every binary four times
# as often as it did as a single test. The split is by binary and not by pass on purpose: how a binary's
# slots interact is a property of that binary's own orderings, so all of a binary's passes belong in one
# process, and a shard holding half of them could report a figure for neither half. Which binary goes to
# which shard is decided below from a measured cost per pass.
#
# The list comes from the targets `qs_unit_test` recorded, so a unit test binary cannot be left out. The
# two read-only live tests join in when they are registered: a slot of either only reads the compositor,
# so running it on its own or out of order is no more than running the suite. The two tests that act on
# the session are deliberately not here — reordering those would change the desktop more times than
# their opt-in asks for, so whether their slots stand alone is a matter for reading them.
get_property(QS_ORDER_CHECK_TARGETS GLOBAL PROPERTY QS_UNIT_TEST_TARGETS)
# The read-only live tests, named in one place: they join the check when there is a compositor to read, and
# they are also what tells the cost table below whether a target it names is one this check could ever run.
set(QS_ORDER_CHECK_READ_ONLY_LIVE niri-live-test niri-live-stream-test)
set(QS_NIRI_SOCKET "$ENV{NIRI_SOCKET}")
if(NOT QS_NIRI_SOCKET STREQUAL "")
    list(APPEND QS_ORDER_CHECK_TARGETS ${QS_ORDER_CHECK_READ_ONLY_LIVE})
endif()
get_property(QS_UNIT_TEST_TARGETS GLOBAL PROPERTY QS_UNIT_TEST_TARGETS)

# A target in tests/unit that `qs_unit_test` did not create is not in the list above, so both checks
# would never run it — a silent hole in checks whose whole purpose is finding silent holes, which is a
# worse failure than no check at all. The guard lives here, after `add_subdirectory(unit)` has finished,
# so every target that file defines is visible to it however far down the file it was written.
get_property(QS_UNIT_DIR_TARGETS DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/unit" PROPERTY BUILDSYSTEM_TARGETS)
foreach(qs_target IN LISTS QS_UNIT_DIR_TARGETS)
    if(NOT qs_target IN_LIST QS_ORDER_CHECK_TARGETS)
        message(FATAL_ERROR
            "${qs_target} is a target in tests/unit that qs_unit_test did not create, so the "
            "slot-order checks would never run it. Create it with qs_unit_test.")
    endif()
endforeach()

set(QS_ORDER_CHECK_BINARIES "")
foreach(qs_target IN LISTS QS_ORDER_CHECK_TARGETS)
    list(APPEND QS_ORDER_CHECK_BINARIES "$<TARGET_FILE:${qs_target}>")
endforeach()
# Joined with `|` rather than `;`, because a semicolon in an add_test argument is a list separator and
# the script would receive the paths as several arguments. Both checks take the same list.
set(QS_ORDER_CHECK_ARGS "-DTESTS=$<JOIN:${QS_ORDER_CHECK_BINARIES},|>")

add_test(NAME slot-order-independence
    COMMAND ${CMAKE_COMMAND} "${QS_ORDER_CHECK_ARGS}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_reordered.cmake)

set_tests_properties(slot-order-independence PROPERTIES TIMEOUT 900)

# --- how the shuffled check's binaries are split into shards ------------------------------------------
#
# One measurement per binary: the wall clock in milliseconds of running it once with all of its slots in
# declared order, which is what one pass of the shuffled check costs it. The binaries are not equally
# expensive — niri-ipc-test's waits are the bulk of a pass, niri-state-test's slots are pure computation —
# and an unbalanced split hands straight back the wall clock the sharding was done to save, so the
# partition below is built from these numbers rather than from the order the targets happen to be in.
#
# Re-measure one with:  time <binary> $(<binary> -functions | tr -d '()')
set(QS_ORDER_CHECK_COSTS
    ipc-server-test=308
    ipc-capabilities-test=81
    ipc-protocol-test=22
    app-logging-test=20
    snapshot-reconcile-test=615
    # Four re-measured together, because a stale table is an unbalanced split that fails nothing: these
    # binaries gained slots in later landings and a pass grew with them — niri-event-stream-test from 359 ms
    # to 883 ms, niri-ipc-test from 925 to 1028, config-watcher-test from 625 to 724, niri-actions-test from
    # 467 to 630. The cost was a shard taking 1.6× the quickest where the split below had predicted 1.0×.
    niri-ipc-test=1028
    niri-outputs-test=660
    config-watcher-test=724
    niri-service-test=503
    niri-actions-test=630
    niri-reconnect-test=437
    niri-event-stream-test=883
    niri-live-test=213
    niri-live-stream-test=114
    sysmon-test=788
    config-test=17
    audio-test=11
    # Pure functions of D-Bus payloads and no I/O at all, which is why it sits beside `audio-test` rather than
    # beside the binaries that open something. Measured as the median of five passes on this machine: 9, 9, 8,
    # 8, 8 ms.
    battery-test=8
    # Pure functions of MPRIS2 D-Bus payloads: metadata unwrap, playback status tokens, every refusal. No
    # daemon, no bus, no display — like battery-test, which is why it sits beside it. Measured as the median
    # of ten passes on this machine: 25, 26, 27, 27, 27, 29, 29, 34, 41 ms; 29 ms chosen (average of the two
    # middle values, excluding the 41 ms outlier).
    media-test=29
    # A bus of its own, and it is `NotificationBus` — the header in tests/support — so this binary and
    # `bar-interaction-test` start the same daemon the same way rather than two spellings of it: a
    # `dbus-daemon` with a config this process writes, naming a service directory of its own holding the
    # shell's service file, so the name resolves to this shell and not the desktop's notifier (a service file
    # using `SystemdService=` is not redirected by `XDG_DATA_DIRS`, which is why `--session` is not enough).
    # Most of a pass is that daemon starting and the address being read from it; the rest is the one slot
    # that waits out a bus-delayed demotion signal, which is why the binary is a half-second rather than
    # the tens of milliseconds the pure-D-Bus-payload binaries cost. Measured as the median of seven
    # passes on this machine: 521, 522, 520, 521, 521, 521, 521 ms. Re-measured once that slot stopped
    # assuming the desktop's sender can run and started probing it first — the probe is one more process
    # launch a pass, so the figure moved by about what a launch costs: 524 ms, the middle of six passes
    # on this machine that came out at 522, 524, 524 with the sender unusable and 524, 530, 524 with it
    # working, which is one figure for both because the difference is inside the spread. Re-measured once
    # the bus moved into that shared header, where the config is written per process instead of copied out
    # of the fixtures: 522 ms, the middle of seven passes at 523, 524, 523, 522, 522, 522 and 521 — the same
    # work either way, so the difference is the spread rather than the change. Re-measured once the daemon
    # learned to publish itself on a handover, which is one more name grant, one more `Notify` round trip and
    # the waits around them in a pass: 528 ms, the middle of seven passes at 526, 527, 527, 528, 528, 529 and
    # 529 — six milliseconds of real work, not a wider spread.
    notification-test=528
    # Reads seven files and compares them with constants compiled into it, plus the meta-objects of the four
    # QML singletons whose classes it links; it does no I/O beyond the read and no work that depends on the
    # machine, which is why it is near the cheapest binary in the table. Measured as the middle of three runs:
    # 13, 15 and 14 ms. It was 71 ms while it linked `quantum-shell-wayland` for the bar window's row, and
    # that same build times 72, 74 and 74 ms today against 13, 15 and 14 without it: the whole difference is
    # that library being loaded and its font caches built, which is the cost the sanitizer job made too high
    # to keep. The row is read from its header instead, for the reason tests/unit gives at the link line.
    spec-values-test=14
    niri-state-test=11
    niri-keyboard-layouts-test=10
    niri-version-test=10
    niri-output-test=9
    # Re-measured three times on this machine, and each time because the number was wrong for a reason
    # rather than merely old. 498 ms was a pass under the session's `QT_QPA_PLATFORM=wayland`, which this
    # binary used to inherit: offscreen, which is what it has always claimed to be and what CI runs it as,
    # costs 345 ms. It then gained the two configuration slots of the system status — one of which waits
    # for a real CPU reading before it can compare a form — and a pass was 445 ms. Three more slots arrived
    # with the volume readout, one of which waits out two hidings and two showings of a widget whose width
    # is fed by font metrics. The three were re-measured once more against the built binaries rather than
    # carried over — a pass of `bar-interaction-test` is 599 ms, of `config-test` 17 ms, and of the new
    # `audio-test` 11 ms, each taken as the middle of three runs — because the earlier figures had been
    # written from an intermediate build and a cost table is only worth what its numbers are.
    # 599 was then carried over across the landing that grew this binary most: the system status got a `/proc`
    # of the test's own — the slots that assert a form now write the numbers they assert and read them back
    # through the widget — and beside them arrived the one slot that reads the *machine's* files, because a
    # fixture proves the parsing and the binding and something has to prove that the reading is real. Measured
    # one slot at a time against the built binary: a single system-status slot costs 140 ms with the window and
    # the QML loaded, the fixture work is under that measurement's resolution, and the slot that reads the
    # machine's `/proc` costs 392 ms — two readings of a real `/proc/stat`, two of its `/proc/meminfo`, and
    # 250 ms of waits inside it. A pass is 870 ms now, the middle of three runs of this binary.
    # Re-measured in the network landing, because placing three more widgets in the bar's trailing group and
    # asserting their room moved the layout this test drives: a pass was 1046 ms against the 870 it used to be,
    # the middle of three runs. Re-measured again once the two slots' assertions that landing had deleted were
    # restored — the arrangement slot's ordering comparisons and the network slot's group-width sums — because a
    # table written against the binary that was left behind describes one that no longer exists: a pass is 1029
    # ms, the middle of three runs of the restored binary that came out at 1029, 1030 and 1028. A stale figure
    # here is an unbalanced split that fails nothing.
    #
    # Then re-measured again for the landing that gave this binary the notification readout end to end, and that
    # is the one that moved it most since the fixtures: this binary now starts a `dbus-daemon` of its own
    # (`NotificationBus`, the same harness `notification-test` uses) and drives the shell as the desktop's
    # notification daemon — two bring-ups, two `Notify` round trips and the widget waits that follow them — so a
    # pass is 1192 ms, the middle of seven runs that came out at 1187, 1189, 1193, 1192, 1195, 1193 and 1191.
    bar-interaction-test=1192
    # The network module's test, measured the same way: 2364 ms, the middle of three runs that came out at 2363,
    # 2364 and 2370 — a spread of seven milliseconds, which is why one figure is enough. It is the most expensive
    # unit pass here and the reason is honest rather than incidental: it starts a bus daemon of its own, walks a
    # real D-Bus connection four objects deep, and deliberately holds a reply in flight for 150 ms to catch the
    # one race a fake daemon can arrange and a real one cannot be asked for.
    network-test=2364)
set(QS_ORDER_CHECK_SHARDS 4)

# The table, checked in both directions before it is used. A target of the check with no measured cost
# would have to be placed on a guess, which is how a partition goes quietly lopsided; an entry naming
# something the check never runs is a measurement of nothing. Both are configure-time errors rather than
# warnings in a log nobody reads.
foreach(entry IN LISTS QS_ORDER_CHECK_COSTS)
    string(FIND "${entry}" "=" separator)
    if(separator EQUAL -1)
        message(FATAL_ERROR "${entry} is not a <target>=<milliseconds> pass cost")
    endif()
    string(SUBSTRING "${entry}" 0 ${separator} entry_target)
    if(NOT entry_target IN_LIST QS_UNIT_TEST_TARGETS AND
       NOT entry_target IN_LIST QS_ORDER_CHECK_READ_ONLY_LIVE)
        message(FATAL_ERROR
            "${entry_target} has a measured pass cost but is not a binary the shuffled slot-order check "
            "can hold, so that measurement is of nothing this check runs")
    endif()
endforeach()

# Heaviest first, then each binary to whichever shard is currently cheapest: a greedy split, not the
# optimal one, and within a few per cent of it on costs that differ by two orders of magnitude. The cost
# is zero-padded before sorting because list(SORT) compares text, and "925" sorts above "1000".
set(qs_ordered "")
foreach(target IN LISTS QS_ORDER_CHECK_TARGETS)
    set(cost "")
    foreach(entry IN LISTS QS_ORDER_CHECK_COSTS)
        string(FIND "${entry}" "=" separator)
        string(SUBSTRING "${entry}" 0 ${separator} entry_target)
        if(entry_target STREQUAL target)
            math(EXPR after "${separator} + 1")
            string(SUBSTRING "${entry}" ${after} -1 cost)
        endif()
    endforeach()
    if(cost STREQUAL "")
        message(FATAL_ERROR
            "${target} is part of the shuffled slot-order check but has no measured pass cost in "
            "QS_ORDER_CHECK_COSTS, and placing it without one would unbalance the shards. Measure it — "
            "`time <binary> $(<binary> -functions | tr -d '()')` — and add \"${target}=<milliseconds>\" "
            "there.")
    endif()
    string(LENGTH "${cost}" digits)
    math(EXPR padding "6 - ${digits}")
    string(REPEAT "0" ${padding} zeros)
    list(APPEND qs_ordered "${zeros}${cost}|${target}")
endforeach()
list(SORT qs_ordered)
list(REVERSE qs_ordered)

foreach(shard RANGE 1 ${QS_ORDER_CHECK_SHARDS})
    set(qs_shard_targets_${shard} "")
    set(qs_shard_cost_${shard} 0)
endforeach()
foreach(entry IN LISTS qs_ordered)
    string(FIND "${entry}" "|" bar)
    math(EXPR after "${bar} + 1")
    string(SUBSTRING "${entry}" ${after} -1 target)
    string(SUBSTRING "${entry}" 0 ${bar} padded_cost)
    string(REGEX REPLACE "^0+" "" cost "${padded_cost}")

    set(cheapest 1)
    foreach(shard RANGE 2 ${QS_ORDER_CHECK_SHARDS})
        if(qs_shard_cost_${shard} LESS qs_shard_cost_${cheapest})
            set(cheapest ${shard})
        endif()
    endforeach()
    list(APPEND qs_shard_targets_${cheapest} "${target}")
    math(EXPR running "${qs_shard_cost_${cheapest}} + ${cost}")
    set(qs_shard_cost_${cheapest} "${running}")
endforeach()

# The partition, checked. A target in no shard would be ordered by nothing at all while the rest of the
# check stayed green, one in two shards would be ordered twice, and an empty shard is the same mistake
# from the other side — a test reporting success having ordered nothing. None of those is visible in a
# passing run, so none of them is left to a passing run.
set(qs_assigned "")
foreach(shard RANGE 1 ${QS_ORDER_CHECK_SHARDS})
    if(qs_shard_targets_${shard} STREQUAL "")
        message(FATAL_ERROR
            "shard ${shard} of the shuffled slot-order check was given no binaries, so it would report "
            "success having ordered nothing")
    endif()
    list(APPEND qs_assigned ${qs_shard_targets_${shard}})
endforeach()
set(qs_assigned_sorted "${qs_assigned}")
list(SORT qs_assigned_sorted)
set(qs_expected_sorted "${QS_ORDER_CHECK_TARGETS}")
list(SORT qs_expected_sorted)
if(NOT qs_assigned_sorted STREQUAL qs_expected_sorted)
    message(FATAL_ERROR
        "the shuffled slot-order check's shards hold '${qs_assigned_sorted}' but the check is over "
        "'${qs_expected_sorted}', so the two disagree about which binaries get ordered")
endif()

# Reported as it is made, because the costs behind this are measurements and a stale one shows up here as
# a lopsided split rather than as a check that quietly takes longer. The figure to read is the ratio: the
# wall clock of the whole check is the slowest shard's, not the sum of the four.
set(qs_slowest 0)
set(qs_quickest 0)
set(qs_shard_costs_text "")
foreach(shard RANGE 1 ${QS_ORDER_CHECK_SHARDS})
    list(APPEND qs_shard_costs_text "${qs_shard_cost_${shard}} ms")
    if(qs_shard_cost_${shard} GREATER qs_slowest)
        set(qs_slowest ${qs_shard_cost_${shard}})
    endif()
    if(qs_quickest EQUAL 0 OR qs_shard_cost_${shard} LESS qs_quickest)
        set(qs_quickest ${qs_shard_cost_${shard}})
    endif()
endforeach()
list(JOIN qs_shard_costs_text ", " qs_shard_costs_text)
list(LENGTH QS_ORDER_CHECK_TARGETS qs_order_check_target_count)
math(EXPR qs_spread "100 * ${qs_slowest} / ${qs_quickest}")

foreach(shard RANGE 1 ${QS_ORDER_CHECK_SHARDS})
    set(qs_shard_binaries "")
    foreach(target IN LISTS qs_shard_targets_${shard})
        list(APPEND qs_shard_binaries "$<TARGET_FILE:${target}>")
    endforeach()
    # The seed is random per run and printed, so a failure names the seed to re-run it with; the pass count
    # is chosen so that the run's triples reach all six of their relative orders, and how far it got is
    # reported rather than assumed. Both are overridable in the environment — the script reads
    # QS_TEST_ORDER_SEED and QS_TEST_ORDER_PASSES — and QS_TEST_ORDER_SEED pins every shard at once. TIMEOUT
    # is generous because a shard orders its own binaries ninety-six times each: about 100 s for the
    # slowest shard here, which is the budget that figure was chosen against.
    add_test(NAME slot-order-randomised-shard-${shard}
        COMMAND ${CMAKE_COMMAND}
            "-DTESTS=$<JOIN:${qs_shard_binaries},|>"
            "-DSHARD_INDEX=${shard}"
            "-DSHARD_COUNT=${QS_ORDER_CHECK_SHARDS}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_shuffled_order.cmake)
    set_tests_properties(slot-order-randomised-shard-${shard} PROPERTIES TIMEOUT 900)
endforeach()

message(STATUS
    "shuffled slot-order check: ${QS_ORDER_CHECK_SHARDS} shards over ${qs_order_check_target_count} "
    "binaries, a pass costing ${qs_shard_costs_text}; the slowest shard is ${qs_spread}% of the quickest, "
    "and ctest runs them together where it is asked to (execution.jobs in the presets)")
if(NOT QS_NIRI_SOCKET STREQUAL "")
    add_test(NAME niri-live-test COMMAND niri-live-test)
    add_test(NAME niri-live-stream-test COMMAND niri-live-stream-test)
    message(STATUS "niri integration tests registered against ${QS_NIRI_SOCKET}")
else()
    message(STATUS "niri integration tests not registered: NIRI_SOCKET is not set (no niri session)")
endif()

# Acting on the session needs a second opt-in, so that a plain test run stays read-only.
set(QS_NIRI_SESSION_TESTS "$ENV{QS_NIRI_SESSION_TESTS}")
if(NOT QS_NIRI_SOCKET STREQUAL "" AND NOT QS_NIRI_SESSION_TESTS STREQUAL "")
    add_test(NAME niri-live-action-test COMMAND niri-live-action-test)
    add_test(NAME niri-live-layershell-test COMMAND niri-live-layershell-test)
    # A parallel run must not have two tests acting on the desktop at once. The lock is what makes
    # execution.jobs safe locally: the shards of the shuffled check read the compositor and take no lock, so
    # they still run together, while the tests that change what is on screen wait for each other.
    set_tests_properties(niri-live-action-test PROPERTIES RESOURCE_LOCK niri-desktop)
    set_tests_properties(niri-live-layershell-test PROPERTIES RESOURCE_LOCK niri-desktop)
    message(STATUS
        "niri session tests registered: they open and close the overview on screen, and put a bar on it")
else()
    message(STATUS
        "niri session tests not registered: set QS_NIRI_SESSION_TESTS=1 to run the tests that act on "
        "the session (the overview one, and the one that maps a bar)")
endif()

# Registered on any machine: it reads, it changes nothing, and where there is no NetworkManager it skips with
# the reason rather than passing quietly.
add_test(NAME network-live-test COMMAND network-live-test)

set(QS_AUDIO_TESTS "$ENV{QS_AUDIO_TESTS}")
if(NOT QS_AUDIO_TESTS STREQUAL "")
    add_test(NAME audio-live-test COMMAND audio-live-test)
    message(STATUS
        "audio live test registered: it starts a PipeWire daemon of its own and changes no device of the"
        " session's")
else()
    message(STATUS
        "audio live test not registered: set QS_AUDIO_TESTS=1 to run the test that starts a PipeWire "
        "daemon of its own")
endif()

# Restarting a compositor needs its own opt-in for the same reason, and one more: it starts and stops a
# compositor of its own, nested in the session.
set(QS_NIRI_RESTART_TESTS "$ENV{QS_NIRI_RESTART_TESTS}")
if(NOT QS_NIRI_SOCKET STREQUAL "" AND NOT QS_NIRI_RESTART_TESTS STREQUAL "")
    add_test(NAME niri-live-restart-test COMMAND niri-live-restart-test)
    set_tests_properties(niri-live-restart-test PROPERTIES RESOURCE_LOCK niri-desktop)
    message(STATUS "niri restart test registered: it starts a niri of its own and restarts it")
    add_test(NAME niri-live-shell-restart-test COMMAND niri-live-shell-restart-test)
    set_tests_properties(niri-live-shell-restart-test PROPERTIES RESOURCE_LOCK niri-desktop)
    message(STATUS "niri shell restart test registered: it drives the built shell across a compositor restart")
else()
    message(STATUS
        "niri restart test not registered: set QS_NIRI_RESTART_TESTS=1 to run the test that starts "
        "and restarts a compositor of its own")
endif()

# --- the project's public names -------------------------------------------------------------------
#
# tests/public_names.cmake declares the test names and environment variables this project presents to
# whoever runs it, and the two checks below read it. They exist because a stale name here fails nothing
# by itself: `ctest -R` that matches nothing prints "No tests were found!!!" and exits 0, so a document
# can keep telling people to run a check that no longer exists with every run still green.
include(${CMAKE_CURRENT_SOURCE_DIR}/public_names.cmake)

string(REPLACE ";" "|" QS_TEST_NAMES_ARG "${QS_TEST_NAMES}")
string(REPLACE ";" "|" QS_ENVIRONMENT_NAMES_ARG "${QS_ENVIRONMENT_NAMES}")
# Every document that tells someone to run a test by name is read here, which is why adding a document is
# an edit to this line: a fourth document whose names nothing checks is the document that lies. The
# engineering spec is derived from the code, so its names are the ones most likely to be left behind by a
# rename, and it is on the list for exactly that reason.
set(QS_PUBLIC_DOCUMENTS "${CMAKE_SOURCE_DIR}/AGENTS.md|${CMAKE_SOURCE_DIR}/QUANTUM_SHELL.md|${CMAKE_SOURCE_DIR}/ENGINEERING_SPEC.md")

add_test(NAME public-names-test
    COMMAND ${CMAKE_COMMAND}
        -DROOT=${CMAKE_SOURCE_DIR}
        "-DTEST_NAMES=${QS_TEST_NAMES_ARG}"
        "-DENVIRONMENT_NAMES=${QS_ENVIRONMENT_NAMES_ARG}"
        "-DDOCUMENTS=${QS_PUBLIC_DOCUMENTS}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_public_names.cmake)
# The other half — a test registered without being declared — is checked from the root build file, as a
# deferred call, so that it runs after every registration in the project rather than after the ones
# written above it. See the note there.

