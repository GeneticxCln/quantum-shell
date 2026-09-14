cmake_minimum_required(VERSION 3.31...4.4)

# Script-mode checks do not inherit the root project's policies, including IN_LIST support.

# Checks the project's test-facing public names — the ctest names and the environment variables in
# tests/public_names.cmake — against the documentation and the build files that use them.
#
# This exists because none of these failures fail anything on their own. `ctest -R <stale name>` prints
# "No tests were found!!!" and exits 0, so a document can keep telling people to run a check that no
# longer exists and every run stays green. An environment variable renamed in the build file and not in
# the document turns an opt-in into a no-op with no symptom at all.
#
# The three lists are compared in both directions on purpose: a name used but undeclared is a change
# nobody recorded, and a name declared but used by nothing is a claim about the project that has stopped
# being true, which is the shape SYSTEM_PROMPT.md calls out as a file that lies about what the shell does.
#
# Inputs, all required:
#   ROOT               the repository root
#   TEST_NAMES         declared ctest names, joined with `|`
#   ENVIRONMENT_NAMES  declared environment variable names, joined with `|`
#   DOCUMENTS          the documents whose commands are checked, joined with `|`

foreach(required IN ITEMS ROOT TEST_NAMES ENVIRONMENT_NAMES DOCUMENTS)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "check_public_names.cmake requires ${required}")
    endif()
endforeach()

string(REPLACE "|" ";" test_names "${TEST_NAMES}")
string(REPLACE "|" ";" environment_names "${ENVIRONMENT_NAMES}")
string(REPLACE "|" ";" documents "${DOCUMENTS}")

set(problems "")

set(prose "")
foreach(document IN LISTS documents)
    if(NOT EXISTS "${document}")
        message(FATAL_ERROR "check_public_names.cmake was given ${document}, which does not exist")
    endif()
    file(READ "${document}" text)
    string(APPEND prose "${text}\n")
endforeach()

# --- the commands the documentation tells people to run --------------------------------------------
#
# `-R <name>` is a command to paste, and a pasted command that matches nothing reports success. Each one is
# matched the way ctest matches it — as a regular expression, since that is what `-R` takes — against the
# declared test names, and it has to select at least one of them. That keeps a command selecting the four
# shards of a check by their common prefix working, which is a command that runs a real check, while a name
# selecting nothing at all still fails here rather than reporting success in ctest.
set(documented_selects 0)
foreach(document IN LISTS documents)
    file(READ "${document}" text)
    string(REGEX MATCHALL "[-]R [A-Za-z0-9_-]+" selects "${text}")
    foreach(select IN LISTS selects)
        math(EXPR documented_selects "${documented_selects} + 1")
        string(SUBSTRING "${select}" 3 -1 name)
        set(selected "")
        foreach(declared IN LISTS test_names)
            if("${declared}" MATCHES "${name}")
                list(APPEND selected "${declared}")
            endif()
        endforeach()
        if(selected STREQUAL "")
            list(APPEND problems
                "${document} tells the reader to run `-R ${name}`, which selects none of the test names "
                "declared in tests/public_names.cmake")
        endif()
    endforeach()
endforeach()

# --- environment variables -------------------------------------------------------------------------
#
# Read from the build files with the accessor rather than from a list, so a rename in a build file and a
# rename in the documentation cannot both be missed. The accessor is spelled here in a form this file
# does not itself contain, because a check that reported its own source as a finding would be worse than
# no check.
set(read_environment "")
file(GLOB_RECURSE build_files "${ROOT}/CMakeLists.txt" "${ROOT}/*.cmake")
foreach(path IN LISTS build_files)
    if(path MATCHES "/build/")
        continue()
    endif()
    file(READ "${path}" text)
    string(REGEX MATCHALL "[$]ENV[{][A-Za-z0-9_]+[}]" accesses "${text}")
    foreach(access IN LISTS accesses)
        string(LENGTH "${access}" length)
        math(EXPR inner "${length} - 6")
        string(SUBSTRING "${access}" 5 ${inner} name)
        list(APPEND read_environment "${name}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES read_environment)

string(REGEX MATCHALL "(NIRI_SOCKET|QS_[A-Z0-9_]+)" documented_environment "${prose}")
list(REMOVE_DUPLICATES documented_environment)

foreach(name IN LISTS documented_environment)
    if(NOT name IN_LIST environment_names)
        list(APPEND problems
            "the documentation names the environment variable ${name}, which is not declared in "
            "tests/public_names.cmake")
    endif()
endforeach()

foreach(name IN LISTS read_environment)
    if(NOT name IN_LIST environment_names)
        list(APPEND problems
            "a build file reads the environment variable ${name}, which is not declared in "
            "tests/public_names.cmake")
    endif()
endforeach()

foreach(name IN LISTS environment_names)
    string(FIND "${prose}" "${name}" in_documents)
    if(in_documents EQUAL -1)
        list(APPEND problems
            "${name} is declared an environment variable of this project but appears in no document, "
            "so nobody can discover it")
    endif()
    if(NOT name IN_LIST read_environment)
        list(APPEND problems
            "${name} is declared an environment variable of this project but no build file reads it, "
            "so setting it does nothing")
    endif()
endforeach()

# --- the test names, from the documentation's side --------------------------------------------------
#
# The other direction — a test registered but not declared — is a configure-time error in
# tests/CMakeLists.txt, because that is where the registration happens. What is left for here is a name
# declared that no document asks anyone to run.
foreach(name IN LISTS test_names)
    string(FIND "${prose}" "${name}" in_documents)
    if(in_documents EQUAL -1)
        list(APPEND problems
            "${name} is declared a public test name but appears in no document, so nothing tells anyone "
            "to run it")
    endif()
endforeach()

list(LENGTH test_names test_count)
list(LENGTH environment_names environment_count)
# Counted as it went rather than measured afterwards: `documented_selects` is a number, and list(LENGTH)
# on a number reports 1, which would understate what this check actually read.
set(select_count "${documented_selects}")

if(problems)
    list(JOIN problems "\n  - " readable)
    message(FATAL_ERROR
        "the project's public names and the documents that use them have drifted apart:\n"
        "  - ${readable}\n\n"
        "Fix the name in whichever place is wrong, or declare it in tests/public_names.cmake.")
endif()

message(STATUS
    "public names: ${test_count} test names, ${environment_count} environment variables, "
    "${select_count} documented -R commands, all consistent")
