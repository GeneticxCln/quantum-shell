# Black-box tests for qs-scan: each case runs the built scanner against a fixture tree and checks the
# exit code plus the parts of the output that matter. Fixtures are input data for the gate, so they
# are never compiled. A case that stops holding is a broken gate, which is why this fails loudly.
if(NOT DEFINED SCANNER OR NOT DEFINED PATTERNS OR NOT DEFINED FIXTURES)
    message(FATAL_ERROR "run_scan_tests.cmake requires SCANNER, PATTERNS and FIXTURES")
endif()

set(case_failures "")

function(scan_case label expected_exit)
    cmake_parse_arguments(CASE "" "ROOT" "CONTAINS;NOT_CONTAINS;ARGS" ${ARGN})
    if(NOT DEFINED CASE_ROOT)
        message(FATAL_ERROR "${label}: ROOT is required")
    endif()

    execute_process(
        COMMAND "${SCANNER}" --root "${CASE_ROOT}" --patterns "${PATTERNS}" ${CASE_ARGS}
        RESULT_VARIABLE exit_code
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output)

    set(problems "")
    if(NOT exit_code EQUAL expected_exit)
        list(APPEND problems "exit code ${exit_code}, expected ${expected_exit}")
    endif()
    foreach(needle IN LISTS CASE_CONTAINS)
        string(FIND "${output}${error_output}" "${needle}" position)
        if(position EQUAL -1)
            list(APPEND problems "output does not mention '${needle}'")
        endif()
    endforeach()
    foreach(needle IN LISTS CASE_NOT_CONTAINS)
        string(FIND "${output}${error_output}" "${needle}" position)
        if(NOT position EQUAL -1)
            list(APPEND problems "output unexpectedly mentions '${needle}'")
        endif()
    endforeach()

    if(problems)
        list(APPEND case_failures "${label}: ${problems}")
        set(case_failures "${case_failures}" PARENT_SCOPE)
        message(STATUS "FAIL ${label}")
        message(STATUS "  scanner output:\n${output}${error_output}")
    else()
        message(STATUS "pass ${label}")
    endif()
endfunction()

scan_case("clean tree passes" 0
    ROOT "${FIXTURES}/clean"
    CONTAINS "0 errors;scanned 1 files")

scan_case("unfinished marker, blank file and swallowed failure all fail" 1
    ROOT "${FIXTURES}/dirty"
    CONTAINS "unfinished-work;banned_marker.cpp;empty-file;empty-catch;3 errors")

scan_case("a test-double name outside tests fails, the same name under tests passes" 1
    ROOT "${FIXTURES}/scope"
    CONTAINS "stub-or-mock;src/service_header.h"
    NOT_CONTAINS "double_header.h")

scan_case("an explicitly ignored path is skipped" 0
    ROOT "${FIXTURES}/ignorable"
    ARGS "--ignore;only_marker.cpp"
    CONTAINS "0 errors")

scan_case("a required path that is missing fails the gate" 1
    ROOT "${FIXTURES}/clean"
    ARGS "--require;no_such_path"
    CONTAINS "required path is missing")

scan_case("a symlinked entry fails instead of being silently skipped" 1
    ROOT "${FIXTURES}/linked"
    CONTAINS "symlink is not followed")

scan_case("build output is skipped, not scanned" 0
    ROOT "${FIXTURES}/build_fixture"
    CONTAINS "skipped")

# A missing pattern table is a setup error, not a violation: exit 2, not 1.
execute_process(
    COMMAND "${SCANNER}" --root "${FIXTURES}/clean" --patterns "${FIXTURES}/no_such_table.txt"
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output)
set(setup_problems "")
if(NOT exit_code EQUAL 2)
    list(APPEND setup_problems "exit code ${exit_code}, expected 2")
endif()
string(FIND "${error_output}" "cannot open pattern table" position)
if(position EQUAL -1)
    list(APPEND setup_problems "the reason is not reported on stderr")
endif()
if(setup_problems)
    list(APPEND case_failures "missing pattern table: ${setup_problems}")
else()
    message(STATUS "pass missing pattern table exits 2")
endif()

if(case_failures)
    string(REPLACE ";" "\n  - " readable "${case_failures}")
    message(FATAL_ERROR "qs-scan self-test failures:\n  - ${readable}")
endif()

message(STATUS "qs-scan self-test: every case passed")
