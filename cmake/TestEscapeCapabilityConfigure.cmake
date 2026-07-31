if(NOT DEFINED CAPSID_SOURCE_DIR OR
   NOT DEFINED CAPSID_BINARY_ROOT OR
   NOT DEFINED CAPSID_GENERATOR)
    message(FATAL_ERROR
        "CAPSID_SOURCE_DIR, CAPSID_BINARY_ROOT and CAPSID_GENERATOR are required")
endif()

file(REMOVE_RECURSE "${CAPSID_BINARY_ROOT}")
file(MAKE_DIRECTORY "${CAPSID_BINARY_ROOT}")

foreach(CAPSID_OPTION
        CAPSID_ENABLE_FFI_CAPABILITY
        CAPSID_ENABLE_RAW_SOCKET_CAPABILITY)
    set(CAPSID_BINARY_DIR
        "${CAPSID_BINARY_ROOT}/${CAPSID_OPTION}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${CAPSID_SOURCE_DIR}"
            -B "${CAPSID_BINARY_DIR}"
            -G "${CAPSID_GENERATOR}"
            -DBUILD_TESTING=OFF
            -DCAPSID_BUILD_WORKER=OFF
            "-D${CAPSID_OPTION}=ON"
        RESULT_VARIABLE CAPSID_RESULT
        OUTPUT_VARIABLE CAPSID_STDOUT
        ERROR_VARIABLE CAPSID_STDERR
    )
    if(CAPSID_RESULT EQUAL 0)
        message(FATAL_ERROR
            "${CAPSID_OPTION}=ON configured successfully without an "
            "audited escape-capability implementation")
    endif()
    string(CONCAT CAPSID_OUTPUT
        "${CAPSID_STDOUT}\n${CAPSID_STDERR}")
    if(NOT CAPSID_OUTPUT MATCHES
            "escape capabilities are intentionally unavailable")
        message(FATAL_ERROR
            "${CAPSID_OPTION}=ON failed for an unrelated reason:\n"
            "${CAPSID_OUTPUT}")
    endif()
endforeach()

message(STATUS "escape capability configure negative controls passed")
