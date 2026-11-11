cmake_minimum_required(VERSION 3.18)

# Package contents test (WP-00 / PR-01 RED gate for P0-6).
#
# The `package` target must produce the Capsid distribution archive
# (`capsid-<version>-<system>-<arch>.tar.gz`) and must NOT be taken over
# by a third-party CPack configuration. The extracted archive must contain
# the frozen runtime manifest from the execution spec §12.2.
#
# Pre-fix behavior: libwebsockets owns `include(CPack)`, so `--target
# package` silently produces a libwebsockets-*.tar.gz with Capsid's name
# nowhere in it.
#
# Usage (script mode):
#   cmake -DCAPSID_BUILD_DIR=... -DCAPSID_WORK_DIR=<dir inside build>
#        [-DCAPSID_BUILD_HOST=ON|OFF (option flag only;
#        capsid-host is expected in the tree iff the build actually has
#        the target, signalled by CAPSID_HOST_TARGET=ON|OFF)]
#        -P tests/test_package_contents.cmake

if(NOT DEFINED CAPSID_HOST_TARGET)
    message(FATAL_ERROR "CAPSID_HOST_TARGET is required")
endif()

if(NOT CAPSID_BUILD_DIR OR NOT CAPSID_WORK_DIR)
    message(FATAL_ERROR
        "CAPSID_BUILD_DIR and CAPSID_WORK_DIR are required")
endif()

get_filename_component(CAPSID_BUILD_DIR "${CAPSID_BUILD_DIR}" REALPATH)
get_filename_component(CAPSID_WORK_DIR "${CAPSID_WORK_DIR}" REALPATH)
file(RELATIVE_PATH CAPSID_WORK_RELATIVE "${CAPSID_BUILD_DIR}" "${CAPSID_WORK_DIR}")
if(IS_ABSOLUTE "${CAPSID_WORK_RELATIVE}" OR CAPSID_WORK_RELATIVE STREQUAL ""
        OR CAPSID_WORK_RELATIVE STREQUAL "." OR CAPSID_WORK_RELATIVE MATCHES "^\.\.(/|$)")
    message(FATAL_ERROR "CAPSID_WORK_DIR must be a strict descendant of CAPSID_BUILD_DIR")
endif()
if(NOT EXISTS "${CAPSID_BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR "package contents requires a configured build directory")
endif()

# Copy-if-different packaging legitimately preserves an identical archive's
# mtime. Move earlier outputs into a fresh, retained run directory before
# invoking the actual package target. A no-op target then cannot pass by
# leaving an older Capsid archive behind. Never remove the build/work tree.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef CAPSID_RUN_ID)
set(CAPSID_RUN_DIR "${CAPSID_WORK_DIR}/run-${CAPSID_RUN_ID}")
set(CAPSID_PREVIOUS_ARCHIVES "${CAPSID_RUN_DIR}/previous-archives")
file(MAKE_DIRECTORY "${CAPSID_PREVIOUS_ARCHIVES}")
# Resolve again after creation, so an existing parent junction cannot redirect
# a new run outside the build before we move any earlier archive.
get_filename_component(CAPSID_PREVIOUS_ARCHIVES "${CAPSID_PREVIOUS_ARCHIVES}" REALPATH)
file(RELATIVE_PATH CAPSID_PREVIOUS_RELATIVE "${CAPSID_BUILD_DIR}" "${CAPSID_PREVIOUS_ARCHIVES}")
if(IS_ABSOLUTE "${CAPSID_PREVIOUS_RELATIVE}" OR CAPSID_PREVIOUS_RELATIVE MATCHES "^\.\.(/|$)"
        OR CAPSID_PREVIOUS_RELATIVE STREQUAL "" OR CAPSID_PREVIOUS_RELATIVE STREQUAL ".")
    message(FATAL_ERROR "CAPSID_WORK_DIR must be a strict descendant of CAPSID_BUILD_DIR")
endif()
file(GLOB CAPSID_ALL_ARCHIVES_BEFORE
    "${CAPSID_BUILD_DIR}/*.tar.gz"
    "${CAPSID_BUILD_DIR}/*.zip")
foreach(archive IN LISTS CAPSID_ALL_ARCHIVES_BEFORE)
    if(IS_DIRECTORY "${archive}" OR IS_SYMLINK "${archive}")
        message(FATAL_ERROR "package archive candidate is not a regular file: ${archive}")
    endif()
endforeach()
foreach(archive IN LISTS CAPSID_ALL_ARCHIVES_BEFORE)
    get_filename_component(archive_name "${archive}" NAME)
    file(SHA256 "${archive}" archive_hash)
    file(APPEND "${CAPSID_RUN_DIR}/previous-archives.txt" "${archive_name}\t${archive_hash}\n")
    file(RENAME "${archive}" "${CAPSID_PREVIOUS_ARCHIVES}/${archive_name}")
endforeach()
message(STATUS "package contents: previous outputs retained in ${CAPSID_PREVIOUS_ARCHIVES}")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        --build "${CAPSID_BUILD_DIR}"
        --target package
    RESULT_VARIABLE package_result
    OUTPUT_VARIABLE package_output
    ERROR_VARIABLE package_error)
file(WRITE "${CAPSID_RUN_DIR}/package.log" "${package_output}${package_error}")
if(NOT package_result STREQUAL "0")
    message(FATAL_ERROR
        "package target failed:\n${package_output}\n${package_error}")
endif()

set(CAPSID_PACKAGE_ARCHIVES)
file(GLOB CAPSID_ALL_ARCHIVES
    "${CAPSID_BUILD_DIR}/*.tar.gz"
    "${CAPSID_BUILD_DIR}/*.zip")
foreach(archive IN LISTS CAPSID_ALL_ARCHIVES)
    if(IS_DIRECTORY "${archive}" OR IS_SYMLINK "${archive}")
        message(FATAL_ERROR "package archive candidate is not a regular file: ${archive}")
    endif()
    get_filename_component(archive_name "${archive}" NAME)
    if(NOT archive_name MATCHES "^capsid-")
        message(FATAL_ERROR "package target produced a third-party archive: ${archive_name}")
    endif()
    list(APPEND CAPSID_PACKAGE_ARCHIVES "${archive}")
endforeach()
if(NOT CAPSID_PACKAGE_ARCHIVES)
    message(FATAL_ERROR
        "package target produced no new Capsid archive. All archives: "
        "${CAPSID_ALL_ARCHIVES}\n${package_output}")
endif()

list(LENGTH CAPSID_PACKAGE_ARCHIVES archive_count)
if(NOT archive_count EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one new Capsid archive, found ${archive_count}: "
        "${CAPSID_PACKAGE_ARCHIVES}")
endif()
list(GET CAPSID_PACKAGE_ARCHIVES 0 CAPSID_PACKAGE_ARCHIVE)

# A Capsid-looking name from another build is not this build's distribution.
# Read the generated identity after packaging, which may reconfigure the build.
file(STRINGS "${CAPSID_BUILD_DIR}/CPackConfig.cmake" CAPSID_PACKAGE_CONFIG
    REGEX "^set\\(CPACK_PACKAGE_FILE_NAME ")
if(NOT CAPSID_PACKAGE_CONFIG MATCHES "^set\\(CPACK_PACKAGE_FILE_NAME \"([^\"]+)\"\\)$")
    message(FATAL_ERROR "cannot identify this build's package archive")
endif()
set(CAPSID_EXPECTED_BASENAME "${CMAKE_MATCH_1}")
if(NOT CAPSID_EXPECTED_BASENAME MATCHES "^capsid-[^/\\;]+$")
    message(FATAL_ERROR "invalid Capsid package basename: ${CAPSID_EXPECTED_BASENAME}")
endif()
get_filename_component(CAPSID_PACKAGE_NAME "${CAPSID_PACKAGE_ARCHIVE}" NAME)
if(NOT CAPSID_PACKAGE_NAME STREQUAL "${CAPSID_EXPECTED_BASENAME}.zip"
        AND NOT CAPSID_PACKAGE_NAME STREQUAL "${CAPSID_EXPECTED_BASENAME}.tar.gz")
    message(FATAL_ERROR "unexpected Capsid archive ${CAPSID_PACKAGE_NAME}; expected ${CAPSID_EXPECTED_BASENAME}")
endif()

file(ARCHIVE_EXTRACT
    INPUT "${CAPSID_PACKAGE_ARCHIVE}"
    DESTINATION "${CAPSID_RUN_DIR}/extracted")
set(CAPSID_EXTRACT_ROOT "${CAPSID_RUN_DIR}/extracted")

# Locate the archive root: either the files sit at the root or under a
# single top-level directory named after the package.
file(GLOB CAPSID_EXTRACTED_ENTRIES
    "${CAPSID_EXTRACT_ROOT}/*")
list(LENGTH CAPSID_EXTRACTED_ENTRIES entry_count)
set(CAPSID_MANIFEST_ROOT "${CAPSID_EXTRACT_ROOT}")
if(entry_count EQUAL 1)
    list(GET CAPSID_EXTRACTED_ENTRIES 0 single_entry)
    if(IS_DIRECTORY "${single_entry}")
        set(CAPSID_MANIFEST_ROOT "${single_entry}")
    endif()
endif()

if(WIN32)
    # MSVC binaries carry .exe and the static runtime library drops the
    # POSIX lib prefix.
    set(CAPSID_MANIFEST
        "bin/capsid-worker.exe"
        "bin/capsid-bytecode-compile.exe"
        "include/capsid/runtime.h"
        "include/capsid/runtime.hpp"
        "lib/cmake/Capsid/CapsidTargets.cmake"
        "share/licenses/capsid/LICENSE"
        "share/licenses/capsid/web-streams-polyfill/LICENSE"
        "share/capsid/build-info.txt"
        "share/capsid/SBOM.spdx.json")
    if(CAPSID_HOST_TARGET)
        list(APPEND CAPSID_MANIFEST "bin/capsid-host.exe")
    endif()
else()
    set(CAPSID_MANIFEST
        "bin/capsid-worker"
        "bin/capsid-bytecode-compile"
        "include/capsid/runtime.h"
        "include/capsid/runtime.hpp"
        "lib/cmake/Capsid/CapsidTargets.cmake"
        "share/licenses/capsid/LICENSE"
        "share/licenses/capsid/web-streams-polyfill/LICENSE"
        "share/capsid/build-info.txt"
        "share/capsid/SBOM.spdx.json")
    if(CAPSID_HOST_TARGET)
        list(APPEND CAPSID_MANIFEST "bin/capsid-host")
    endif()
endif()

list(APPEND CAPSID_MANIFEST
    "lib/cmake/Capsid/CapsidConfig.cmake"
    "lib/cmake/Capsid/CapsidConfigVersion.cmake")

set(missing)
foreach(entry IN LISTS CAPSID_MANIFEST)
    if(NOT EXISTS "${CAPSID_MANIFEST_ROOT}/${entry}")
        list(APPEND missing "${entry}")
    endif()
endforeach()

if(WIN32)
    if(NOT EXISTS "${CAPSID_MANIFEST_ROOT}/lib/capsid_runtime.lib")
        list(APPEND missing "lib/capsid_runtime.lib")
    endif()
else()
    file(GLOB CAPSID_PACKAGE_LIBS
        "${CAPSID_MANIFEST_ROOT}/lib/libcapsid_runtime.*")
    if(NOT CAPSID_PACKAGE_LIBS)
        list(APPEND missing "lib/libcapsid_runtime.*")
    endif()
endif()

file(GLOB_RECURSE CAPSID_PACKAGE_DOC
    "${CAPSID_MANIFEST_ROOT}/share/doc/capsid/*")
if(NOT CAPSID_PACKAGE_DOC)
    list(APPEND missing "share/doc/capsid/")
endif()

if(missing)
    set(contents)
    file(GLOB_RECURSE CAPSID_PACKAGED_FILES
        "${CAPSID_MANIFEST_ROOT}/*")
    foreach(path IN LISTS CAPSID_PACKAGED_FILES)
        file(RELATIVE_PATH relative
            "${CAPSID_MANIFEST_ROOT}" "${path}")
        list(APPEND contents "${relative}")
    endforeach()
    message(FATAL_ERROR
        "Capsid archive ${CAPSID_PACKAGE_ARCHIVE} is missing "
        "${missing};\npackage contents: ${contents}")
endif()

get_filename_component(CAPSID_PACKAGE_NAME
    "${CAPSID_PACKAGE_ARCHIVE}" NAME)
message(STATUS "PASS: package ${CAPSID_PACKAGE_NAME} contains the full "
        "runtime manifest")
