# WPT coverage manifest audit.
#
# Uses IN_LIST (CMP0057). CMake 3.28 defaults the policy to unset, which
# treats "IN_LIST" as an ordinary argument and breaks the membership test
# (the coverage audit would then pass vacuously or fail spuriously
# depending on the argument order); set it NEW explicitly.
cmake_policy(SET CMP0057 NEW)
if(POLICY CMP0121)
    # Range variables used below (RANGE 0 N) need the inclusive upper bound
    # semantics; keep the modern behavior.
    cmake_policy(SET CMP0121 NEW)
endif()

# tests/wpt/manifest.json declares two lists that must agree:
#
#   tests[].paths    -- the upstream files each conformance group claims as its
#                       evidence. docs/standards-matrix.md and
#                       docs/conformance-deviations.md cite these when closing a
#                       CAPSID-D gap.
#   executedProfile  -- the flat list of files actually turned into worker realms.
#
# If a path appears in a group but not in executedProfile, the documentation cites
# evidence that is never produced. Nothing else in the build cross-checks this, so
# the drift is otherwise invisible.
#
# Also verifies the number of registered CTest cases matches executedProfile, so a
# fixture that silently fails to generate cannot reduce coverage unnoticed.

if(NOT DEFINED CAPSID_WPT_MANIFEST OR NOT EXISTS "${CAPSID_WPT_MANIFEST}")
    message(FATAL_ERROR
        "CAPSID_WPT_MANIFEST must name the existing tests/wpt/manifest.json")
endif()

file(READ "${CAPSID_WPT_MANIFEST}" CAPSID_MANIFEST_JSON)

# --- executedProfile -------------------------------------------------------

string(JSON CAPSID_EXECUTED_COUNT ERROR_VARIABLE CAPSID_JSON_ERROR
    LENGTH "${CAPSID_MANIFEST_JSON}" executedProfile)
if(CAPSID_JSON_ERROR)
    message(FATAL_ERROR
        "manifest.json has no readable executedProfile array: ${CAPSID_JSON_ERROR}")
endif()
if(CAPSID_EXECUTED_COUNT LESS 1)
    message(FATAL_ERROR "manifest.json executedProfile is empty")
endif()

set(CAPSID_EXECUTED_PATHS "")
math(EXPR CAPSID_EXECUTED_LAST "${CAPSID_EXECUTED_COUNT} - 1")
foreach(CAPSID_INDEX RANGE 0 ${CAPSID_EXECUTED_LAST})
    string(JSON CAPSID_PATH GET "${CAPSID_MANIFEST_JSON}" executedProfile ${CAPSID_INDEX})
    list(APPEND CAPSID_EXECUTED_PATHS "${CAPSID_PATH}")
endforeach()

# --- tests[].paths ---------------------------------------------------------

string(JSON CAPSID_GROUP_COUNT ERROR_VARIABLE CAPSID_JSON_ERROR
    LENGTH "${CAPSID_MANIFEST_JSON}" tests)
if(CAPSID_JSON_ERROR)
    message(FATAL_ERROR
        "manifest.json has no readable tests array: ${CAPSID_JSON_ERROR}")
endif()

set(CAPSID_DECLARED_PATHS "")
set(CAPSID_MISSING_REPORT "")
math(EXPR CAPSID_GROUP_LAST "${CAPSID_GROUP_COUNT} - 1")
foreach(CAPSID_GROUP_INDEX RANGE 0 ${CAPSID_GROUP_LAST})
    string(JSON CAPSID_GROUP GET "${CAPSID_MANIFEST_JSON}" tests ${CAPSID_GROUP_INDEX})

    string(JSON CAPSID_GROUP_NAME ERROR_VARIABLE CAPSID_NAME_ERROR
        GET "${CAPSID_GROUP}" area)
    if(CAPSID_NAME_ERROR)
        set(CAPSID_GROUP_NAME "tests[${CAPSID_GROUP_INDEX}]")
    endif()

    string(JSON CAPSID_GROUP_GAP ERROR_VARIABLE CAPSID_GAP_ERROR
        GET "${CAPSID_GROUP}" gap)
    if(CAPSID_GAP_ERROR)
        set(CAPSID_GROUP_GAP "-")
    endif()

    string(JSON CAPSID_PATH_COUNT ERROR_VARIABLE CAPSID_PATHS_ERROR
        LENGTH "${CAPSID_GROUP}" paths)
    if(CAPSID_PATHS_ERROR)
        continue()
    endif()

    math(EXPR CAPSID_PATH_LAST "${CAPSID_PATH_COUNT} - 1")
    foreach(CAPSID_PATH_INDEX RANGE 0 ${CAPSID_PATH_LAST})
        string(JSON CAPSID_PATH GET "${CAPSID_GROUP}" paths ${CAPSID_PATH_INDEX})
        list(APPEND CAPSID_DECLARED_PATHS "${CAPSID_PATH}")

        if(NOT "${CAPSID_PATH}" IN_LIST CAPSID_EXECUTED_PATHS)
            string(APPEND CAPSID_MISSING_REPORT
                "\n    ${CAPSID_PATH}"
                "\n      declared by group '${CAPSID_GROUP_NAME}' (gap ${CAPSID_GROUP_GAP})")
        endif()
    endforeach()
endforeach()

if(CAPSID_MISSING_REPORT)
    message(FATAL_ERROR
        "WPT coverage drift: manifest.json declares upstream files as conformance "
        "evidence that are never executed.${CAPSID_MISSING_REPORT}\n\n"
        "Either add the path to executedProfile (and to the CAPSID_WPT_BATCH list in "
        "cmake/build_tests.cmake), or remove it from tests[].paths and reopen the "
        "corresponding gap in docs/conformance-deviations.md. Documentation must "
        "not cite evidence the suite does not produce.")
endif()

# --- registered CTest paths vs executedProfile ------------------------------

if(DEFINED CAPSID_WPT_REGISTERED_PATHS_FILE)
    if(NOT EXISTS "${CAPSID_WPT_REGISTERED_PATHS_FILE}")
        message(FATAL_ERROR
            "WPT coverage drift: CAPSID_WPT_REGISTERED_PATHS_FILE was explicitly "
            "supplied but the file does not exist\n  ${CAPSID_WPT_REGISTERED_PATHS_FILE}\n"
            "Falling back to count-only validation would silently accept a "
            "missing fixture list as sufficient coverage evidence.")
    endif()
    file(STRINGS "${CAPSID_WPT_REGISTERED_PATHS_FILE}" CAPSID_REGISTERED_PATHS
        REGEX "^[^#]")
    # Normalize: strip trailing carriage returns and whitespace.
    set(CAPSID_NORMALIZED_REGISTERED "")
    foreach(CAPSID_PATH IN LISTS CAPSID_REGISTERED_PATHS)
        string(STRIP "${CAPSID_PATH}" CAPSID_STRIPPED)
        if(CAPSID_STRIPPED)
            list(APPEND CAPSID_NORMALIZED_REGISTERED "${CAPSID_STRIPPED}")
        endif()
    endforeach()

    # Detect duplicates in the registered list.
    set(CAPSID_REGISTERED_DUPES "")
    set(CAPSID_SEEN_REGISTERED "")
    foreach(CAPSID_PATH IN LISTS CAPSID_NORMALIZED_REGISTERED)
        if("${CAPSID_PATH}" IN_LIST CAPSID_SEEN_REGISTERED)
            list(APPEND CAPSID_REGISTERED_DUPES "${CAPSID_PATH}")
        else()
            list(APPEND CAPSID_SEEN_REGISTERED "${CAPSID_PATH}")
        endif()
    endforeach()

    set(CAPSID_EXECUTED_DUPES "")
    set(CAPSID_SEEN_EXECUTED "")
    foreach(CAPSID_PATH IN LISTS CAPSID_EXECUTED_PATHS)
        if("${CAPSID_PATH}" IN_LIST CAPSID_SEEN_EXECUTED)
            list(APPEND CAPSID_EXECUTED_DUPES "${CAPSID_PATH}")
        else()
            list(APPEND CAPSID_SEEN_EXECUTED "${CAPSID_PATH}")
        endif()
    endforeach()

    # Bidirectional comparison: paths in registered but not in executed (missing
    # from the test run), and paths in executed but not in registered (unexpected
    # — the host registered a test the manifest doesn't document).
    set(CAPSID_MISSING "")
    foreach(CAPSID_PATH IN LISTS CAPSID_NORMALIZED_REGISTERED)
        if(NOT "${CAPSID_PATH}" IN_LIST CAPSID_EXECUTED_PATHS)
            list(APPEND CAPSID_MISSING "${CAPSID_PATH}")
        endif()
    endforeach()

    set(CAPSID_UNEXPECTED "")
    foreach(CAPSID_PATH IN LISTS CAPSID_EXECUTED_PATHS)
        if(NOT "${CAPSID_PATH}" IN_LIST CAPSID_NORMALIZED_REGISTERED)
            list(APPEND CAPSID_UNEXPECTED "${CAPSID_PATH}")
        endif()
    endforeach()

    set(CAPSID_PATH_FAILURES "")

    if(CAPSID_REGISTERED_DUPES)
        string(REPLACE ";" "\n    " CAPSID_INDENTED "${CAPSID_REGISTERED_DUPES}")
        string(APPEND CAPSID_PATH_FAILURES
            "\n  duplicate paths in the registered list:\n    ${CAPSID_INDENTED}")
    endif()

    if(CAPSID_EXECUTED_DUPES)
        string(REPLACE ";" "\n    " CAPSID_INDENTED "${CAPSID_EXECUTED_DUPES}")
        string(APPEND CAPSID_PATH_FAILURES
            "\n  duplicate paths in executedProfile:\n    ${CAPSID_INDENTED}")
    endif()

    if(CAPSID_MISSING)
        string(REPLACE ";" "\n    " CAPSID_INDENTED "${CAPSID_MISSING}")
        string(APPEND CAPSID_PATH_FAILURES
            "\n  paths registered as CTest cases but not in executedProfile:"
            "\n    ${CAPSID_INDENTED}")
    endif()

    if(CAPSID_UNEXPECTED)
        string(REPLACE ";" "\n    " CAPSID_INDENTED "${CAPSID_UNEXPECTED}")
        string(APPEND CAPSID_PATH_FAILURES
            "\n  paths in executedProfile but not registered as CTest cases:"
            "\n    ${CAPSID_INDENTED}")
    endif()

    if(CAPSID_PATH_FAILURES)
        list(LENGTH CAPSID_NORMALIZED_REGISTERED CAPSID_REGISTERED_COUNT)
        message(FATAL_ERROR
            "WPT coverage drift: the registered CTest path set and "
            "executedProfile differ.${CAPSID_PATH_FAILURES}\n\n"
            "A fixture that silently fails to generate or a path that is "
            "accidentally duplicated reduces conformance coverage; this "
            "mismatch is treated as a failure.")
    endif()

    # Count is a secondary consistency check, not the primary audit.
    list(LENGTH CAPSID_NORMALIZED_REGISTERED CAPSID_REGISTERED_COUNT)
    if(NOT CAPSID_REGISTERED_COUNT EQUAL CAPSID_EXECUTED_COUNT)
        list(LENGTH CAPSID_EXECUTED_PATHS CAPSID_EXECUTED_COUNT)
        message(FATAL_ERROR
            "WPT coverage drift: ${CAPSID_EXECUTED_COUNT} paths in "
            "manifest.json executedProfile but ${CAPSID_REGISTERED_COUNT} CTest "
            "cases registered. This count mismatch accompanies a path-level "
            "discrepancy; fix the path differences first.\n"
            "A fixture that fails to generate silently reduces conformance "
            "coverage; this mismatch is treated as a failure.")
    endif()
elseif(DEFINED CAPSID_WPT_REGISTERED_COUNT)
    if(NOT CAPSID_WPT_REGISTERED_COUNT EQUAL CAPSID_EXECUTED_COUNT)
        message(FATAL_ERROR
            "WPT coverage drift: ${CAPSID_EXECUTED_COUNT} paths in "
            "manifest.json executedProfile but ${CAPSID_WPT_REGISTERED_COUNT} CTest "
            "cases registered.\n"
            "A fixture that fails to generate silently reduces conformance "
            "coverage; this mismatch is treated as a failure.")
    endif()
endif()

list(LENGTH CAPSID_DECLARED_PATHS CAPSID_DECLARED_COUNT)
message(STATUS
    "WPT coverage manifest consistent: ${CAPSID_DECLARED_COUNT} declared paths, "
    "all present in executedProfile (${CAPSID_EXECUTED_COUNT} executed)")
