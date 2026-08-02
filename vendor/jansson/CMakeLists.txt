# Vendored Jansson 2.15.1 — locked static library for the Capsid Host.
#
# See VENDOR.txt for provenance and the exact locked archive. This directory
# is included only by cmake/build_host.cmake (CAPSID_BUILD_HOST=ON); it is
# not installed and never part of the public ABI.
#
# The two generated configuration headers are produced from the vendored
# upstream CMake templates with the values that upstream would detect on the
# supported platforms (Linux/macOS, Clang/GCC). No configure-time download.

set(JANSSON_VERSION "2.15.1")

# Platform detection mirrors the upstream CMake build (CMakeLists.txt
# lines 78-95, 224-225): the same checks, in the same order. Nothing is
# hardcoded per platform; e.g. macOS has no endian.h, so lookup3.h uses its
# __BYTE_ORDER fallback instead.
include(CheckIncludeFiles)
include(CheckFunctionExists)
check_include_files(endian.h HAVE_ENDIAN_H)
check_include_files(fcntl.h HAVE_FCNTL_H)
check_include_files(sched.h HAVE_SCHED_H)
check_include_files(unistd.h HAVE_UNISTD_H)
check_include_files(sys/param.h HAVE_SYS_PARAM_H)
check_include_files(sys/stat.h HAVE_SYS_STAT_H)
check_include_files(sys/time.h HAVE_SYS_TIME_H)
check_include_files(sys/types.h HAVE_SYS_TYPES_H)
check_function_exists(close HAVE_CLOSE)
check_function_exists(getpid HAVE_GETPID)
check_function_exists(gettimeofday HAVE_GETTIMEOFDAY)
check_function_exists(open HAVE_OPEN)
check_function_exists(read HAVE_READ)
check_function_exists(sched_yield HAVE_SCHED_YIELD)
check_include_files(stdint.h HAVE_STDINT_H)
check_include_files(inttypes.h HAVE_INTTYPES_H)
check_include_files(locale.h HAVE_LOCALE_H)
check_function_exists(setlocale HAVE_SETLOCALE)

# Public jansson_config.h.
set(JSON_INT_T "long long")
set(JSON_STRTOINT "strtoll")
set(JSON_INTEGER_FORMAT "\"lld\"")
set(JSON_INLINE "inline")
# Clang and GCC support both builtin families on all supported platforms;
# the checks upstream performs would also succeed here.
set(JSON_HAVE_ATOMIC_BUILTINS 1)
set(JSON_HAVE_SYNC_BUILTINS 1)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/jansson_config.h.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/include/jansson_config.h"
    @ONLY)

# Private jansson_private_config.h. The int32_t/uintN_t/ssize_t fallback
# values are never used because the HAVE_*_T definitions are enabled, but
# configure_file needs them to exist.
set(HAVE_SYNC_BUILTINS 1)
set(HAVE_ATOMIC_BUILTINS 1)
set(HAVE_INT32_T 1)
set(HAVE_UINT32_T 1)
set(HAVE_UINT16_T 1)
set(HAVE_UINT8_T 1)
set(HAVE_SSIZE_T 1)
set(JSON_INT32 "int32_t")
set(JSON_UINT32 "uint32_t")
set(JSON_UINT16 "uint16_t")
set(JSON_UINT8 "uint8_t")
set(JSON_SSIZE "ssize_t")
set(USE_URANDOM 1)
set(JANSSON_INITIAL_HASHTABLE_ORDER 3)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/jansson_private_config.h.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/private_include/jansson_private_config.h"
    @ONLY)

add_library(capsid_jansson STATIC
    src/dump.c
    src/error.c
    src/hashtable.c
    src/hashtable_seed.c
    src/load.c
    src/memory.c
    src/pack_unpack.c
    src/strbuffer.c
    src/strconv.c
    src/utf.c
    src/value.c
    src/version.c
)
set_target_properties(capsid_jansson PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(capsid_jansson
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/private_include"
)
target_compile_definitions(capsid_jansson
    PRIVATE
        HAVE_CONFIG_H
        JANSSON_VERSION="2.15.1"
)
