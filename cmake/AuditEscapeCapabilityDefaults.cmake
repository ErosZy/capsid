if(NOT DEFINED CAPSID_CACHE OR NOT EXISTS "${CAPSID_CACHE}")
    message(FATAL_ERROR "CAPSID_CACHE must name an existing CMakeCache.txt")
endif()

file(READ "${CAPSID_CACHE}" CAPSID_CACHE_CONTENT)
foreach(CAPSID_OPTION
        CAPSID_ENABLE_FFI_CAPABILITY
        CAPSID_ENABLE_RAW_SOCKET_CAPABILITY)
    if(NOT CAPSID_CACHE_CONTENT MATCHES
            "(^|\n)${CAPSID_OPTION}:BOOL=OFF(\n|$)")
        message(FATAL_ERROR
            "${CAPSID_OPTION} is absent or not fail-closed OFF")
    endif()
endforeach()

if(CAPSID_CACHE_CONTENT MATCHES
        "(^|\n)BUILD_WITH_FFI:BOOL=ON(\n|$)")
    message(FATAL_ERROR
        "txiki FFI was enabled behind the Capsid capability gate")
endif()

message(STATUS "escape capability defaults are fail-closed")
