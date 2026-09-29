cmake_minimum_required(VERSION 3.31...4.4)

# Drives tools/dist.cmake against a small git repository this script makes for itself, and checks what a release
# needs: the tarball holds the committed files under a versioned prefix and nothing else, the checksum file
# verifies with `sha256sum -c`, the same commit gives the same bytes twice, and a dirty tree is refused.
#
# Inputs: DIST_SCRIPT (tools/dist.cmake), WORK (a scratch directory this script owns and empties).

foreach(required IN ITEMS DIST_SCRIPT WORK)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "check_dist.cmake requires ${required}")
    endif()
endforeach()

find_program(GIT_EXECUTABLE git)
find_program(SHA256SUM_EXECUTABLE sha256sum)
if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "git is not on the PATH, so the release tarball cannot be built or checked here")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/repo")
set(repo "${WORK}/repo")

function(git)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${repo} -c user.name=t -c user.email=t@example.invalid
            -c commit.gpgsign=false ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed: ${out}${err}")
    endif()
endfunction()

git(init -q)
file(WRITE "${repo}/CMakeLists.txt" "project(tiny)\n")
file(WRITE "${repo}/LICENSE" "MIT\n")
file(MAKE_DIRECTORY "${repo}/src")
file(WRITE "${repo}/src/main.cpp" "int main() { return 0; }\n")
git(add -A)
git(commit -q -m "one")
# Ignored files must stay out of a tarball built from a commit; an untracked file makes the tree dirty (below).
file(WRITE "${repo}/.gitignore" "build/\n")
git(add .gitignore)
git(commit -q -m "two")
file(MAKE_DIRECTORY "${repo}/build")
file(WRITE "${repo}/build/artifact.o" "not source\n")

function(build out_dir)
    execute_process(COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${repo} -DVERSION=1.2.3 -DOUTPUT_DIR=${out_dir}
            -P ${DIST_SCRIPT}
        RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(BUILD_STATUS ${status} PARENT_SCOPE)
    set(BUILD_ERROR "${out}${err}" PARENT_SCOPE)
endfunction()

build("${WORK}/one")
if(NOT BUILD_STATUS EQUAL 0)
    message(FATAL_ERROR "dist.cmake failed on a clean tree: ${BUILD_ERROR}")
endif()
set(tarball "${WORK}/one/quantum-shell-1.2.3.tar.gz")
foreach(expected IN ITEMS "${tarball}" "${tarball}.sha256")
    if(NOT EXISTS "${expected}")
        message(FATAL_ERROR "dist.cmake did not write ${expected}")
    endif()
endforeach()

# Contents: the committed files under the versioned prefix, and not the ignored build output or the git directory.
execute_process(COMMAND ${CMAKE_COMMAND} -E tar tzf ${tarball}
    OUTPUT_VARIABLE listing RESULT_VARIABLE list_status)
if(NOT list_status EQUAL 0)
    message(FATAL_ERROR "the tarball cannot be listed")
endif()
foreach(wanted IN ITEMS "quantum-shell-1.2.3/CMakeLists.txt" "quantum-shell-1.2.3/LICENSE"
        "quantum-shell-1.2.3/src/main.cpp")
    string(FIND "${listing}" "${wanted}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "the tarball lacks ${wanted}:\n${listing}")
    endif()
endforeach()
foreach(unwanted IN ITEMS "artifact.o" ".git/" "build/")
    string(FIND "${listing}" "${unwanted}" at)
    if(NOT at EQUAL -1)
        message(FATAL_ERROR "the tarball holds ${unwanted}, which is not committed source:\n${listing}")
    endif()
endforeach()

# The checksum file is in the format `sha256sum -c` reads, and it verifies.
if(SHA256SUM_EXECUTABLE)
    execute_process(COMMAND ${SHA256SUM_EXECUTABLE} -c quantum-shell-1.2.3.tar.gz.sha256
        WORKING_DIRECTORY "${WORK}/one" RESULT_VARIABLE verify_status OUTPUT_VARIABLE verify_out
        ERROR_VARIABLE verify_err)
    if(NOT verify_status EQUAL 0)
        message(FATAL_ERROR "sha256sum -c rejected the checksum file: ${verify_out}${verify_err}")
    endif()
endif()

# A checksum that does not match is refused by the same tool: the check has teeth.
file(WRITE "${WORK}/one/bad.sha256" "0000000000000000000000000000000000000000000000000000000000000000  quantum-shell-1.2.3.tar.gz\n")
if(SHA256SUM_EXECUTABLE)
    execute_process(COMMAND ${SHA256SUM_EXECUTABLE} -c bad.sha256
        WORKING_DIRECTORY "${WORK}/one" RESULT_VARIABLE bad_status OUTPUT_QUIET ERROR_QUIET)
    if(bad_status EQUAL 0)
        message(FATAL_ERROR "sha256sum accepted a wrong checksum, so the verification above proves nothing")
    endif()
endif()

# Reproducible: the same commit built again, elsewhere, has the same checksum. The second build is made two seconds
# later on purpose: gzip stamps the time of its input into the header unless told not to, so two builds inside one
# second agree by accident, and only a gap makes a missing `-n` show.
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 2)
build("${WORK}/two")
if(NOT BUILD_STATUS EQUAL 0)
    message(FATAL_ERROR "the second build failed: ${BUILD_ERROR}")
endif()
file(SHA256 "${tarball}" first)
file(SHA256 "${WORK}/two/quantum-shell-1.2.3.tar.gz" second)
if(NOT first STREQUAL second)
    message(FATAL_ERROR "the same commit gave two different tarballs (${first} and ${second})")
endif()

# A dirty tree is refused, and the refusal names the path.
file(WRITE "${repo}/src/main.cpp" "int main() { return 1; }\n")
build("${WORK}/three")
if(BUILD_STATUS EQUAL 0)
    message(FATAL_ERROR "dist.cmake built a release from a tree with uncommitted changes")
endif()
string(FIND "${BUILD_ERROR}" "src/main.cpp" at)
if(at EQUAL -1)
    message(FATAL_ERROR "the refusal does not name the dirty path: ${BUILD_ERROR}")
endif()
if(EXISTS "${WORK}/three/quantum-shell-1.2.3.tar.gz")
    message(FATAL_ERROR "a refused build left a tarball behind")
endif()

message(STATUS "dist: contents, checksum, reproducibility and the dirty-tree refusal all hold")
