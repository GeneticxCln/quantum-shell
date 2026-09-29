cmake_minimum_required(VERSION 3.31...4.4)

# Builds the release tarball and its checksum from the committed tree.
#
#   cmake -DSOURCE_DIR=<repo> -DVERSION=<x.y.z> -DOUTPUT_DIR=<dir> -P tools/dist.cmake
#
# Written as a script rather than as a shell line in a document so the properties a release needs are enforced
# rather than remembered:
#
#   * **It archives a commit, not a directory.** `git archive HEAD`, so the tarball holds exactly what is
#     committed — no build tree, no editor droppings, nothing that exists only on one machine — and a file that is
#     untracked cannot end up in a release by accident.
#   * **It refuses a tree that is not clean.** A tarball named after a version but built from a tree with
#     uncommitted changes would not be what that version is, and would have a checksum that nobody could reproduce
#     from the commit. Committed changes only, or it stops and says which paths are dirty.
#   * **It is reproducible.** `git archive` stamps every entry with the commit's time and `gzip -n` leaves out the
#     name and time, so the same commit gives the same bytes and therefore the same checksum, on any machine.
#   * **The checksum file is the format `sha256sum -c` reads**, so a person verifies the download with the tool
#     every system has.
#
# Nothing here publishes anything: it writes two files into OUTPUT_DIR. Cutting a release — tagging, uploading —
# is a decision for a person, not for a build target.

foreach(required IN ITEMS SOURCE_DIR VERSION OUTPUT_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "dist.cmake requires ${required}")
    endif()
endforeach()

find_program(GIT_EXECUTABLE git)
find_program(GZIP_EXECUTABLE gzip)
if(NOT GIT_EXECUTABLE OR NOT GZIP_EXECUTABLE)
    message(FATAL_ERROR "dist.cmake needs git and gzip on the PATH (git: '${GIT_EXECUTABLE}', gzip: '${GZIP_EXECUTABLE}')")
endif()

execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SOURCE_DIR} status --porcelain
    OUTPUT_VARIABLE dirty RESULT_VARIABLE status OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "${SOURCE_DIR} is not a git work tree, so there is no commit to archive")
endif()
if(NOT dirty STREQUAL "")
    message(FATAL_ERROR "refusing to build a release from a tree with uncommitted or untracked changes; "
                        "commit them or remove them first. Dirty paths:\n${dirty}")
endif()

set(name "quantum-shell-${VERSION}")
set(tarball "${OUTPUT_DIR}/${name}.tar.gz")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

# The tar is written by git and then compressed by gzip as two steps, each checked, so a failure of either stage
# is a failure of the target rather than a truncated file with the right name.
set(tar "${OUTPUT_DIR}/${name}.tar")
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SOURCE_DIR} archive --format=tar --prefix=${name}/
        -o ${tar} HEAD
    RESULT_VARIABLE archive_status ERROR_VARIABLE archive_error)
if(NOT archive_status EQUAL 0)
    message(FATAL_ERROR "git archive failed: ${archive_error}")
endif()
execute_process(COMMAND ${GZIP_EXECUTABLE} -n -9 -f ${tar}
    RESULT_VARIABLE gzip_status ERROR_VARIABLE gzip_error)
if(NOT gzip_status EQUAL 0)
    message(FATAL_ERROR "gzip failed: ${gzip_error}")
endif()

file(SHA256 "${tarball}" digest)
file(WRITE "${tarball}.sha256" "${digest}  ${name}.tar.gz\n")
message(STATUS "wrote ${tarball}")
message(STATUS "sha256 ${digest}")
