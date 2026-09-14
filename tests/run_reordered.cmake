# Proves that each test's slots stand alone.
#
# The bug this catches: a slot whose QTRY wait is already satisfied by state an earlier slot left
# behind. The wait then does not wait at all, the assertions that follow read the earlier slot's data,
# and the suite is green in declaration order while asserting nothing about what it claims to. Two slots
# in this suite were found that way by hand, which is why the check is now automatic.
#
# Two orders, because they catch it from different sides:
#
#   - every slot on its own, in a fresh process. A slot that only passes when another slot has run
#     first has a dependency it should not have, and this is the order that leaves no room for one.
#   - the whole list reversed. Cheap, and it moves the slots that run last to the front, which is where
#     a leftover-state dependency becomes visible as a failure rather than as a silently satisfied wait.
#
# QtTest runs the slots named on its command line in the order given, and each named slot runs with
# initTestCase in a fresh process, so neither order needs anything from the tests themselves.
if(NOT DEFINED TESTS)
    message(FATAL_ERROR "run_reordered.cmake requires TESTS (the Qt test binaries to check)")
endif()

# The caller joins the binaries with `|`, so that one -D argument can carry the whole list without a
# semicolon turning it into several arguments.
string(REPLACE "|" ";" TESTS "${TESTS}")

include("${CMAKE_CURRENT_LIST_DIR}/slot_list.cmake")

set(failures "")
set(checked 0)

foreach(binary IN LISTS TESTS)
    qs_slots_of("${binary}" slots)

    list(LENGTH slots slot_count)
    message(STATUS "checking ${binary}: ${slot_count} slots")

    # Reversed declaration order, one process.
    set(reversed "${slots}")
    list(REVERSE reversed)
    execute_process(
        COMMAND "${binary}" ${reversed}
        RESULT_VARIABLE code
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output
        TIMEOUT 300)
    math(EXPR checked "${checked} + 1")
    if(NOT code EQUAL 0)
        list(APPEND failures "${binary} in reverse order: exit ${code}\n${output}${error_output}")
    endif()

    # Each slot alone, in its own process.
    foreach(slot IN LISTS slots)
        execute_process(
            COMMAND "${binary}" "${slot}"
            RESULT_VARIABLE code
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error_output
            TIMEOUT 300)
        math(EXPR checked "${checked} + 1")
        if(NOT code EQUAL 0)
            list(APPEND failures "${binary} ${slot} alone: exit ${code}\n${output}${error_output}")
        endif()
    endforeach()
endforeach()

if(failures)
    string(REPLACE ";" "\n  - " readable "${failures}")
    message(FATAL_ERROR
        "${checked} runs checked, and these did not stand alone:\n  - ${readable}")
endif()

message(STATUS "order independence: ${checked} runs, every slot passes alone and in reverse")
