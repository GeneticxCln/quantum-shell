# The names this project presents to whoever runs its tests, declared once.
#
# `ctest -R <name>` that matches nothing prints "No tests were found!!!" and still exits 0. That makes a
# renamed test invisible in the worst way: every command in AGENTS.md and QUANTUM_SHELL.md that names it
# keeps reporting success while running nothing at all. The same is true of the environment variables
# below — rename one and the opt-in quietly stops opting in, with the suite green either way.
#
# So these names are written down here and nowhere else, and two checks read this file:
#
#   * `tests/CMakeLists.txt` fails the configure if a test is registered without being declared here, so
#     a rename cannot reach a build tree unnoticed.
#   * `public-names-test` reads this file against both documents and against the environment variables
#     the build files actually read, so a documented command that names a test nobody registers fails,
#     as does a name documented but read by nothing.
#
# Adding a test means adding it here and naming it in AGENTS.md; that is the price of a name other
# people type. A name that is stale here is a name the documentation no longer runs.

set(QS_TEST_NAMES
    public-names-test
    qs-scan-self-test
    repo-scan
    slot-order-independence
    slot-order-randomised-shard-1
    slot-order-randomised-shard-2
    slot-order-randomised-shard-3
    slot-order-randomised-shard-4
    niri-live-test
    niri-live-stream-test
    niri-live-action-test
    niri-live-layershell-test
    niri-live-restart-test
    niri-live-shell-restart-test
    audio-live-test
    network-live-test
    niri-version-test
    niri-ipc-test
    niri-event-stream-test
    niri-state-test
    niri-actions-test
    niri-output-test
    niri-keyboard-layouts-test
    niri-outputs-test
    niri-service-test
    niri-reconnect-test
    config-test
    config-watcher-test
    sysmon-test
    audio-test
    network-test
    battery-test
    media-test
    notification-test
    ipc-protocol-test
    ipc-server-test
    ipc-capabilities-test
    app-logging-test
    snapshot-reconcile-test
    spec-values-test
    bar-interaction-test)

# The environment variables that change what a test run does. `NIRI_SOCKET` is niri's own and is read to
# decide whether there is a compositor to talk to at all; the rest are this project's opt-ins.
set(QS_ENVIRONMENT_NAMES
    NIRI_SOCKET
    QS_NIRI_SESSION_TESTS
    QS_NIRI_RESTART_TESTS
    QS_AUDIO_TESTS
    QS_TEST_ORDER_SEED
    QS_TEST_ORDER_COVERAGE
    QS_TEST_ORDER_PASSES
    QS_MIN_PAIR_COVERAGE
    QS_WORST_TRIPLES_SHOWN
    QS_WORST_QUADS_SHOWN)

# --- the guard that reads the lists above -----------------------------------------------------------
#
# Every test the build registers has to be declared above. The check walks the directory tree rather than
# a written-down list of directories, so a test registered in a directory that does not exist yet is
# covered the moment that directory is added, and it is registered as a *deferred* call from the root
# build file so that it runs at the end of configuration: an `add_test` appended after the check was
# written would otherwise be the one registration it never looked at, and appending is exactly how a new
# test gets added.
function(qs_collect_registered_tests dir out_var)
    get_property(tests DIRECTORY "${dir}" PROPERTY TESTS)
    get_property(subdirectories DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        qs_collect_registered_tests("${subdirectory}" nested)
        list(APPEND tests ${nested})
    endforeach()
    list(REMOVE_DUPLICATES tests)
    set("${out_var}" "${tests}" PARENT_SCOPE)
endfunction()

function(qs_check_all_registered_tests_are_declared)
    qs_collect_registered_tests("${CMAKE_SOURCE_DIR}" registered)

    # A guard that finds nothing to check reports success, which is the same silent hole in another
    # place: the checks in this project exist to find those, so this one refuses to be one.
    if(registered STREQUAL "")
        message(FATAL_ERROR
            "no registered tests were found to compare against tests/public_names.cmake, so this guard "
            "would pass by finding nothing at all")
    endif()

    foreach(test IN LISTS registered)
        if(NOT test IN_LIST QS_TEST_NAMES)
            message(FATAL_ERROR
                "${test} is registered as a test but not declared in tests/public_names.cmake. Declare "
                "it there and name it in AGENTS.md: a documented `ctest -R` that matches nothing exits "
                "0, so a name registered but undeclared is one no documented command can be checked "
                "against.")
        endif()
    endforeach()
endfunction()
