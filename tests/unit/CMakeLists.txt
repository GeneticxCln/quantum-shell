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
# Linked explicitly because a refusal has to be readable in the log as well as signalled, and the slot
# that checks the record links the same category the shell writes it on rather than a copy of the name.
target_link_libraries(niri-actions-test PRIVATE quantum-shell-logging)

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

# The bar's own gestures and layout, against the components the shell ships. What a click and a wheel mean,
# and where a widget ends up, are properties of the bar's QML and of nothing in C++, so the files the
# application packs are loaded into a real engine here: the whole bar, given real window events, with the
# requests that reach niri's end of the socket as the assertion; and the group component on its own, so its
# layout can be driven with widgets of sizes the bar itself happens not to have. Offscreen, so it needs no
# display and no session — which is what lets it run in CI and under the slot-order checks like any other
# unit test.
qs_unit_test(bar-interaction-test
    bar_interaction_test.cpp
    FakeNiriServer.cpp)
# `quantum-shell-system` as well: the bar holds the system status, and the widget's own reading of the service
# — including its empty state before a reading exists — is asserted here rather than described. And
# `quantum-shell-config`, because which readouts the centre group draws and the form the memory one takes are
# `[bar.system]`'s values: the widget is written against `Config`, so the configuration is registered for this
# engine and driven the way the watcher drives it.
# `quantum-shell-audio` and its logging category as well: the bar's volume readout is written against
# `PipeWireService`, so the service is registered for this engine — never started, which is the empty state the
# widget draws and the state that makes a click and a wheel observable — and the records it writes while
# refusing a gesture with nothing attached are what this test reads back.
# `quantum-shell-battery` for the same reason `quantum-shell-network` is here: the fifth readout is written
# against `BatteryService`, so the service is registered for this engine — never started, which is the dash the
# widget draws while the daemon has not answered — and the rules it publishes are what the slots exercise.
# `quantum-shell-dbus` arrives through the two of them rather than being named here.
#
# `qs-test-support` for `NotificationBus.h`, and the notification daemon is the second service here that is
# *driven* rather than left in its empty state: `qml/Notifications.qml` reads `NotificationService`, and what
# the readout draws is a sender's text arriving over a real bus. So this binary starts a `dbus-daemon` of its
# own (the same harness `notification-test` uses), takes the notifications name with the service, hands it
# back at the end of the slot that asked, and — with a real reading in hand — tells the readout's
# configuration slot apart from one that would pass with the flag ignored.
target_link_libraries(bar-interaction-test PRIVATE Qt6::Quick quantum-shell-system quantum-shell-config quantum-shell-notifications
                                                 quantum-shell-audio quantum-shell-network
                                                 quantum-shell-battery quantum-shell-media quantum-shell-logging
                                                 qs-test-support)
# The components are taken out of the one list the application's resources are built from, so there is no
# second copy of them to keep in step and no name written down twice. They are read from the source tree
# rather than from a resource of this binary because that resource set belongs to the application; that the
# packed copies load in a real shell is what `niri-live-layershell-test` asserts, on the built binary.
#
# Two of them, and the fatal errors are separate rather than one loop with a flag: a `QS_QML_FILES` that
# stopped naming either file is the thing this guard exists for, and the message has to say which.
foreach(qs_qml IN LISTS QS_QML_FILES)
    if(qs_qml MATCHES "Bar\\.qml$" AND NOT qs_qml MATCHES "CapsuleGroup\\.qml$")
        set(QS_BAR_QML "${CMAKE_SOURCE_DIR}/${qs_qml}")
    elseif(qs_qml MATCHES "CapsuleGroup\\.qml$")
        set(QS_GROUP_QML "${CMAKE_SOURCE_DIR}/${qs_qml}")
    endif()
endforeach()
if(NOT DEFINED QS_BAR_QML)
    message(FATAL_ERROR
        "no Bar.qml is named in QS_QML_FILES, so bar-interaction-test would have no bar to load and would "
        "fail at run time instead of here.")
endif()
if(NOT DEFINED QS_GROUP_QML)
    message(FATAL_ERROR
        "no CapsuleGroup.qml is named in QS_QML_FILES, so bar-interaction-test would have no group to drive "
        "and would fail at run time instead of here.")
endif()
target_compile_definitions(bar-interaction-test PRIVATE
    QS_BAR_QML="${QS_BAR_QML}"
    QS_GROUP_QML="${QS_GROUP_QML}")
# Where `NotificationBus` takes the shell's notification service file from. The same fixtures directory
# `notification-test` is given, because it is the same file: the name a sender reaching that bus resolves.
target_compile_definitions(bar-interaction-test PRIVATE FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")

# The system's own readings: the /proc lines they come from, what a refusal looks like, and the cadence the
# service owns. The service takes the directory it reads from, so the parsing is driven by files this test
# writes — every shape of input, including the ones the kernel does not produce — and two slots read the real
# /proc as well, because a parser only ever handed this test's own strings agrees with this test and nothing
# else. `quantum-shell-logging` because a refusal is only a report if the record can be read.
qs_unit_test(sysmon-test sysmon_test.cpp)
target_link_libraries(sysmon-test PRIVATE quantum-shell-system quantum-shell-logging)

# The audio module's pure half: the Props parse, the percentage convention, the wheel's arithmetic, the
# metadata value, and the pod a write is. No daemon, no socket and no display — the cases here include the
# shapes a healthy daemon never sends, which is the half a live test cannot reach. `quantum-shell-audio`
# carries the functions; the tests build their own pods with SPA's own builder, which is why they need the
# same header path the library compiles against — as SYSTEM, for the reason stated at that target.
qs_unit_test(audio-test audio_test.cpp)
target_link_libraries(audio-test PRIVATE quantum-shell-audio)
target_include_directories(audio-test SYSTEM PRIVATE ${PIPEWIRE_INCLUDE_DIRS})

# The network readout's module: what NetworkManager publishes, read two ways. The first half needs nothing at
# all — the D-Bus names, the daemon's numbers as tokens and the reading the four objects of the chain add up to
# are functions of the payloads. The second half drives the service against a fake daemon on a bus this test
# starts for itself, which is what makes the shell's *reading* checkable against payloads it did not choose and
# its *asking* countable. `quantum-shell-logging` because a refusal is only a report if the record can be read.
qs_unit_test(network-test network_test.cpp)
target_link_libraries(network-test PRIVATE quantum-shell-network quantum-shell-logging)

# The battery readout's pure half: UPower's D-Bus protocol as this shell reads it. Every name and number is
# declared once in `BatteryStatus.h` and mirrored at compile time, the daemon's state and warning numbers are
# mapped to tokens, and the reading the two objects add up to is a function of the payloads. No daemon, no
# bus and no display — the cases here include the shapes a healthy daemon never sends, which is the half a
# live test cannot reach. `quantum-shell-battery` carries the functions.
qs_unit_test(battery-test battery_test.cpp)
target_link_libraries(battery-test PRIVATE quantum-shell-battery)

# The media readout's pure half: MPRIS2 D-Bus protocol as this shell reads it. Metadata unwrap, the
# playback status tokens the widget switches on, and every refusal the parser produces. No daemon, no bus
# and no display — the cases here include the shapes a healthy player never sends, which is the half a live
# test cannot reach. `quantum-shell-media` carries the functions.
qs_unit_test(media-test media_test.cpp)
target_link_libraries(media-test PRIVATE quantum-shell-media)
qs_unit_test(notification-test notification_test.cpp)
# `qs-test-support` for `NotificationBus.h`: the private `dbus-daemon` this test's daemon registers on, which
# `bar-interaction-test` drives the same way — one spelling of the bus config, the service directory it names
# and the address the daemon prints, rather than two that can drift.
target_link_libraries(notification-test PRIVATE quantum-shell-notifications qs-test-support)
target_compile_definitions(notification-test PRIVATE FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")

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

# The engineering spec's numbers against the code's: the version floors, the config defaults and bounds, the
# frozen names, the connection schedules and the suite counts, read out of `ENGINEERING_SPEC.md` and compared
# with the constants this binary is linked against. No display, no session, no daemon — it reads files.
#
# It links the libraries whose constants it compares rather than parsing headers, which is the difference
# between this and a mirror: a renamed or moved constant stops the build here rather than being read out of a
# header by a regular expression that happened to still match.
qs_unit_test(spec-values-test spec_values_test.cpp)
target_link_libraries(spec-values-test PRIVATE
    quantum-shell-config quantum-shell-notifications
    quantum-shell-system
    quantum-shell-audio
    quantum-shell-network
    quantum-shell-battery
    quantum-shell-media
    quantum-shell-ipc)
# `quantum-shell-wayland` is deliberately *not* linked, and the reason is measured rather than a preference.
# The bar's window derives from `QQuickWindow`, so including its header and linking it here would drag Qt
# Quick and the font stack into a test whose subject is a document — and under the sanitizer job that link
# makes this binary report the font stack's process-lifetime fontconfig caches as leaks and exit nonzero
# (722474 bytes in 16702 allocations, every frame in libfontconfig or libpangocairo and none in this
# repository), while the same binary without it exits clean. The bar window's row is read from its header
# instead: the weaker source, which catches a renamed or added declaration and says so where it reads it.
# The files it reads, named at configure time and checked for here, so a moved document fails the configure
# rather than being found by a test that searched for it. `bar-interaction-test` takes the shipped QML the
# same way and for the same reason.
foreach(qs_spec_file IN ITEMS
        ENGINEERING_SPEC.md
        CMakeLists.txt
        tests/public_names.cmake
        tests/CMakeLists.txt
        src/system/SysMonService.cpp
        src/audio/PipeWireService.cpp
        src/dbus/NetworkService.cpp
        src/wayland/LayerShellWindow.h)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${qs_spec_file}")
        message(FATAL_ERROR
            "spec-values-test reads ${qs_spec_file}, which is not in this source tree, so the document's "
            "claims about it would go unchecked instead of failing.")
    endif()
endforeach()
target_compile_definitions(spec-values-test PRIVATE QS_SPEC_ROOT="${CMAKE_SOURCE_DIR}")

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
