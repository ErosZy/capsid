cmake_minimum_required(VERSION 3.18)

project(capsid_runtime VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)

option(CAPSID_BUILD_WORKER "Build the txiki.js worker process" ON)
option(CAPSID_STRICT_WARNINGS "Treat Capsid Runtime warnings as errors" ON)
option(CAPSID_USE_MIMALLOC "Use mimalloc for the txiki.js worker" OFF)
option(CAPSID_ENABLE_LTO "Enable interprocedural optimization for production targets" ON)
option(CAPSID_ENABLE_ASAN "Build all runtime targets with AddressSanitizer" OFF)
option(CAPSID_ENABLE_UBSAN "Build all runtime targets with UndefinedBehaviorSanitizer" OFF)
option(CAPSID_BUILD_FUZZERS "Build the libFuzzer protocol boundary targets" OFF)
option(CAPSID_BUILD_BENCHMARKS "Build the Capsid Runtime benchmark suite" OFF)
option(CAPSID_GENERATE_LINK_MAP
    "Generate a capsid-worker linker map for artifact audits" OFF)
option(CAPSID_BUILD_SQLITE_BENCHMARK
    "Build the benchmark-only read-only SQLite worker variant" OFF)
option(CAPSID_ENABLE_FFI_CAPABILITY
    "Enable the process-escape FFI capability (not implemented)" OFF)
option(CAPSID_ENABLE_RAW_SOCKET_CAPABILITY
    "Enable the process-escape raw-socket capability (not implemented)" OFF)
set(CAPSID_WPT_ROOT "" CACHE PATH
    "Optional checkout of the WPT repository at the revision locked by tests/wpt/manifest.json")
set(CAPSID_ESBUILD_EXECUTABLE "" CACHE FILEPATH
    "Optional host esbuild executable (defaults to vendor/txiki.js node_modules)")

if(CAPSID_ENABLE_FFI_CAPABILITY OR
   CAPSID_ENABLE_RAW_SOCKET_CAPABILITY OR
   (DEFINED BUILD_WITH_FFI AND BUILD_WITH_FFI))
    message(FATAL_ERROR
        "FFI/raw-socket escape capabilities are intentionally unavailable: "
        "they can bypass ordinary JavaScript policy and require a separate "
        "audited ABI, OS-sandbox profile and negative-test suite")
endif()
set(CAPSID_WORKER_LINK_MAP
    "${CMAKE_CURRENT_BINARY_DIR}/capsid-worker.map" CACHE FILEPATH
    "capsid-worker linker map path when CAPSID_GENERATE_LINK_MAP is enabled")

if(CAPSID_ENABLE_ASAN OR CAPSID_ENABLE_UBSAN OR CAPSID_BUILD_FUZZERS)
    if(CAPSID_USE_MIMALLOC)
        message(FATAL_ERROR
            "Sanitizer and fuzz builds require the system allocator; "
            "configure with CAPSID_USE_MIMALLOC=OFF")
    endif()
    if(CAPSID_ENABLE_LTO)
        message(STATUS "Disabling CAPSID_ENABLE_LTO for sanitizer/fuzz instrumentation")
        set(CAPSID_ENABLE_LTO OFF CACHE BOOL
            "Enable interprocedural optimization for production targets" FORCE)
    endif()
endif()
if(MSVC AND (CAPSID_ENABLE_UBSAN OR CAPSID_BUILD_FUZZERS))
    message(FATAL_ERROR "UBSan and libFuzzer builds are not supported with MSVC")
endif()
if(CAPSID_BUILD_FUZZERS AND NOT BUILD_TESTING)
    message(FATAL_ERROR "CAPSID_BUILD_FUZZERS requires BUILD_TESTING=ON")
endif()
if(CAPSID_BUILD_FUZZERS AND
   NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "CAPSID_BUILD_FUZZERS requires Clang or AppleClang")
endif()
if(CAPSID_BUILD_SQLITE_BENCHMARK AND NOT CAPSID_BUILD_BENCHMARKS)
    message(FATAL_ERROR
        "CAPSID_BUILD_SQLITE_BENCHMARK requires CAPSID_BUILD_BENCHMARKS=ON")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CAPSID_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CAPSID_CAPABILITY_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/capability-manifest.json")
set_property(
    DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CAPSID_CAPABILITY_MANIFEST}"
)
file(SHA256
    "${CAPSID_CAPABILITY_MANIFEST}"
    CAPSID_CAPABILITY_MANIFEST_SHA256
)
file(MAKE_DIRECTORY "${CAPSID_GENERATED_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/capability_manifest_hash.h.in"
    "${CAPSID_GENERATED_DIR}/capability_manifest_hash.h"
    @ONLY
)

add_library(capsid_sanitizers INTERFACE)
if(CAPSID_ENABLE_ASAN)
    if(MSVC)
        target_compile_options(capsid_sanitizers INTERFACE /fsanitize=address)
    else()
        target_compile_options(capsid_sanitizers INTERFACE
            -fsanitize=address
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
        target_link_options(capsid_sanitizers INTERFACE -fsanitize=address)
    endif()
endif()
if(CAPSID_ENABLE_UBSAN)
    target_compile_options(capsid_sanitizers INTERFACE
        -fsanitize=undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    target_link_options(capsid_sanitizers INTERFACE -fsanitize=undefined)
endif()

add_library(capsid_runtime STATIC
    src/audit.cc
    src/capability_policy.cc
    src/client.cc
    src/cpu_topology.cc
    src/egress_policy.cc
    src/memory_metrics.cc
    src/protocol.cc
    src/response_headers.cc
)
add_library(capsid::runtime ALIAS capsid_runtime)
set_target_properties(capsid_runtime PROPERTIES OUTPUT_NAME capsid_runtime)
target_link_libraries(capsid_runtime PUBLIC capsid_sanitizers)
if(CAPSID_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT CAPSID_IPO_SUPPORTED OUTPUT CAPSID_IPO_ERROR)
    if(CAPSID_IPO_SUPPORTED)
        set_property(TARGET capsid_runtime PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING
            "CAPSID_ENABLE_LTO requested but unsupported: ${CAPSID_IPO_ERROR}")
    endif()
endif()
target_include_directories(capsid_runtime
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CAPSID_GENERATED_DIR}"
)
target_compile_definitions(capsid_runtime PRIVATE CAPSID_WORKER_DEFAULT_PATH="capsid-worker")
if(CAPSID_ENABLE_ASAN)
    # ASan reserves a very large shadow address range. A production RLIMIT_AS
    # default prevents the instrumented worker from mapping that range, so
    # sanitizer builds retain the QuickJS heap limit but leave RLIMIT_AS off.
    target_compile_definitions(capsid_runtime PRIVATE CAPSID_ASAN_BUILD=1)
endif()

if(CAPSID_STRICT_WARNINGS)
    if(MSVC)
        target_compile_options(capsid_runtime PRIVATE /W4 /WX)
    else()
        target_compile_options(capsid_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()


# --- Sub-sections ---
include(cmake/build_worker.cmake)
include(cmake/build_tests.cmake)
include(cmake/build_benchmarks.cmake)
