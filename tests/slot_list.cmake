# The slots of a Qt test binary, read from the binary itself rather than from its source.
#
# `test -functions` writes one slot per line, as `name()`. A line that is not that shape is ignored rather
# than guessed at — but a binary that lists no slots at all is an error, not a pass: a check that runs
# nothing and reports success is worse than no check, and is the same silent-hole mistake the checks in
# this directory exist to find.
#
# Included by run_reordered.cmake and run_shuffled_order.cmake, so both read a binary the same way.

function(qs_slots_of binary out_var)
    if(NOT EXISTS "${binary}")
        message(FATAL_ERROR "qs_slots_of: ${binary} does not exist")
    endif()

    execute_process(
        COMMAND "${binary}" -functions
        RESULT_VARIABLE code
        OUTPUT_VARIABLE listing
        ERROR_VARIABLE error_output
        TIMEOUT 120)

    if(NOT code EQUAL 0)
        message(FATAL_ERROR "qs_slots_of: ${binary} -functions exited ${code}: ${error_output}")
    endif()

    string(REPLACE "\n" ";" lines "${listing}")
    set(slots "")
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line MATCHES "^([A-Za-z_][A-Za-z0-9_]*)\\(\\)$")
            list(APPEND slots "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    if(slots STREQUAL "")
        message(FATAL_ERROR "qs_slots_of: ${binary} listed no slots")
    endif()

    set("${out_var}" "${slots}" PARENT_SCOPE)
endfunction()
