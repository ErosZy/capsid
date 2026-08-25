cmake_minimum_required(VERSION 3.18)

project(capsid_runtime VERSION 0.2.2 LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "Capsid builds on Windows with MSVC only: the Ninja generator "
        "otherwise picks mingw gcc off PATH, which cannot build the "
        "CRT/winsock port. Pass -DCMAKE_C_COMPILER=cl "
        "-DCMAKE_CXX_COMPILER=cl alongside -G Ninja.")
endif()

if(MSVC)
    # Static CRT everywhere: the vendored txiki.js build pins
    # CMAKE_MSVC_RUNTIME_LIBRARY to MultiThreaded (its CMakeLists.txt line
    # 23), and vcpkg's x64-windows-static triplet links /MT libraries, so
    # mixing in the default /MD runtime would fail at link time with
    # LNK2038 mismatches. Capsid's Windows package is fully static for the
    # same self-containment reason as the musl Linux package.
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Every project and vendored source is UTF-8 (comments include
    # em-dashes etc.). MSVC otherwise assumes the ANSI codepage, and C4819
    # becomes a hard error under /WX on non-Western-locale machines.
    add_compile_options(/utf-8)
    # single_worker_server.cc is compiled into ~20 host/test targets; in
    # Debug, MSVC emits one COMDAT section per inline function and blows
    # past the default 2^16-section object limit (C1128). /bigobj lifts
    # the limit and is inert for ordinary TUs.
    add_compile_options(/bigobj)
    # The portability layer deliberately uses the POSIX-named CRT entry
    # points (open/read/write/close/...); without these defines every
    # such call warns C4996 and fails /WX. The secure-CRT deprecations
    # (fopen/strcpy families) are suppressed too: this codebase is a
    # POSIX port with explicit bounds checking and never relied on the
    # ISO-secure variants; warning-level correctness still gates through
    # /W4 /WX.
    add_compile_definitions(
        _CRT_NONSTDC_NO_DEPRECATE
        _CRT_SECURE_NO_DEPRECATE
    )
endif()

option(CAPSID_BUILD_WORKER "Build the txiki.js worker process" ON)
option(CAPSID_BUILD_HOST "Build the first-party Capsid Host" OFF)
option(CAPSID_STRICT_WARNINGS "Treat Capsid Runtime warnings as errors" ON)
option(CAPSID_USE_MIMALLOC
    "Use mimalloc only for the txiki.js worker heap (no global override)" ON)
option(CAPSID_ENABLE_LTO "Enable interprocedural optimization for production targets" ON)
if(WIN32 AND CAPSID_ENABLE_LTO)
    # MSVC /GL binds operator new/delete at compile time, so replaceable
    # global overrides — which the ABI guard tests use to inject
    # allocation failures — no longer intercept allocations in IPO'd
    # translation units. Disable IPO on Windows to keep that contract
    # testable (GCC/Clang preserve the replaceable symbols under LTO).
    set(CAPSID_ENABLE_LTO OFF CACHE BOOL
        "Enable interprocedural optimization for production targets" FORCE)
    message(STATUS "Disabling CAPSID_ENABLE_LTO on Windows (MSVC /GL binds "
        "replaceable operator new/delete)")
endif()
option(CAPSID_ENABLE_ASAN "Build all runtime targets with AddressSanitizer" OFF)
option(CAPSID_ENABLE_UBSAN "Build all runtime targets with UndefinedBehaviorSanitizer" OFF)
option(CAPSID_ENABLE_TSAN "Build all runtime targets with ThreadSanitizer" OFF)
option(CAPSID_BUILD_FUZZERS "Build the libFuzzer protocol boundary targets" OFF)
# A1: quickjs-ng opcode profiling counters (overlay patch 0036). The
# worker dumps one JSON object per runtime to stderr at teardown. This is
# a measurement build only — production performance is never measured with
# it, and zero-tax is enforced by the overlay OFF configuration.
option(CAPSID_ENABLE_OPCODE_PROFILE
    "Build the worker with quickjs-ng opcode profiling counters" OFF)
option(CAPSID_GENERATE_LINK_MAP
    "Generate a capsid-worker linker map for artifact audits" OFF)
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

if(CAPSID_ENABLE_ASAN OR CAPSID_ENABLE_UBSAN OR CAPSID_ENABLE_TSAN OR
   CAPSID_BUILD_FUZZERS)
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
if(CAPSID_ENABLE_TSAN AND (CAPSID_ENABLE_ASAN OR CAPSID_ENABLE_UBSAN))
    message(FATAL_ERROR
        "TSan cannot be combined with ASan or UBSan; use a dedicated build")
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
if(CAPSID_ENABLE_TSAN)
    if(MSVC)
        message(FATAL_ERROR "TSan requires Clang or GCC on Linux")
    endif()
    # GCC 15.x regressed: libstdc++'s std::atomic_thread_fence trips
    # -Werror=tsan in every translation unit that includes <atomic>.
    # Demote ONLY that warning class so the TSan instrumentation itself
    # stays on — this is a downgraded compile diagnostic, never a race
    # suppression. Clang does not need it (and is rejected below when its
    # TSan runtime is missing).
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(capsid_sanitizers INTERFACE -Wno-error=tsan)
    endif()
    target_compile_options(capsid_sanitizers INTERFACE
        -fsanitize=thread
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    target_link_options(capsid_sanitizers INTERFACE -fsanitize=thread)
    # Clang on Alpine/musl ships no TSan runtime (libclang_rt.tsan_cxx.a).
    # Fail at configure time with the remedy instead of at link time with
    # an incomprehensible missing-library error.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-runtime-dir
            OUTPUT_VARIABLE CAPSID_CLANG_RUNTIME_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE CAPSID_CLANG_RUNTIME_RESULT
        )
        # Distro layouts suffix the runtime with the target triple
        # (Ubuntu: libclang_rt.tsan_cxx-x86_64.a); the linker resolves the
        # suffixed form automatically, so accept either spelling.
        file(GLOB CAPSID_CLANG_TSAN_RUNTIMES
             "${CAPSID_CLANG_RUNTIME_DIR}/libclang_rt.tsan_cxx*.a")
        if(NOT CAPSID_CLANG_RUNTIME_RESULT EQUAL 0 OR
           NOT CAPSID_CLANG_TSAN_RUNTIMES)
            message(FATAL_ERROR
                "Clang TSan runtime is not installed (Alpine/musl ships no "
                "libclang_rt.tsan_cxx.a). Configure with GCC instead: "
                "-DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc")
        endif()
    endif()
endif()

add_library(capsid_runtime STATIC
    src/audit.cc
    src/build_info.cc
    src/capability_policy.cc
    src/client.cc
    src/cpu_topology.cc
    src/egress_policy.cc
    # Binding v1: the READY proof codec and the startup state machine are
    # shared by the Host ABI surface and its consumers.
    src/ipc_validation.cc
    src/memory_metrics.cc
    src/protocol.cc
    src/response_headers.cc
)
add_library(capsid::runtime ALIAS capsid_runtime)
set_target_properties(capsid_runtime PROPERTIES OUTPUT_NAME capsid_runtime)
target_link_libraries(capsid_runtime PUBLIC capsid_sanitizers)
if(WIN32)
    # egress_policy uses inet_pton/ntohs; every consumer of the runtime
    # (host, tests, embedders) must carry the Winsock import library.
    target_link_libraries(capsid_runtime PUBLIC ws2_32)
endif()
if(CAPSID_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT CAPSID_IPO_SUPPORTED OUTPUT CAPSID_IPO_ERROR)
    if(CAPSID_IPO_SUPPORTED)
        # LTO is a production-target optimization (the flag above is the
        # documented intent). Debug links non-LTO test objects against the
        # LTO archive members, and GCC 13 then drops always_inline
        # libstdc++ ctors from the LTO partitions ("undefined reference to
        # std::_Vector_base<...>::_Vector_impl_data()"); at -O0 LTO gains
        # nothing anyway, so Debug builds stay LTO-free.
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            set_property(TARGET capsid_runtime PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
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
if(CAPSID_ENABLE_TSAN)
    # TSan reserves shadow memory across a large virtual range as well; the
    # same RLIMIT_AS carve-out applies (see CAPSID_ASAN_BUILD).
    target_compile_definitions(capsid_runtime PRIVATE CAPSID_TSAN_BUILD=1)
endif()

if(CAPSID_STRICT_WARNINGS)
    if(MSVC)
        target_compile_options(capsid_runtime PRIVATE /W4 /WX)
    else()
        target_compile_options(capsid_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()


# --- Sub-sections ---
# OpenSSL is the digest/signature provider for the shipped binaries
# (capsid-bytecode-compile, capsid-host). It must be STATIC: the package
# smoke (§12.4 step 5) rejects undeclared dynamic dependencies, and a
# Homebrew/system shared libcrypto would leave the package dependent on a
# host-specific path at runtime. Set before every find_package(OpenSSL).
set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL
    "link OpenSSL statically into shipped binaries" FORCE)
include(cmake/build_worker.cmake)
include(cmake/build_host.cmake)
include(cmake/build_tests.cmake)

# --- Single generated build-identity source ---
# One canonical record shared by capsid_runtime, capsid-worker and
# capsid-bytecode-compile. Locked inputs come from
# docs/txiki-upgrade-baseline.json; with CAPSID_BUILD_WORKER=ON the locked
# values are validated against the prepared overlay and any mismatch fails
# the configure step.
include(cmake/ComputeBuildIdentity.cmake)
include(TestBigEndian)
test_big_endian(CAPSID_WORDS_BIG_ENDIAN)
if(CAPSID_WORDS_BIG_ENDIAN)
    set(CAPSID_BUILD_ENDIANNESS "big")
else()
    set(CAPSID_BUILD_ENDIANNESS "little")
endif()
# Compile flags that affect QuickJS bytecode compatibility. Kept canonical
# and stable for a given configuration; bytecode never crosses builds that
# differ here (see the Host design review §13.1). WP-07, spec §11.1: the
# value must be ONE canonical string — a multi-argument set() would form a
# CMake semicolon list and a one-value argument would silently truncate it.
set(CAPSID_BUILD_COMPILE_FLAGS
    "build_type=${CMAKE_BUILD_TYPE} lto=${CAPSID_ENABLE_LTO} asan=${CAPSID_ENABLE_ASAN} ubsan=${CAPSID_ENABLE_UBSAN} mimalloc=${CAPSID_USE_MIMALLOC}")
math(EXPR CAPSID_POINTER_WIDTH_BITS "${CMAKE_SIZEOF_VOID_P} * 8")
# Provenance target triple. A cross compiler reports its own triple; native
# builds compose one from the platform the CMake toolchain targets.
if(CMAKE_CXX_COMPILER_TARGET)
    set(CAPSID_BUILD_TARGET_TRIPLE "${CMAKE_CXX_COMPILER_TARGET}")
else()
    set(CAPSID_BUILD_TARGET_TRIPLE
        "${CMAKE_SYSTEM_PROCESSOR}-unknown-${CMAKE_SYSTEM_NAME}")
endif()
# Override point for the locked identity manifest (docs/txiki-upgrade-
# baseline.json). The build-identity matrix uses it to prove that a real
# bytecode-ABI difference changes the compatibility ID.
set(CAPSID_LOCKED_IDENTITY_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/txiki-upgrade-baseline.json"
    CACHE FILEPATH
    "Locked vendor/QuickJS manifest backing the bytecode-compatibility record")
# ABI version 7 matches CAPSID_ABI_VERSION in include/capsid/runtime.h;
# FetchRPC version 3 matches capsid::protocol::kVersion in src/protocol.h.
# v3 adds kFlagRequestEnd on RequestHead (bodyless requests).
capsid_generate_build_identity(
    OUT_HEADER "${CAPSID_GENERATED_DIR}/build_identity.h"
    LOCKED_MANIFEST "${CAPSID_LOCKED_IDENTITY_MANIFEST}"
    CAPSID_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_VERSION "${PROJECT_VERSION}"
    ABI_VERSION "7"
    FETCHRPC_VERSION "3"
    COMPILE_FLAGS "${CAPSID_BUILD_COMPILE_FLAGS}"
    ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}"
    ENDIANNESS "${CAPSID_BUILD_ENDIANNESS}"
    POINTER_WIDTH_BITS "${CAPSID_POINTER_WIDTH_BITS}"
    BYTECODE_FORMAT "quickjs-ng-bytecode-v1"
    CAPABILITY_MANIFEST_SHA256 "${CAPSID_CAPABILITY_MANIFEST_SHA256}"
    BUILD_TYPE "${CMAKE_BUILD_TYPE}"
    LTO "${CAPSID_ENABLE_LTO}"
    ASAN "${CAPSID_ENABLE_ASAN}"
    UBSAN "${CAPSID_ENABLE_UBSAN}"
    TSAN "${CAPSID_ENABLE_TSAN}"
    MIMALLOC "${CAPSID_USE_MIMALLOC}"
    HOST "${CAPSID_BUILD_HOST}"
    WORKER "${CAPSID_BUILD_WORKER}"
    COMPILER_ID "${CMAKE_CXX_COMPILER_ID}"
    COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}"
    TARGET_TRIPLE "${CAPSID_BUILD_TARGET_TRIPLE}")

# WP-08, spec §12: install rules, CPack packaging, build-info.txt/SBOM
# packaging. Included LAST so capsid owns the CPack configuration and the
# CPACK_PACKAGE_NAME tripwire (§12.1) runs after every third-party include.
include(cmake/CapsidPackage.cmake)
