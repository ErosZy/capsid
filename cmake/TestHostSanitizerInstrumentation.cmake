if(NOT DEFINED CAPSID_COMPILE_COMMANDS OR
   NOT EXISTS "${CAPSID_COMPILE_COMMANDS}")
    message(FATAL_ERROR "Host compile_commands.json is missing")
endif()

file(READ "${CAPSID_COMPILE_COMMANDS}" CAPSID_COMMANDS)

# Audit every Host translation unit that this configuration actually builds.
# Keeping a hand-maintained security-source list allowed newly added Admin and
# managed-service files to miss the instrumentation gate silently.
get_filename_component(
    CAPSID_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(GLOB CAPSID_HOST_SOURCE_FILES
    RELATIVE "${CAPSID_SOURCE_ROOT}"
    "${CAPSID_SOURCE_ROOT}/src/host/*.cc")
list(APPEND CAPSID_HOST_SOURCE_FILES "vendor/jansson/src/load.c")

foreach(CAPSID_SOURCE IN LISTS CAPSID_HOST_SOURCE_FILES)
    string(REPLACE "." "\\." CAPSID_SOURCE_REGEX "${CAPSID_SOURCE}")
    string(REGEX MATCH "${CAPSID_SOURCE_REGEX}" CAPSID_SOURCE_IS_BUILT
        "${CAPSID_COMMANDS}")
    if(NOT CAPSID_SOURCE_IS_BUILT)
        continue()
    endif()

    if(CAPSID_EXPECT_ASAN)
        string(REGEX MATCH
            "-fsanitize=address[^}]*${CAPSID_SOURCE_REGEX}"
            CAPSID_ASAN_MATCH
            "${CAPSID_COMMANDS}")
        if(NOT CAPSID_ASAN_MATCH)
            message(FATAL_ERROR
                "ASan does not instrument ${CAPSID_SOURCE}")
        endif()
    endif()

    if(CAPSID_EXPECT_UBSAN)
        string(REGEX MATCH
            "-fsanitize=undefined[^}]*${CAPSID_SOURCE_REGEX}"
            CAPSID_UBSAN_MATCH
            "${CAPSID_COMMANDS}")
        if(NOT CAPSID_UBSAN_MATCH)
            message(FATAL_ERROR
                "UBSan does not instrument ${CAPSID_SOURCE}")
        endif()
    endif()

    if(CAPSID_EXPECT_TSAN)
        string(REGEX MATCH
            "-fsanitize=thread[^}]*${CAPSID_SOURCE_REGEX}"
            CAPSID_TSAN_MATCH
            "${CAPSID_COMMANDS}")
        if(NOT CAPSID_TSAN_MATCH)
            message(FATAL_ERROR
                "TSan does not instrument ${CAPSID_SOURCE}")
        endif()
    endif()
endforeach()
