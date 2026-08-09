# Source-provenance test for the pinned txiki.js vendor and overlay patches.
#
# This audit verifies:
#   1. Vendor checkout is clean (no untracked or modified files).
#   2. Vendor HEAD matches the expected tag (if configured).
#   3. All 12 patches apply cleanly.
#   4. The overlay stamp matches the key computed from the shared function.
#
# CAPSID_TXIKI_PREPARE_SCRIPT and CAPSID_TXIKI_OVERLAY_STAMP are mandatory —
# the audit must not silently downgrade when either is missing.

foreach(CAPSID_REQUIRED_VAR
        CAPSID_TXIKI_VENDOR
        CAPSID_TXIKI_PATCH_DIR
        CAPSID_TXIKI_PREPARE_SCRIPT
        CAPSID_TXIKI_OVERLAY_STAMP)
    if(NOT DEFINED ${CAPSID_REQUIRED_VAR})
        message(FATAL_ERROR "${CAPSID_REQUIRED_VAR} is required")
    endif()
endforeach()

foreach(CAPSID_REQUIRED_FILE
        CAPSID_TXIKI_VENDOR
        CAPSID_TXIKI_PATCH_DIR
        CAPSID_TXIKI_PREPARE_SCRIPT)
    if(NOT EXISTS "${${CAPSID_REQUIRED_FILE}}")
        message(FATAL_ERROR
            "${CAPSID_REQUIRED_FILE} does not exist: ${${CAPSID_REQUIRED_FILE}}")
    endif()
endforeach()

find_program(CAPSID_GIT_EXECUTABLE git REQUIRED)
set(CAPSID_AUDIT_FAILURES "")

# --- vendor revision ---------------------------------------------------------

if(DEFINED CAPSID_TXIKI_EXPECTED_TAG)
    execute_process(
        COMMAND "${CAPSID_GIT_EXECUTABLE}" -C "${CAPSID_TXIKI_VENDOR}"
            describe --tags --exact-match HEAD
        RESULT_VARIABLE CAPSID_TAG_RESULT
        OUTPUT_VARIABLE CAPSID_ACTUAL_TAG
        ERROR_VARIABLE CAPSID_TAG_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT CAPSID_TAG_RESULT EQUAL 0 OR
       NOT CAPSID_ACTUAL_TAG STREQUAL CAPSID_TXIKI_EXPECTED_TAG)
        string(APPEND CAPSID_AUDIT_FAILURES
            "\n  expected tag '${CAPSID_TXIKI_EXPECTED_TAG}', "
            "got '${CAPSID_ACTUAL_TAG}': ${CAPSID_TAG_ERROR}")
    endif()
endif()

# --- submodule status (every line, not just first match) ---------------------

execute_process(
    COMMAND "${CAPSID_GIT_EXECUTABLE}" -C "${CAPSID_TXIKI_VENDOR}"
        submodule status --recursive
    RESULT_VARIABLE CAPSID_SUBMOD_RESULT
    OUTPUT_VARIABLE CAPSID_SUBMOD_RAW
    ERROR_VARIABLE CAPSID_SUBMOD_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT CAPSID_SUBMOD_RESULT EQUAL 0)
    string(APPEND CAPSID_AUDIT_FAILURES
        "\n  cannot read submodule status: ${CAPSID_SUBMOD_ERROR}")
endif()

# These submodules are not build inputs; allow being uninitialized exactly
# `-` (not `+` checked-out-at-different-revision, nor `U` merge conflict).
set(CAPSID_SUBMOD_UNINIT_ALLOWLIST
    "deps/quickjs/test262"
)

set(CAPSID_BAD_SUBMOD_LINES "")
if(CAPSID_SUBMOD_RAW)
    string(REPLACE "\n" ";" CAPSID_SUBMOD_ENTRIES "${CAPSID_SUBMOD_RAW}")
    foreach(CAPSID_ENTRY IN LISTS CAPSID_SUBMOD_ENTRIES)
        # Leading space = clean (at committed revision).  '+' means the
        # checked-out revision differs; 'U' means merge conflict.
        if(CAPSID_ENTRY MATCHES "^\\+")
            list(APPEND CAPSID_BAD_SUBMOD_LINES "${CAPSID_ENTRY}")
        elseif(CAPSID_ENTRY MATCHES "^U")
            list(APPEND CAPSID_BAD_SUBMOD_LINES "${CAPSID_ENTRY}")
        elseif(CAPSID_ENTRY MATCHES "^-")
            # `-` means uninitialized. Only allowed for specific paths.
            if(CAPSID_ENTRY MATCHES "^-([0-9a-f]+) (.+)$")
                set(CAPSID_SUB_PATH "${CMAKE_MATCH_2}")
                string(REGEX REPLACE " [(].*[)]$" "" CAPSID_SUB_PATH "${CAPSID_SUB_PATH}")
                set(CAPSID_IS_ALLOWED 0)
                foreach(CAPSID_ALLOWED IN LISTS CAPSID_SUBMOD_UNINIT_ALLOWLIST)
                    if(CAPSID_SUB_PATH STREQUAL CAPSID_ALLOWED)
                        set(CAPSID_IS_ALLOWED 1)
                        break()
                    endif()
                endforeach()
                if(NOT CAPSID_IS_ALLOWED)
                    list(APPEND CAPSID_BAD_SUBMOD_LINES "${CAPSID_ENTRY}")
                endif()
            else()
                list(APPEND CAPSID_BAD_SUBMOD_LINES "${CAPSID_ENTRY}")
            endif()
        endif()
    endforeach()
endif()
if(CAPSID_BAD_SUBMOD_LINES)
    string(REPLACE ";" "\n    " CAPSID_INDENTED_SUBMOD
        "${CAPSID_BAD_SUBMOD_LINES}")
    string(APPEND CAPSID_AUDIT_FAILURES
        "\n  submodules are out of sync:\n    ${CAPSID_INDENTED_SUBMOD}")
endif()

# --- vendor cleanliness ------------------------------------------------------

execute_process(
    COMMAND "${CAPSID_GIT_EXECUTABLE}" -C "${CAPSID_TXIKI_VENDOR}"
        status --porcelain=v1 --untracked-files=all --ignore-submodules=none
    RESULT_VARIABLE CAPSID_STATUS_RESULT
    OUTPUT_VARIABLE CAPSID_VENDOR_STATUS
    ERROR_VARIABLE CAPSID_STATUS_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT CAPSID_STATUS_RESULT EQUAL 0)
    string(APPEND CAPSID_AUDIT_FAILURES
        "\n  git status failed: ${CAPSID_STATUS_ERROR}")
endif()

if(CAPSID_VENDOR_STATUS)
    string(REPLACE "\n" "\n    " CAPSID_INDENTED_STATUS
        "${CAPSID_VENDOR_STATUS}")
    message(FATAL_ERROR
        "txiki.js vendor checkout is dirty — refusing to proceed.\n"
        "    ${CAPSID_INDENTED_STATUS}\n\n"
        "Remove untracked files, restore modified tracked files, and ensure "
        "all vendor customizations live in ${CAPSID_TXIKI_PATCH_DIR}. "
        "Do not use .gitignore to mask pollution.")
endif()

# --- patch integrity ---------------------------------------------------------

file(GLOB CAPSID_TXIKI_PATCHES "${CAPSID_TXIKI_PATCH_DIR}/*.patch")
list(SORT CAPSID_TXIKI_PATCHES)
list(LENGTH CAPSID_TXIKI_PATCHES CAPSID_PATCH_COUNT)

if(NOT CAPSID_PATCH_COUNT EQUAL 13)
    string(APPEND CAPSID_AUDIT_FAILURES
        "\n  expected 13 v26.6.0 patches, found ${CAPSID_PATCH_COUNT}")
endif()

if(CAPSID_PATCH_COUNT GREATER 0)
    execute_process(
        COMMAND "${CAPSID_GIT_EXECUTABLE}" -C "${CAPSID_TXIKI_VENDOR}"
            apply --check ${CAPSID_TXIKI_PATCHES}
        RESULT_VARIABLE CAPSID_APPLY_RESULT
        OUTPUT_VARIABLE CAPSID_APPLY_OUTPUT
        ERROR_VARIABLE CAPSID_APPLY_ERROR
    )
    if(NOT CAPSID_APPLY_RESULT EQUAL 0)
        string(APPEND CAPSID_AUDIT_FAILURES
            "\n  combined git apply --check failed:\n"
            "${CAPSID_APPLY_OUTPUT}${CAPSID_APPLY_ERROR}")
    endif()
endif()

# --- overlay key via shared function -----------------------------------------

include("${CMAKE_CURRENT_LIST_DIR}/ComputeTxikiOverlayKey.cmake")
capsid_compute_txiki_overlay_key(
    OUT_KEY CAPSID_OVERLAY_KEY
    OUT_PATCHES CAPSID_KEY_PATCHES
    VENDOR_DIR "${CAPSID_TXIKI_VENDOR}"
    PATCH_DIR "${CAPSID_TXIKI_PATCH_DIR}"
    PREPARE_SCRIPT "${CAPSID_TXIKI_PREPARE_SCRIPT}"
)

# --- stamp verification ------------------------------------------------------

if(NOT EXISTS "${CAPSID_TXIKI_OVERLAY_STAMP}")
    message(FATAL_ERROR
        "txiki.js overlay stamp is missing: ${CAPSID_TXIKI_OVERLAY_STAMP}\n"
        "The overlay was not stamped during configure. Ensure the build "
        "writes this file after applying patches.")
endif()

get_filename_component(
    CAPSID_STAMP_NAME "${CAPSID_TXIKI_OVERLAY_STAMP}" NAME)
get_filename_component(
    CAPSID_TXIKI_OVERLAY "${CAPSID_TXIKI_OVERLAY_STAMP}" DIRECTORY)
if(NOT CAPSID_STAMP_NAME STREQUAL ".capsid-overlay-key")
    message(FATAL_ERROR
        "txiki.js overlay stamp must be named .capsid-overlay-key inside "
        "the prepared overlay: ${CAPSID_TXIKI_OVERLAY_STAMP}")
endif()

file(STRINGS "${CAPSID_TXIKI_OVERLAY_STAMP}" CAPSID_STAMP_LINES)
list(LENGTH CAPSID_STAMP_LINES CAPSID_STAMP_LINE_COUNT)
if(NOT CAPSID_STAMP_LINE_COUNT EQUAL 3)
    message(FATAL_ERROR
        "txiki.js overlay stamp has an invalid record count; expected "
        "schema, key, and manifest lines")
endif()
list(GET CAPSID_STAMP_LINES 0 CAPSID_STAMP_SCHEMA)
list(GET CAPSID_STAMP_LINES 1 CAPSID_STAMP_KEY_LINE)
list(GET CAPSID_STAMP_LINES 2 CAPSID_STAMP_MANIFEST_LINE)
if(NOT CAPSID_STAMP_SCHEMA STREQUAL
       "schema=capsid-txiki-overlay-stamp-v1" OR
   NOT CAPSID_STAMP_KEY_LINE MATCHES "^key=[0-9a-f]+$" OR
   NOT CAPSID_STAMP_MANIFEST_LINE MATCHES "^manifest=[0-9a-f]+$")
    message(FATAL_ERROR
        "txiki.js overlay stamp has an invalid schema or field encoding")
endif()
string(REGEX REPLACE "^key=" "" CAPSID_STAMPED_KEY
    "${CAPSID_STAMP_KEY_LINE}")
string(REGEX REPLACE "^manifest=" "" CAPSID_STAMPED_MANIFEST
    "${CAPSID_STAMP_MANIFEST_LINE}")
string(LENGTH "${CAPSID_STAMPED_KEY}" CAPSID_STAMPED_KEY_LENGTH)
string(LENGTH "${CAPSID_STAMPED_MANIFEST}"
    CAPSID_STAMPED_MANIFEST_LENGTH)
if(NOT CAPSID_STAMPED_KEY_LENGTH EQUAL 64 OR
   NOT CAPSID_STAMPED_MANIFEST_LENGTH EQUAL 64)
    message(FATAL_ERROR
        "txiki.js overlay stamp key and manifest must be SHA-256 values")
endif()

if(NOT CAPSID_STAMPED_KEY STREQUAL CAPSID_OVERLAY_KEY)
    message(FATAL_ERROR
        "txiki.js overlay key mismatch.\n"
        "  computed: ${CAPSID_OVERLAY_KEY}\n"
        "  stamped:  ${CAPSID_STAMPED_KEY}\n"
        "The overlay was built from a different combination of vendor "
        "revision, submodule revisions, patches, or PrepareTxiki.cmake "
        "than the audit expects. Re-run cmake configure to regenerate "
        "the overlay.")
endif()

capsid_compute_txiki_overlay_manifest(
    OUT_MANIFEST CAPSID_ACTUAL_OVERLAY_MANIFEST
    OVERLAY_DIR "${CAPSID_TXIKI_OVERLAY}"
    PATCHES ${CAPSID_KEY_PATCHES}
)
if(NOT CAPSID_STAMPED_MANIFEST STREQUAL
       CAPSID_ACTUAL_OVERLAY_MANIFEST)
    message(FATAL_ERROR
        "txiki.js overlay manifest mismatch.\n"
        "  computed: ${CAPSID_ACTUAL_OVERLAY_MANIFEST}\n"
        "  stamped:  ${CAPSID_STAMPED_MANIFEST}\n"
        "The prepared overlay was modified or the stamp was detached from "
        "the overlay it describes. Re-run cmake configure.")
endif()

# --- final report ------------------------------------------------------------

if(CAPSID_AUDIT_FAILURES)
    message(FATAL_ERROR
        "txiki.js vendor/patch integrity audit failed:"
        "${CAPSID_AUDIT_FAILURES}")
endif()

message(STATUS
    "txiki.js vendor clean, 11 patches apply, overlay key and manifest verified")
