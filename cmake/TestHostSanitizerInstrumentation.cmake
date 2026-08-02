if(NOT DEFINED CAPSID_COMPILE_COMMANDS OR
   NOT EXISTS "${CAPSID_COMPILE_COMMANDS}")
    message(FATAL_ERROR "Host compile_commands.json is missing")
endif()

file(READ "${CAPSID_COMPILE_COMMANDS}" CAPSID_COMMANDS)

set(CAPSID_HOST_SECURITY_SOURCES
    "src/host/config\\.cc"
    "src/host/bytecode_attestation\\.cc"
    "src/host/secret_snapshot\\.cc"
    "src/host/generation_identity\\.cc"
    "src/host/active_state\\.cc"
    "src/host/request_normalization\\.cc"
    "src/host/service_lifecycle\\.cc"
    "src/host/worker_recovery\\.cc"
    "src/host/single_worker_server\\.cc"
    "src/host/main\\.cc"
    "vendor/jansson/src/load\\.c")

foreach(CAPSID_SOURCE IN LISTS CAPSID_HOST_SECURITY_SOURCES)
    if(CAPSID_EXPECT_ASAN)
        string(REGEX MATCH
            "-fsanitize=address[^}]*${CAPSID_SOURCE}"
            CAPSID_ASAN_MATCH
            "${CAPSID_COMMANDS}")
        if(NOT CAPSID_ASAN_MATCH)
            message(FATAL_ERROR
                "ASan does not instrument ${CAPSID_SOURCE}")
        endif()
    endif()

    if(CAPSID_EXPECT_UBSAN)
        string(REGEX MATCH
            "-fsanitize=undefined[^}]*${CAPSID_SOURCE}"
            CAPSID_UBSAN_MATCH
            "${CAPSID_COMMANDS}")
        if(NOT CAPSID_UBSAN_MATCH)
            message(FATAL_ERROR
                "UBSan does not instrument ${CAPSID_SOURCE}")
        endif()
    endif()
endforeach()
