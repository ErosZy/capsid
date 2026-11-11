if(NOT DEFINED CAPSID_COMPILE_COMMANDS OR
   NOT EXISTS "${CAPSID_COMPILE_COMMANDS}")
    message(FATAL_ERROR "Host compile_commands.json is missing")
endif()
if(NOT CAPSID_NODE_EXECUTABLE)
    find_program(CAPSID_NODE_EXECUTABLE NAMES node REQUIRED)
endif()
get_filename_component(
    CAPSID_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
execute_process(
    COMMAND "${CAPSID_NODE_EXECUTABLE}"
        "${CAPSID_SOURCE_ROOT}/tests/audit-host-sanitizers.mjs"
        --compile-commands "${CAPSID_COMPILE_COMMANDS}"
        --source-root "${CAPSID_SOURCE_ROOT}"
        --asan "${CAPSID_EXPECT_ASAN}"
        --ubsan "${CAPSID_EXPECT_UBSAN}"
        --tsan "${CAPSID_EXPECT_TSAN}"
        --fuzzers "${CAPSID_EXPECT_FUZZERS}"
        --vendor-root "${CAPSID_VENDOR_ROOT}"
        --worker-writer-fixture "${CAPSID_WORKER_WRITER_FIXTURE}"
        --worker-timeout-writer-fixture "${CAPSID_WORKER_TIMEOUT_WRITER_FIXTURE}"
    RESULT_VARIABLE CAPSID_AUDIT_EXIT
    OUTPUT_VARIABLE CAPSID_AUDIT_OUTPUT
    ERROR_VARIABLE CAPSID_AUDIT_ERROR)
if(NOT CAPSID_AUDIT_EXIT EQUAL 0)
    message(FATAL_ERROR
        "Sanitizer compile audit failed (${CAPSID_AUDIT_EXIT}):\n"
        "${CAPSID_AUDIT_OUTPUT}${CAPSID_AUDIT_ERROR}")
endif()
message(STATUS "${CAPSID_AUDIT_OUTPUT}")
