# Negative controls for the txiki.js overlay integrity audit.
#
# These controls verify that the audit is connected to the stamp consumed by
# the build and that PrepareTxiki.cmake is part of the same fingerprint. They
# intentionally avoid prescribing the fingerprint serialization or hash
# algorithm.

foreach(CAPSID_REQUIRED_INPUT
        CAPSID_TXIKI_AUDIT_SCRIPT
        CAPSID_TXIKI_VENDOR
        CAPSID_TXIKI_PATCH_DIR
        CAPSID_TXIKI_OVERLAY_STAMP
        CAPSID_TXIKI_PREPARE_SCRIPT
        CAPSID_TEST_WORK_DIR)
    if(NOT DEFINED ${CAPSID_REQUIRED_INPUT})
        message(FATAL_ERROR "${CAPSID_REQUIRED_INPUT} is required")
    endif()
endforeach()

foreach(CAPSID_REQUIRED_FILE
        CAPSID_TXIKI_AUDIT_SCRIPT
        CAPSID_TXIKI_OVERLAY_STAMP
        CAPSID_TXIKI_PREPARE_SCRIPT)
    if(NOT EXISTS "${${CAPSID_REQUIRED_FILE}}")
        message(FATAL_ERROR
            "${CAPSID_REQUIRED_FILE} does not exist: "
            "${${CAPSID_REQUIRED_FILE}}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${CAPSID_TEST_WORK_DIR}")
set(CAPSID_BOGUS_STAMP "${CAPSID_TEST_WORK_DIR}/bogus-overlay-key")
file(WRITE "${CAPSID_BOGUS_STAMP}" "deliberately-not-the-overlay-key\n")

set(CAPSID_DETACHED_OVERLAY
    "${CAPSID_TEST_WORK_DIR}/detached-overlay")
file(REMOVE_RECURSE "${CAPSID_DETACHED_OVERLAY}")
file(MAKE_DIRECTORY "${CAPSID_DETACHED_OVERLAY}")
set(CAPSID_DETACHED_STAMP
    "${CAPSID_DETACHED_OVERLAY}/.capsid-overlay-key")
configure_file(
    "${CAPSID_TXIKI_OVERLAY_STAMP}"
    "${CAPSID_DETACHED_STAMP}"
    COPYONLY)

get_filename_component(
    CAPSID_REAL_OVERLAY "${CAPSID_TXIKI_OVERLAY_STAMP}" DIRECTORY)
set(CAPSID_TAMPERED_OVERLAY
    "${CAPSID_TEST_WORK_DIR}/tampered-overlay")
file(REMOVE_RECURSE "${CAPSID_TAMPERED_OVERLAY}")
file(MAKE_DIRECTORY "${CAPSID_TAMPERED_OVERLAY}")
file(GLOB CAPSID_PATCH_FILES "${CAPSID_TXIKI_PATCH_DIR}/*.patch")
list(SORT CAPSID_PATCH_FILES)
set(CAPSID_PATCHED_PATHS "")
foreach(CAPSID_PATCH_FILE IN LISTS CAPSID_PATCH_FILES)
    file(STRINGS "${CAPSID_PATCH_FILE}" CAPSID_PATCH_HEADERS
        REGEX "^\\+\\+\\+ b/")
    foreach(CAPSID_PATCH_HEADER IN LISTS CAPSID_PATCH_HEADERS)
        string(REGEX REPLACE "^\\+\\+\\+ b/([^ \t]+).*$" "\\1"
            CAPSID_PATCHED_PATH "${CAPSID_PATCH_HEADER}")
        list(APPEND CAPSID_PATCHED_PATHS "${CAPSID_PATCHED_PATH}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES CAPSID_PATCHED_PATHS)
list(SORT CAPSID_PATCHED_PATHS)
foreach(CAPSID_PATCHED_PATH IN LISTS CAPSID_PATCHED_PATHS)
    get_filename_component(
        CAPSID_PATCHED_PARENT "${CAPSID_PATCHED_PATH}" DIRECTORY)
    file(MAKE_DIRECTORY
        "${CAPSID_TAMPERED_OVERLAY}/${CAPSID_PATCHED_PARENT}")
    configure_file(
        "${CAPSID_REAL_OVERLAY}/${CAPSID_PATCHED_PATH}"
        "${CAPSID_TAMPERED_OVERLAY}/${CAPSID_PATCHED_PATH}"
        COPYONLY)
endforeach()
configure_file(
    "${CAPSID_TXIKI_OVERLAY_STAMP}"
    "${CAPSID_TAMPERED_OVERLAY}/.capsid-overlay-key"
    COPYONLY)
list(GET CAPSID_PATCHED_PATHS 0 CAPSID_TAMPERED_PATH)
file(APPEND
    "${CAPSID_TAMPERED_OVERLAY}/${CAPSID_TAMPERED_PATH}"
    "\n# overlay manifest negative control\n")

file(READ "${CAPSID_TXIKI_PREPARE_SCRIPT}" CAPSID_PREPARE_CONTENT)
set(CAPSID_CHANGED_PREPARE
    "${CAPSID_TEST_WORK_DIR}/PrepareTxiki.changed.cmake")
file(WRITE "${CAPSID_CHANGED_PREPARE}"
    "${CAPSID_PREPARE_CONTENT}\n# overlay-key negative control\n")

set(CAPSID_NEGATIVE_CONTROL_FAILURES "")

function(capsid_expect_overlay_rejection CAPSID_LABEL CAPSID_STAMP CAPSID_PREPARE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DCAPSID_TXIKI_VENDOR=${CAPSID_TXIKI_VENDOR}"
            "-DCAPSID_TXIKI_PATCH_DIR=${CAPSID_TXIKI_PATCH_DIR}"
            "-DCAPSID_TXIKI_EXPECTED_TAG=v26.6.0"
            "-DCAPSID_TXIKI_OVERLAY_STAMP=${CAPSID_STAMP}"
            "-DCAPSID_TXIKI_PREPARE_SCRIPT=${CAPSID_PREPARE}"
            "-DCAPSID_TXIKI_PROBE_DIR=${CAPSID_TEST_WORK_DIR}/audit-probe"
            -P "${CAPSID_TXIKI_AUDIT_SCRIPT}"
        RESULT_VARIABLE CAPSID_AUDIT_RESULT
        OUTPUT_VARIABLE CAPSID_AUDIT_OUTPUT
        ERROR_VARIABLE CAPSID_AUDIT_ERROR
    )

    if(CAPSID_AUDIT_RESULT EQUAL 0)
        list(APPEND CAPSID_NEGATIVE_CONTROL_FAILURES
            "${CAPSID_LABEL}: audit unexpectedly succeeded")
    else()
        set(CAPSID_AUDIT_DIAGNOSTIC
            "${CAPSID_AUDIT_OUTPUT}\n${CAPSID_AUDIT_ERROR}")
        if(NOT CAPSID_AUDIT_DIAGNOSTIC MATCHES "[Oo]verlay")
            list(APPEND CAPSID_NEGATIVE_CONTROL_FAILURES
                "${CAPSID_LABEL}: audit failed for an unrelated reason")
        endif()
    endif()

    set(CAPSID_NEGATIVE_CONTROL_FAILURES
        "${CAPSID_NEGATIVE_CONTROL_FAILURES}" PARENT_SCOPE)
endfunction()

capsid_expect_overlay_rejection(
    "bogus stamp"
    "${CAPSID_BOGUS_STAMP}"
    "${CAPSID_TXIKI_PREPARE_SCRIPT}")
capsid_expect_overlay_rejection(
    "changed PrepareTxiki.cmake"
    "${CAPSID_TXIKI_OVERLAY_STAMP}"
    "${CAPSID_CHANGED_PREPARE}")
capsid_expect_overlay_rejection(
    "stamp detached from prepared overlay"
    "${CAPSID_DETACHED_STAMP}"
    "${CAPSID_TXIKI_PREPARE_SCRIPT}")
capsid_expect_overlay_rejection(
    "tampered prepared overlay"
    "${CAPSID_TAMPERED_OVERLAY}/.capsid-overlay-key"
    "${CAPSID_TXIKI_PREPARE_SCRIPT}")

if(CAPSID_NEGATIVE_CONTROL_FAILURES)
    string(REPLACE ";" "\n  " CAPSID_FAILURE_REPORT
        "${CAPSID_NEGATIVE_CONTROL_FAILURES}")
    message(FATAL_ERROR
        "txiki.js overlay audit negative controls failed:\n"
        "  ${CAPSID_FAILURE_REPORT}")
endif()

message(STATUS
    "txiki.js overlay audit rejected bogus, stale, and detached stamps")
