# Independent contract check for the canonical txiki.js overlay key.
#
# This intentionally reconstructs the documented serialization instead of
# calling implementation helpers for the expected value.  In particular,
# submodules are ordered by path; sorting the rendered "<commit> <path>"
# records accidentally orders them by commit and must be detected.

foreach(CAPSID_REQUIRED_INPUT
        CAPSID_TXIKI_KEY_SCRIPT
        CAPSID_TXIKI_VENDOR
        CAPSID_TXIKI_PATCH_DIR
        CAPSID_TXIKI_PREPARE_SCRIPT)
    if(NOT DEFINED ${CAPSID_REQUIRED_INPUT})
        message(FATAL_ERROR "${CAPSID_REQUIRED_INPUT} is required")
    endif()
endforeach()

find_program(CAPSID_GIT_EXECUTABLE git REQUIRED)

include("${CAPSID_TXIKI_KEY_SCRIPT}")
capsid_compute_txiki_overlay_key(
    OUT_KEY CAPSID_ACTUAL_KEY
    OUT_PATCHES CAPSID_ACTUAL_PATCHES
    VENDOR_DIR "${CAPSID_TXIKI_VENDOR}"
    PATCH_DIR "${CAPSID_TXIKI_PATCH_DIR}"
    PREPARE_SCRIPT "${CAPSID_TXIKI_PREPARE_SCRIPT}"
)

execute_process(
    COMMAND "${CAPSID_GIT_EXECUTABLE}" -C "${CAPSID_TXIKI_VENDOR}"
        rev-parse HEAD
    OUTPUT_VARIABLE CAPSID_VENDOR_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE CAPSID_VENDOR_RESULT
)
if(NOT CAPSID_VENDOR_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot resolve vendor revision")
endif()

execute_process(
    COMMAND "${CAPSID_GIT_EXECUTABLE}" -C "${CAPSID_TXIKI_VENDOR}"
        submodule status --recursive
    OUTPUT_VARIABLE CAPSID_SUBMODULE_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE CAPSID_SUBMODULE_RESULT
)
if(NOT CAPSID_SUBMODULE_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot read recursive submodule status")
endif()

set(CAPSID_SORTABLE_SUBMODULES "")
if(CAPSID_SUBMODULE_STATUS)
    string(REPLACE "\n" ";" CAPSID_SUBMODULE_ENTRIES
        "${CAPSID_SUBMODULE_STATUS}")
    foreach(CAPSID_ENTRY IN LISTS CAPSID_SUBMODULE_ENTRIES)
        string(SUBSTRING "${CAPSID_ENTRY}" 0 1 CAPSID_PREFIX)
        if(NOT CAPSID_PREFIX STREQUAL " " AND
           NOT CAPSID_PREFIX STREQUAL "+" AND
           NOT CAPSID_PREFIX STREQUAL "-" AND
           NOT CAPSID_PREFIX STREQUAL "U")
            message(FATAL_ERROR
                "cannot parse submodule status line: ${CAPSID_ENTRY}")
        endif()
        if(NOT CAPSID_ENTRY MATCHES "^.([0-9a-f]+) (.+)$")
            message(FATAL_ERROR
                "cannot parse submodule status line: ${CAPSID_ENTRY}")
        endif()
        set(CAPSID_COMMIT "${CMAKE_MATCH_1}")
        set(CAPSID_PATH "${CMAKE_MATCH_2}")
        string(REGEX REPLACE " [(].*[)]$" "" CAPSID_PATH "${CAPSID_PATH}")
        list(APPEND CAPSID_SORTABLE_SUBMODULES
            "${CAPSID_PATH}|${CAPSID_COMMIT}")
    endforeach()
endif()
list(SORT CAPSID_SORTABLE_SUBMODULES)

file(SHA256 "${CAPSID_TXIKI_PREPARE_SCRIPT}" CAPSID_PREPARE_HASH)
file(GLOB CAPSID_PATCHES "${CAPSID_TXIKI_PATCH_DIR}/*.patch")
list(SORT CAPSID_PATCHES)

set(CAPSID_CANONICAL_INPUT "schema=capsid-txiki-overlay-v1\n")
string(APPEND CAPSID_CANONICAL_INPUT
    "vendor=${CAPSID_VENDOR_REVISION}\n")
foreach(CAPSID_SORTABLE IN LISTS CAPSID_SORTABLE_SUBMODULES)
    string(REGEX REPLACE "^([^|]+)[|](.+)$" "\\1" CAPSID_PATH
        "${CAPSID_SORTABLE}")
    string(REGEX REPLACE "^([^|]+)[|](.+)$" "\\2" CAPSID_COMMIT
        "${CAPSID_SORTABLE}")
    string(APPEND CAPSID_CANONICAL_INPUT
        "submodule=${CAPSID_COMMIT} ${CAPSID_PATH}\n")
endforeach()
string(APPEND CAPSID_CANONICAL_INPUT "prepare=${CAPSID_PREPARE_HASH}\n")
foreach(CAPSID_PATCH IN LISTS CAPSID_PATCHES)
    get_filename_component(CAPSID_PATCH_NAME "${CAPSID_PATCH}" NAME)
    file(SHA256 "${CAPSID_PATCH}" CAPSID_PATCH_HASH)
    string(APPEND CAPSID_CANONICAL_INPUT
        "patch=${CAPSID_PATCH_NAME} ${CAPSID_PATCH_HASH}\n")
endforeach()
string(SHA256 CAPSID_EXPECTED_KEY "${CAPSID_CANONICAL_INPUT}")

if(NOT CAPSID_ACTUAL_KEY STREQUAL CAPSID_EXPECTED_KEY)
    message(FATAL_ERROR
        "txiki.js overlay key is not canonical.\n"
        "  expected (submodules sorted by path): ${CAPSID_EXPECTED_KEY}\n"
        "  actual:                               ${CAPSID_ACTUAL_KEY}")
endif()

message(STATUS "txiki.js overlay key uses canonical path ordering")
