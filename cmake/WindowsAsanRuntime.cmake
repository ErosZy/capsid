# MSVC /MT still imports the ASan DLL in current toolchains. An instrumented
# archive must carry that exact dependency; normal packages remain static.
set(CAPSID_ASAN_RUNTIME_NAME "")
set(CAPSID_ASAN_RUNTIME_SHA256 "")
if(MSVC AND CAPSID_ENABLE_ASAN)
    set(CAPSID_MSVC_ASAN_RUNTIME "" CACHE FILEPATH
        "ASan runtime from the selected MSVC toolchain (auto-detected beside cl)")
    if(NOT CAPSID_MSVC_ASAN_RUNTIME)
        get_filename_component(CAPSID_ASAN_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        file(GLOB CAPSID_ASAN_RUNTIME_CANDIDATES
            "${CAPSID_ASAN_COMPILER_DIR}/clang_rt.asan_dynamic-*.dll")
        list(LENGTH CAPSID_ASAN_RUNTIME_CANDIDATES CAPSID_ASAN_RUNTIME_COUNT)
        if(NOT CAPSID_ASAN_RUNTIME_COUNT EQUAL 1)
            message(FATAL_ERROR "Cannot select one ASan DLL beside ${CMAKE_CXX_COMPILER}; "
                "set CAPSID_MSVC_ASAN_RUNTIME to the matching toolchain runtime")
        endif()
        list(GET CAPSID_ASAN_RUNTIME_CANDIDATES 0 CAPSID_MSVC_ASAN_RUNTIME)
    endif()
    if(NOT EXISTS "${CAPSID_MSVC_ASAN_RUNTIME}" OR IS_DIRECTORY "${CAPSID_MSVC_ASAN_RUNTIME}")
        message(FATAL_ERROR "CAPSID_MSVC_ASAN_RUNTIME must name an existing DLL")
    endif()
    get_filename_component(CAPSID_ASAN_RUNTIME_NAME "${CAPSID_MSVC_ASAN_RUNTIME}" NAME)
    if(NOT CAPSID_ASAN_RUNTIME_NAME MATCHES "^clang_rt[.]asan_dynamic-(x86_64|i386|aarch64)[.]dll$")
        message(FATAL_ERROR "Unexpected MSVC ASan runtime name: ${CAPSID_ASAN_RUNTIME_NAME}")
    endif()
    file(SHA256 "${CAPSID_MSVC_ASAN_RUNTIME}" CAPSID_ASAN_RUNTIME_SHA256)
    get_filename_component(CAPSID_ASAN_RUNTIME_DIR "${CAPSID_MSVC_ASAN_RUNTIME}" DIRECTORY)
    foreach(CAPSID_CRT_RUNTIME IN ITEMS vcruntime140 vcruntime140_1)
        if(NOT EXISTS "${CAPSID_ASAN_RUNTIME_DIR}/${CAPSID_CRT_RUNTIME}.dll")
            message(FATAL_ERROR "Missing ${CAPSID_CRT_RUNTIME}.dll beside the selected ASan runtime")
        endif()
        file(SHA256 "${CAPSID_ASAN_RUNTIME_DIR}/${CAPSID_CRT_RUNTIME}.dll"
            CAPSID_${CAPSID_CRT_RUNTIME}_SHA256)
        install(FILES "${CAPSID_ASAN_RUNTIME_DIR}/${CAPSID_CRT_RUNTIME}.dll" DESTINATION bin)
    endforeach()
    file(WRITE "${CAPSID_GENERATED_DIR}/asan-runtime.json"
        "{\n  \"schema\": \"capsid-msvc-asan-runtime-v1\",\n"
        "  \"file\": \"${CAPSID_ASAN_RUNTIME_NAME}\",\n"
        "  \"sha256\": \"${CAPSID_ASAN_RUNTIME_SHA256}\",\n"
        "  \"compilerId\": \"${CMAKE_CXX_COMPILER_ID}\",\n"
        "  \"compilerVersion\": \"${CMAKE_CXX_COMPILER_VERSION}\",\n"
        "  \"crtFiles\": [\n"
        "    {\"file\": \"vcruntime140.dll\", \"sha256\": \"${CAPSID_vcruntime140_SHA256}\"},\n"
        "    {\"file\": \"vcruntime140_1.dll\", \"sha256\": \"${CAPSID_vcruntime140_1_SHA256}\"}\n"
        "  ]\n}\n")
    install(FILES "${CAPSID_MSVC_ASAN_RUNTIME}" DESTINATION bin)
    install(FILES "${CAPSID_GENERATED_DIR}/asan-runtime.json" DESTINATION share/capsid)
endif()
