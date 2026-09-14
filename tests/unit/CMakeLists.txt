# Unit tests for the niri IPC client, the event stream and the state model it feeds.
#
# None of these needs a compositor: the compositor's end of the socket is FakeNiriServer, a test double
# defined here beside the tests that use it, so they run on any machine and in CI.
#
# Every binary here is created through `qs_unit_test`, which records it as it goes: the check in
# tests/CMakeLists.txt runs every slot of every one of these on its own, to prove that no slot passes
# only because an earlier slot left state behind. A binary created some other way would be left out of
# that silently, so there is a guard at the bottom of this file that fails the configure instead.

function(qs_unit_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE Qt6::Test quantum-shell-niri)
    add_test(NAME ${name} COMMAND ${name})
    set_property(GLOBAL APPEND PROPERTY QS_UNIT_TEST_TARGETS ${name})
endfunction()

qs_unit_test(niri-version-test niri_version_test.cpp)

qs_unit_test(niri-ipc-test
    niri_ipc_test.cpp
    FakeNiriServer.cpp)

qs_unit_test(niri-event-stream-test
    niri_event_stream_test.cpp
    FakeNiriServer.cpp)

qs_unit_test(niri-state-test niri_state_test.cpp)

qs_unit_test(niri-actions-test
    niri_actions_test.cpp
    FakeNiriServer.cpp)

qs_unit_test(niri-output-test niri_output_test.cpp)

qs_unit_test(niri-keyboard-layouts-test niri_keyboard_layouts_test.cpp)

qs_unit_test(niri-outputs-test
    niri_outputs_test.cpp
    FakeNiriServer.cpp)

# The QObject service QML binds to: real values from the model, notify signals that fire only on a
# real change, and a binding evaluated in a real QML engine with no display.
qs_unit_test(niri-service-test
    niri_service_test.cpp
    FakeNiriServer.cpp)

# Keeping the connections attached: socket discovery, backoff, and rebuilding state after the
# compositor it was built from is gone. Its restart is a second FakeNiriServer on a new path.
qs_unit_test(niri-reconnect-test
    niri_reconnect_test.cpp
    FakeNiriServer.cpp)

# The configuration schema, the QObject tree QML binds to, and the watcher that keeps the two in step
# with the file. Nothing here needs a compositor or a display: the schema is a pure function of a string,
# the tree's diffing is driven by applying values, and the watcher is given a path inside a scratch
# directory rather than the one the person running the test has.
qs_unit_test(config-test config_test.cpp)
target_link_libraries(config-test PRIVATE quantum-shell-config)

qs_unit_test(config-watcher-test config_watcher_test.cpp)
target_link_libraries(config-watcher-test PRIVATE quantum-shell-config)

# The IPC in three pieces, none of which needs a compositor or a display. `ipc-protocol-test` is the wire
# format as pure functions; `ipc-server-test` drives the server over a real `QLocalSocket` with a test double
# for what it can reach, because the server's job is to carry an answer and not to know what one means; and
# `ipc-capabilities-test` is the other half, a real service and a real configuration behind that interface,
# fed by the same `FakeNiriServer` the niri tests use. What is left — a real shell answering a real `qsctl` —
# is the live test's, where the socket is the frozen name rather than a test's own.
qs_unit_test(ipc-protocol-test ipc_protocol_test.cpp)
target_link_libraries(ipc-protocol-test PRIVATE quantum-shell-ipc)

qs_unit_test(ipc-server-test ipc_server_test.cpp)
target_link_libraries(ipc-server-test PRIVATE quantum-shell-ipc)

qs_unit_test(ipc-capabilities-test
    ipc_capabilities_test.cpp
    FakeNiriServer.cpp)
target_link_libraries(ipc-capabilities-test PRIVATE quantum-shell-capabilities)

# The shell's logging: the format a record is written in, and the category names a person filters on. No
# socket and no display, which is the point of the facility taking no sink of its own.
qs_unit_test(app-logging-test app_logging_test.cpp)
target_link_libraries(app-logging-test PRIVATE quantum-shell-logging)

# The reconciliation the live snapshot comparisons run on. The race those comparisons guard against
# cannot be produced on demand against a real desktop — it needs a change made between two readings — so
# what is pinned here is the mechanism's contract: a reading that disagrees and then agrees is reconciled,
# the budget bounds the readings, exhaustion fails rather than passing quietly, and a model that missed an
# event is told apart from a desktop that never held still. It links the same header the live test uses.
qs_unit_test(snapshot-reconcile-test snapshot_reconcile_test.cpp)
target_link_libraries(snapshot-reconcile-test PRIVATE qs-test-support)

# A target in this directory that did not come from `qs_unit_test` is not in the list the
# order-independence check walks, so it would be checked by nothing. That is a silent hole in a check
# whose whole purpose is to find silent holes, so it fails here, at configure time, instead.
get_property(qs_recorded GLOBAL PROPERTY QS_UNIT_TEST_TARGETS)
get_property(qs_present DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(qs_target IN LISTS qs_present)
    if(NOT qs_target IN_LIST qs_recorded)
        message(FATAL_ERROR
            "${qs_target} is a target in tests/unit that qs_unit_test did not create, so the "
            "order-independence check would never run it. Create it with qs_unit_test.")
    endif()
endforeach()
