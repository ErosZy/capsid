foreach(CAPSID_REQUIRED
        CAPSID_AUDIT_SCRIPT
        CAPSID_WORKER
        CAPSID_TJS_ARCHIVE
        CAPSID_NM
        CAPSID_AR
        CAPSID_STRIP)
    if(NOT DEFINED ${CAPSID_REQUIRED})
        message(FATAL_ERROR "${CAPSID_REQUIRED} is required")
    endif()
endforeach()
if(NOT EXISTS "${CAPSID_AUDIT_SCRIPT}" OR
   NOT EXISTS "${CAPSID_WORKER}" OR
   NOT EXISTS "${CAPSID_TJS_ARCHIVE}")
    message(FATAL_ERROR "restricted audit inputs must exist")
endif()
if(NOT DEFINED CAPSID_EXPECT_LTO)
    message(FATAL_ERROR "CAPSID_EXPECT_LTO is required")
endif()
if(NOT DEFINED CAPSID_ENABLE_BINARY_INJECTION)
    message(FATAL_ERROR "CAPSID_ENABLE_BINARY_INJECTION is required")
endif()
if(CAPSID_ENABLE_BINARY_INJECTION AND
   (NOT DEFINED CAPSID_OBJCOPY OR CAPSID_OBJCOPY STREQUAL ""))
    message(FATAL_ERROR
        "CAPSID_OBJCOPY is required when binary injection is enabled")
endif()
if(NOT DEFINED CAPSID_TEST_WORK_DIR)
    message(FATAL_ERROR "CAPSID_TEST_WORK_DIR is required")
endif()

file(REMOVE_RECURSE "${CAPSID_TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${CAPSID_TEST_WORK_DIR}")

function(capsid_expect_audit_failure worker pattern label)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DCAPSID_WORKER=${worker}"
            "-DCAPSID_TJS_ARCHIVE=${CAPSID_TJS_ARCHIVE}"
            "-DCAPSID_NM=${CAPSID_NM}"
            "-DCAPSID_AR=${CAPSID_AR}"
            "-DCAPSID_EXPECT_LTO=${CAPSID_EXPECT_LTO}"
            -P "${CAPSID_AUDIT_SCRIPT}"
        RESULT_VARIABLE CAPSID_RESULT
        OUTPUT_VARIABLE CAPSID_STDOUT
        ERROR_VARIABLE CAPSID_STDERR
    )
    if(CAPSID_RESULT EQUAL 0)
        message(FATAL_ERROR
            "${label}: restricted worker audit unexpectedly passed")
    endif()
    set(CAPSID_OUTPUT "${CAPSID_STDOUT}\n${CAPSID_STDERR}")
    if(NOT CAPSID_OUTPUT MATCHES "${pattern}")
        message(FATAL_ERROR
            "${label}: audit failed for the wrong reason:\n${CAPSID_OUTPUT}")
    endif()
endfunction()

set(CAPSID_STRIPPED "${CAPSID_TEST_WORK_DIR}/capsid-worker-stripped")
configure_file("${CAPSID_WORKER}" "${CAPSID_STRIPPED}" COPYONLY)
execute_process(
    COMMAND "${CAPSID_STRIP}" --strip-all "${CAPSID_STRIPPED}"
    RESULT_VARIABLE CAPSID_STRIP_RESULT
    ERROR_VARIABLE CAPSID_STRIP_ERROR)
if(NOT CAPSID_STRIP_RESULT EQUAL 0)
    message(FATAL_ERROR "strip negative-control artifact: ${CAPSID_STRIP_ERROR}")
endif()
capsid_expect_audit_failure(
    "${CAPSID_STRIPPED}"
    "symbol audit cannot run|not trustworthy"
    "stripped artifact")

set(CAPSID_SPECIFIER "${CAPSID_TEST_WORK_DIR}/capsid-worker-specifier")
configure_file("${CAPSID_WORKER}" "${CAPSID_SPECIFIER}" COPYONLY)
file(APPEND "${CAPSID_SPECIFIER}" "\ntjs:fs\n")
capsid_expect_audit_failure(
    "${CAPSID_SPECIFIER}"
    "forbidden txiki.js module name"
    "forbidden module specifier")

if(CAPSID_ENABLE_BINARY_INJECTION)
    set(CAPSID_SYMBOL "${CAPSID_TEST_WORK_DIR}/capsid-worker-symbol")
    configure_file("${CAPSID_WORKER}" "${CAPSID_SYMBOL}" COPYONLY)
    execute_process(
        COMMAND "${CAPSID_OBJCOPY}"
            --add-symbol "tjs__mod_fs_init=.text:0,global"
            "${CAPSID_SYMBOL}"
        RESULT_VARIABLE CAPSID_OBJCOPY_RESULT
        ERROR_VARIABLE CAPSID_OBJCOPY_ERROR)
    if(NOT CAPSID_OBJCOPY_RESULT EQUAL 0)
        message(FATAL_ERROR
            "inject forbidden symbol: ${CAPSID_OBJCOPY_ERROR}")
    endif()
    capsid_expect_audit_failure(
        "${CAPSID_SYMBOL}"
        "forbidden symbol is present"
        "forbidden native symbol")

    set(CAPSID_UNIT "${CAPSID_TEST_WORK_DIR}/capsid-worker-unit")
    configure_file("${CAPSID_WORKER}" "${CAPSID_UNIT}" COPYONLY)
    execute_process(
        COMMAND "${CAPSID_OBJCOPY}"
            --add-symbol "mod_fs.c.deadbeef=.text:0,global"
            "${CAPSID_UNIT}"
        RESULT_VARIABLE CAPSID_OBJCOPY_RESULT
        ERROR_VARIABLE CAPSID_OBJCOPY_ERROR)
    if(NOT CAPSID_OBJCOPY_RESULT EQUAL 0)
        message(FATAL_ERROR
            "inject forbidden translation unit: ${CAPSID_OBJCOPY_ERROR}")
    endif()
    capsid_expect_audit_failure(
        "${CAPSID_UNIT}"
        "forbidden translation unit is linked"
        "forbidden LTO translation unit")

    message(STATUS
        "restricted worker audit rejects stripped, specifier, symbol, and TU injections")
else()
    message(STATUS
        "restricted worker audit rejects stripped and specifier inputs; "
        "ELF symbol/TU injection is unavailable on this platform")
endif()
