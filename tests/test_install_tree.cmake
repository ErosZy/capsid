# Install tree test (WP-00 / PR-01 RED gate for P0-6).
#
# The frozen runtime distribution manifest from the execution spec §12.2.
# The test installs the configured build into a FRESH EMPTY prefix and
# asserts every manifest entry exists. The pre-fix tree has no Capsid
# install() rules at all, so `cmake --install` reports success while the
# prefix stays empty.
#
# Usage (script mode):
#   cmake -DCAPSID_BUILD_DIR=... -DCAPSID_PREFIX=<fresh dir>
#        [-DCAPSID_BUILD_HOST=ON|OFF (option flag only;
#        capsid-host is expected in the tree iff the build actually has
#        the target, signalled by CAPSID_HOST_TARGET=ON|OFF)]
#        -P tests/test_install_tree.cmake

if(NOT DEFINED CAPSID_HOST_TARGET)
    message(FATAL_ERROR "CAPSID_HOST_TARGET is required")
endif()

if(NOT CAPSID_BUILD_DIR OR NOT CAPSID_PREFIX)
    message(FATAL_ERROR
        "CAPSID_BUILD_DIR and CAPSID_PREFIX are required")
endif()

file(REMOVE_RECURSE "${CAPSID_PREFIX}")
file(MAKE_DIRECTORY "${CAPSID_PREFIX}")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        --install "${CAPSID_BUILD_DIR}"
        --prefix "${CAPSID_PREFIX}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "cmake --install failed:\n${install_output}\n${install_error}")
endif()

# Executable names carry the platform suffix on Windows (.exe); the
# manifest is platform-aware so a Windows install tree is verified
# against the names Windows actually produces.
if(WIN32)
    set(CAPSID_BIN_SUFFIX ".exe")
else()
    set(CAPSID_BIN_SUFFIX "")
endif()
set(CAPSID_MANIFEST
    "bin/capsid-worker${CAPSID_BIN_SUFFIX}"
    "bin/capsid-bytecode-compile${CAPSID_BIN_SUFFIX}"
    "include/capsid/runtime.h"
    "include/capsid/runtime.hpp"
    "lib/cmake/Capsid/CapsidTargets.cmake"
    "share/licenses/capsid/LICENSE"
    "share/capsid/build-info.txt"
    "share/capsid/SBOM.spdx.json")
if(CAPSID_HOST_TARGET)
    list(APPEND CAPSID_MANIFEST "bin/capsid-host${CAPSID_BIN_SUFFIX}")
endif()

set(missing)
foreach(entry IN LISTS CAPSID_MANIFEST)
    if(NOT EXISTS "${CAPSID_PREFIX}/${entry}")
        list(APPEND missing "${entry}")
    endif()
endforeach()

# Libraries may be static or shared; accept any runtime-library name the
# platform produces (libcapsid_runtime.* on POSIX, capsid_runtime.* on
# MSVC).
file(GLOB CAPSID_RUNTIME_LIBS
    "${CAPSID_PREFIX}/lib/*capsid_runtime.*")
if(NOT CAPSID_RUNTIME_LIBS)
    list(APPEND missing "lib/*capsid_runtime.*")
endif()

# Support documentation must be a non-empty directory.
set(CAPSID_DOC_DIR "${CAPSID_PREFIX}/share/doc/capsid")
if(NOT IS_DIRECTORY "${CAPSID_DOC_DIR}")
    list(APPEND missing "share/doc/capsid/")
else()
    file(GLOB_RECURSE CAPSID_DOC_FILES "${CAPSID_DOC_DIR}/*")
    if(NOT CAPSID_DOC_FILES)
        list(APPEND missing "share/doc/capsid/ (empty)")
    endif()
endif()

if(missing)
    set(installed)
    file(GLOB_RECURSE CAPSID_INSTALLED_FILES
        "${CAPSID_PREFIX}/*")
    foreach(path IN LISTS CAPSID_INSTALLED_FILES)
        file(RELATIVE_PATH relative
            "${CAPSID_PREFIX}" "${path}")
        list(APPEND installed "${relative}")
    endforeach()
    message(FATAL_ERROR
        "install tree is missing expected files: ${missing}\n"
        "installed tree contains: ${installed}")
endif()

message(STATUS "PASS: install tree contains the full runtime manifest")
