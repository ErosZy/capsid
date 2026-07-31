# Negative control for AuditWptCoverage.cmake.
#
# The registered path set differs from executedProfile by exactly one entry but
# retains the same count. A count-only audit incorrectly accepts this fixture.

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
        "cannot build the WPT coverage negative control: ${CAPSID_JSON_ERROR}")
endif()

set(CAPSID_REGISTERED_PATHS "")
math(EXPR CAPSID_EXECUTED_LAST "${CAPSID_EXECUTED_COUNT} - 1")
foreach(CAPSID_INDEX RANGE 0 ${CAPSID_EXECUTED_LAST})
    string(JSON CAPSID_PATH GET
        "${CAPSID_MANIFEST_JSON}" executedProfile ${CAPSID_INDEX})
    list(APPEND CAPSID_REGISTERED_PATHS "${CAPSID_PATH}")
endforeach()
list(REMOVE_AT CAPSID_REGISTERED_PATHS 0)
list(INSERT CAPSID_REGISTERED_PATHS 0
    "synthetic/equal-count-substitution.any.js")

file(MAKE_DIRECTORY "${CAPSID_TEST_WORK_DIR}")
set(CAPSID_REGISTERED_PATHS_FILE
    "${CAPSID_TEST_WORK_DIR}/wpt-equal-count-substitution.txt")
string(REPLACE ";" "\n" CAPSID_REGISTERED_PATHS_TEXT
    "${CAPSID_REGISTERED_PATHS}")
file(WRITE "${CAPSID_REGISTERED_PATHS_FILE}"
    "${CAPSID_REGISTERED_PATHS_TEXT}\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DCAPSID_WPT_MANIFEST=${CAPSID_WPT_MANIFEST}"
        "-DCAPSID_WPT_REGISTERED_COUNT=${CAPSID_EXECUTED_COUNT}"
        "-DCAPSID_WPT_REGISTERED_PATHS_FILE=${CAPSID_REGISTERED_PATHS_FILE}"
        -P "${CAPSID_WPT_AUDIT_SCRIPT}"
    RESULT_VARIABLE CAPSID_AUDIT_RESULT
    OUTPUT_VARIABLE CAPSID_AUDIT_OUTPUT
    ERROR_VARIABLE CAPSID_AUDIT_ERROR
)

if(CAPSID_AUDIT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "WPT coverage audit accepted an equal-count path substitution; "
        "it must compare the exact registered path set, not only its length")
endif()

set(CAPSID_AUDIT_DIAGNOSTIC "${CAPSID_AUDIT_OUTPUT}\n${CAPSID_AUDIT_ERROR}")
if(NOT CAPSID_AUDIT_DIAGNOSTIC MATCHES "WPT coverage drift")
    message(FATAL_ERROR
        "WPT coverage audit failed for an unrelated reason:\n"
        "${CAPSID_AUDIT_DIAGNOSTIC}")
endif()

message(STATUS
    "WPT coverage audit rejected an equal-count path substitution")
