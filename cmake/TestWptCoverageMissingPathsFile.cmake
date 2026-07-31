# Negative control for AuditWptCoverage.cmake.
#
# Once the caller explicitly supplies CAPSID_WPT_REGISTERED_PATHS_FILE, silently
# falling back to the count-only audit when that file is absent defeats the
# path-level coverage guarantee.

foreach(CAPSID_REQUIRED_INPUT
        CAPSID_WPT_AUDIT_SCRIPT
        CAPSID_WPT_MANIFEST
        CAPSID_TEST_WORK_DIR)
    if(NOT DEFINED ${CAPSID_REQUIRED_INPUT})
        message(FATAL_ERROR "${CAPSID_REQUIRED_INPUT} is required")
    endif()
endforeach()

file(READ "${CAPSID_WPT_MANIFEST}" CAPSID_MANIFEST_JSON)
string(JSON CAPSID_EXECUTED_COUNT ERROR_VARIABLE CAPSID_JSON_ERROR
    LENGTH "${CAPSID_MANIFEST_JSON}" executedProfile)
if(CAPSID_JSON_ERROR OR CAPSID_EXECUTED_COUNT LESS 1)
    message(FATAL_ERROR
        "cannot build the WPT missing-path-file negative control: "
        "${CAPSID_JSON_ERROR}")
endif()

file(MAKE_DIRECTORY "${CAPSID_TEST_WORK_DIR}")
set(CAPSID_MISSING_PATHS_FILE
    "${CAPSID_TEST_WORK_DIR}/deliberately-missing-registered-paths.txt")
file(REMOVE "${CAPSID_MISSING_PATHS_FILE}")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DCAPSID_WPT_MANIFEST=${CAPSID_WPT_MANIFEST}"
        "-DCAPSID_WPT_REGISTERED_COUNT=${CAPSID_EXECUTED_COUNT}"
        "-DCAPSID_WPT_REGISTERED_PATHS_FILE=${CAPSID_MISSING_PATHS_FILE}"
        -P "${CAPSID_WPT_AUDIT_SCRIPT}"
    RESULT_VARIABLE CAPSID_AUDIT_RESULT
    OUTPUT_VARIABLE CAPSID_AUDIT_OUTPUT
    ERROR_VARIABLE CAPSID_AUDIT_ERROR
)

if(CAPSID_AUDIT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "WPT coverage audit accepted an explicitly supplied registered-path "
        "file that does not exist; it must not fall back to count-only "
        "validation")
endif()

message(STATUS
    "WPT coverage audit rejected a missing registered-path file")
