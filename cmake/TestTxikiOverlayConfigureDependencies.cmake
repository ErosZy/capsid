# Verify that every input used to construct the txiki.js overlay participates
# in CMake's generated reconfiguration graph.
#
# Patch contents and PrepareTxiki.cmake affect the overlay key during configure,
# so changing them must cause CMake to run again. CONFIGURE_DEPENDS on the patch
# glob is also required so adding or removing a patch cannot leave a stale
# overlay.

foreach(CAPSID_REQUIRED_INPUT
        CAPSID_CMAKE_CONFIGURE_DEPENDS
        CAPSID_CMAKE_VERIFY_GLOBS
        CAPSID_TXIKI_VENDOR
        CAPSID_TXIKI_PATCH_DIR
        CAPSID_TXIKI_PREPARE_SCRIPT
        CAPSID_TXIKI_KEY_SCRIPT
        CAPSID_CAPABILITY_MANIFEST)
    if(NOT DEFINED ${CAPSID_REQUIRED_INPUT})
        message(FATAL_ERROR "${CAPSID_REQUIRED_INPUT} is required")
    endif()
endforeach()

if(NOT EXISTS "${CAPSID_CMAKE_CONFIGURE_DEPENDS}")
    message(FATAL_ERROR
        "CMake reconfiguration metadata is missing: "
        "${CAPSID_CMAKE_CONFIGURE_DEPENDS}")
endif()
if(NOT EXISTS "${CAPSID_CMAKE_VERIFY_GLOBS}")
    message(FATAL_ERROR
        "CMake glob verification metadata is missing: "
        "${CAPSID_CMAKE_VERIFY_GLOBS}")
endif()

file(STRINGS "${CAPSID_CMAKE_CONFIGURE_DEPENDS}"
    CAPSID_REGISTERED_DEPENDENCIES)
set(CAPSID_REGISTERED_DEPENDENCIES_NORMALIZED "")
foreach(CAPSID_REGISTERED_DEPENDENCY IN LISTS
        CAPSID_REGISTERED_DEPENDENCIES)
    file(TO_CMAKE_PATH
        "${CAPSID_REGISTERED_DEPENDENCY}"
        CAPSID_REGISTERED_DEPENDENCY_NORMALIZED)
    list(APPEND CAPSID_REGISTERED_DEPENDENCIES_NORMALIZED
        "${CAPSID_REGISTERED_DEPENDENCY_NORMALIZED}")
endforeach()
list(REMOVE_DUPLICATES CAPSID_REGISTERED_DEPENDENCIES_NORMALIZED)

file(GLOB CAPSID_TXIKI_PATCHES "${CAPSID_TXIKI_PATCH_DIR}/*.patch")
list(SORT CAPSID_TXIKI_PATCHES)
set(CAPSID_REQUIRED_DEPENDENCIES
    "${CAPSID_TXIKI_PREPARE_SCRIPT}"
    "${CAPSID_TXIKI_KEY_SCRIPT}"
    "${CAPSID_CAPABILITY_MANIFEST}"
    "${CAPSID_TXIKI_VENDOR}/CMakeLists.txt"
    "${CAPSID_TXIKI_VENDOR}/LICENSE"
    "${CAPSID_TXIKI_VENDOR}/src/wasm.c"
    "${CAPSID_TXIKI_VENDOR}/src/js/polyfills/wasm.js"
    "${CAPSID_TXIKI_VENDOR}/tests/fixtures/hello.txt"
    "${CAPSID_TXIKI_VENDOR}/deps/wamr/core/iwasm/interpreter/wasm_loader.c"
    "${CAPSID_TXIKI_VENDOR}/deps/libwebsockets/include/libwebsockets/lws-network-helper.h"
    ${CAPSID_TXIKI_PATCHES})

find_program(CAPSID_GIT_EXECUTABLE git REQUIRED)
execute_process(
    COMMAND "${CAPSID_GIT_EXECUTABLE}" -C "${CAPSID_TXIKI_VENDOR}"
        rev-parse --absolute-git-dir
    OUTPUT_VARIABLE CAPSID_VENDOR_GIT_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE CAPSID_VENDOR_GIT_RESULT
)
if(NOT CAPSID_VENDOR_GIT_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot resolve txiki.js git directory")
endif()
foreach(CAPSID_GIT_STATE_FILE IN ITEMS
        "${CAPSID_VENDOR_GIT_DIR}/HEAD"
        "${CAPSID_VENDOR_GIT_DIR}/index"
        "${CAPSID_VENDOR_GIT_DIR}/packed-refs")
    if(EXISTS "${CAPSID_GIT_STATE_FILE}")
        list(APPEND CAPSID_REQUIRED_DEPENDENCIES
            "${CAPSID_GIT_STATE_FILE}")
    endif()
endforeach()

execute_process(
    COMMAND "${CAPSID_GIT_EXECUTABLE}" -C "${CAPSID_TXIKI_VENDOR}"
        submodule foreach --recursive --quiet
        "printf '%s\\n' \"$(git rev-parse --absolute-git-dir)\""
    OUTPUT_VARIABLE CAPSID_SUBMODULE_GIT_DIRS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE CAPSID_SUBMODULE_GIT_RESULT
)
if(NOT CAPSID_SUBMODULE_GIT_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot resolve txiki.js submodule git directories")
endif()
if(CAPSID_SUBMODULE_GIT_DIRS)
    string(REPLACE "\n" ";" CAPSID_SUBMODULE_GIT_DIR_LIST
        "${CAPSID_SUBMODULE_GIT_DIRS}")
    foreach(CAPSID_SUBMODULE_GIT_DIR IN LISTS CAPSID_SUBMODULE_GIT_DIR_LIST)
        foreach(CAPSID_GIT_STATE_NAME IN ITEMS HEAD index packed-refs)
            set(CAPSID_GIT_STATE_FILE
                "${CAPSID_SUBMODULE_GIT_DIR}/${CAPSID_GIT_STATE_NAME}")
            if(EXISTS "${CAPSID_GIT_STATE_FILE}")
                list(APPEND CAPSID_REQUIRED_DEPENDENCIES
                    "${CAPSID_GIT_STATE_FILE}")
            endif()
        endforeach()
    endforeach()
endif()
list(REMOVE_DUPLICATES CAPSID_REQUIRED_DEPENDENCIES)

set(CAPSID_MISSING_DEPENDENCIES "")
foreach(CAPSID_REQUIRED_DEPENDENCY IN LISTS CAPSID_REQUIRED_DEPENDENCIES)
    file(TO_CMAKE_PATH
        "${CAPSID_REQUIRED_DEPENDENCY}" CAPSID_NORMALIZED_DEPENDENCY)
    list(FIND CAPSID_REGISTERED_DEPENDENCIES_NORMALIZED
        "${CAPSID_NORMALIZED_DEPENDENCY}" CAPSID_DEPENDENCY_INDEX)
    if(CAPSID_DEPENDENCY_INDEX EQUAL -1)
        list(APPEND CAPSID_MISSING_DEPENDENCIES
            "${CAPSID_NORMALIZED_DEPENDENCY}")
    endif()
endforeach()

file(READ "${CAPSID_CMAKE_VERIFY_GLOBS}" CAPSID_GLOB_METADATA)
file(TO_CMAKE_PATH "${CAPSID_TXIKI_PATCH_DIR}" CAPSID_NORMALIZED_PATCH_DIR)
set(CAPSID_PATCH_GLOB "${CAPSID_NORMALIZED_PATCH_DIR}/*.patch")
string(FIND "${CAPSID_GLOB_METADATA}"
    "\"${CAPSID_PATCH_GLOB}\"" CAPSID_PATCH_GLOB_INDEX)

if(CAPSID_MISSING_DEPENDENCIES OR CAPSID_PATCH_GLOB_INDEX EQUAL -1)
    set(CAPSID_FAILURE_REPORT "")
    if(CAPSID_MISSING_DEPENDENCIES)
        string(REPLACE ";" "\n    " CAPSID_INDENTED_MISSING
            "${CAPSID_MISSING_DEPENDENCIES}")
        string(APPEND CAPSID_FAILURE_REPORT
            "\n  inputs missing from the reconfiguration graph:"
            "\n    ${CAPSID_INDENTED_MISSING}")
    endif()
    if(CAPSID_PATCH_GLOB_INDEX EQUAL -1)
        string(APPEND CAPSID_FAILURE_REPORT
            "\n  patch glob is not registered with CONFIGURE_DEPENDS:"
            "\n    ${CAPSID_PATCH_GLOB}")
    endif()
    message(FATAL_ERROR
        "txiki.js overlay can remain stale after an input changes."
        "${CAPSID_FAILURE_REPORT}")
endif()

message(STATUS
    "txiki.js overlay inputs participate in CMake reconfiguration")
