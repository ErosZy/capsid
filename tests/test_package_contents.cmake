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
#   cmake -DCAPSID_BUILD_DIR=... -DCAPSID_WORK_DIR=<fresh dir>
#        [-DCAPSID_BUILD_HOST=ON|OFF]
#        -P tests/test_package_contents.cmake

if(NOT CAPSID_BUILD_DIR OR NOT CAPSID_WORK_DIR)
    message(FATAL_ERROR
        "CAPSID_BUILD_DIR and CAPSID_WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${CAPSID_WORK_DIR}")
file(MAKE_DIRECTORY "${CAPSID_WORK_DIR}")

# Snapshot archives that already exist so stale third-party artifacts from
# earlier runs can never be confused with this run's output.
file(GLOB CAPSID_PREEXISTING_ARCHIVES
    "${CAPSID_BUILD_DIR}/*.tar.gz")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        --build "${CAPSID_BUILD_DIR}"
        --target package
    RESULT_VARIABLE package_result
    OUTPUT_VARIABLE package_output
    ERROR_VARIABLE package_error)
if(NOT package_result EQUAL 0)
    message(FATAL_ERROR
        "package target failed:\n${package_output}\n${package_error}")
endif()

set(CAPSID_PACKAGE_ARCHIVES)
file(GLOB CAPSID_CANDIDATE_ARCHIVES
    "${CAPSID_BUILD_DIR}/capsid-*.tar.gz")
foreach(archive IN LISTS CAPSID_CANDIDATE_ARCHIVES)
    list(FIND CAPSID_PREEXISTING_ARCHIVES "${archive}" preexisting)
    if(preexisting EQUAL -1)
        list(APPEND CAPSID_PACKAGE_ARCHIVES "${archive}")
    endif()
endforeach()
if(NOT CAPSID_PACKAGE_ARCHIVES)
    file(GLOB CAPSID_FOREIGN_ARCHIVES
        "${CAPSID_BUILD_DIR}/*.tar.gz")
    message(FATAL_ERROR
        "package target produced no Capsid archive. Found: "
        "${CAPSID_FOREIGN_ARCHIVES}\n${package_output}")
endif()

list(LENGTH CAPSID_PACKAGE_ARCHIVES archive_count)
if(NOT archive_count EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one Capsid archive, found ${archive_count}: "
        "${CAPSID_PACKAGE_ARCHIVES}")
endif()
list(GET CAPSID_PACKAGE_ARCHIVES 0 CAPSID_PACKAGE_ARCHIVE)

# No third-party archive may be produced by the Capsid package target.
set(CAPSID_NEW_ARCHIVES)
file(GLOB CAPSID_ALL_ARCHIVES "${CAPSID_BUILD_DIR}/*.tar.gz")
foreach(archive IN LISTS CAPSID_ALL_ARCHIVES)
    list(FIND CAPSID_PREEXISTING_ARCHIVES "${archive}" preexisting)
    if(preexisting EQUAL -1)
        list(APPEND CAPSID_NEW_ARCHIVES "${archive}")
    endif()
endforeach()
foreach(archive IN LISTS CAPSID_NEW_ARCHIVES)
    get_filename_component(archive_name "${archive}" NAME)
    if(NOT archive_name MATCHES "^capsid-")
        message(FATAL_ERROR
            "package target produced a third-party archive: "
            "${archive_name}")
    endif()
endforeach()

file(ARCHIVE_EXTRACT
    INPUT "${CAPSID_PACKAGE_ARCHIVE}"
    DESTINATION "${CAPSID_WORK_DIR}/extracted")
set(CAPSID_EXTRACT_ROOT "${CAPSID_WORK_DIR}/extracted")

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

set(CAPSID_MANIFEST
    "bin/capsid-worker"
    "bin/capsid-bytecode-compile"
    "include/capsid/runtime.h"
    "include/capsid/runtime.hpp"
    "lib/cmake/Capsid/CapsidTargets.cmake"
    "share/licenses/capsid/LICENSE"
    "share/capsid/build-info.txt"
    "share/capsid/SBOM.spdx.json")
if(CAPSID_BUILD_HOST)
    list(APPEND CAPSID_MANIFEST "bin/capsid-host")
endif()

set(missing)
foreach(entry IN LISTS CAPSID_MANIFEST)
    if(NOT EXISTS "${CAPSID_MANIFEST_ROOT}/${entry}")
        list(APPEND missing "${entry}")
    endif()
endforeach()

file(GLOB CAPSID_PACKAGE_LIBS
    "${CAPSID_MANIFEST_ROOT}/lib/libcapsid_runtime.*")
if(NOT CAPSID_PACKAGE_LIBS)
    list(APPEND missing "lib/libcapsid_runtime.*")
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
