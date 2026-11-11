# Reproducibility gate (remediation spec §12.3).
#
# Two fresh Release builds from the same inputs must agree on the content
# manifest and the identity records. The txiki/quickjs/lws toolchain is not
# yet bit-reproducible (embedded build paths and archive member metadata),
# so binary hashes are allowed to differ — but every difference is recorded
# in CAPSID_WORK_DIR/repro-differences.txt, and the deterministic artifacts
# (identity records, SBOM identity fields, headers, docs, license, CMake
# export files) must be byte-identical:
#
#   HARD   - package file NAME lists are identical (FILE-MANIFEST.txt)
#   HARD   - build-info identity records are identical (version/commit/
#            commitDate/buildId/compatibilityId/featureFlags/cmakeBuildType)
#   HARD   - the whole build-info.txt hashes identically (also covers the
#            bytecode-compatibility and build-provenance records)
#   HARD   - SBOM identity fields are identical (name/spdxVersion/
#            documentNamespace/creationInfo.created)
#   HARD   - deterministic text files hash-identically (include/,
#            share/doc/, share/licenses/, share/capsid/build-info.txt,
#            lib/cmake/ — everything except FILE-MANIFEST.txt and
#            SBOM.spdx.json, which embed per-build binary hashes by design)
#   RECORD - every bin/ and lib/ hash/size difference with the file list,
#            written to CAPSID_WORK_DIR/repro-differences.txt
#
# Usage (script mode):
#   cmake -DCAPSID_BUILD_DIR=<baseline build> -DCAPSID_SOURCE_DIR=<repo>
#        -DCAPSID_WORK_DIR=<fresh dir> -DCAPSID_CMAKE_COMMAND=<cmake>
#        -DCAPSID_STRICT_WARNINGS=ON|OFF
#        -DCAPSID_ENABLE_FFI_CAPABILITY=ON|OFF
#        -DCAPSID_ENABLE_RAW_SOCKET_CAPABILITY=ON|OFF
#        [-DCAPSID_REPRO_GENERATOR=<generator, mirrors the baseline build>]
#        [-DCAPSID_REPRO_PARALLEL=<build parallelism, default 2>]
#        [-DCAPSID_REPRO_PREFIX_PATH=<CMAKE_PREFIX_PATH, e.g. a pinned
#          OpenSSL install that the baseline build was configured with>]
#        [-DCAPSID_REPRO_OPENSSL_ROOT_DIR=<OPENSSL_ROOT_DIR, e.g. a pinned
#          OpenSSL install found via OPENSSL_ROOT_DIR instead>]
#        [-DCAPSID_REPRO_BOOST_DIR=<effective Boost package directory>]
#        [-DCAPSID_REPRO_BOOST_INCLUDE_DIR=<effective Boost include root>]
#        [-DCAPSID_REPRO_BOOST_SYSTEM_LIBRARY=<effective Boost.System library>]
#        -P tests/test_reproducibility.cmake
#
# The feature flags (worker/host/lto/asan/ubsan/tsan/mimalloc/sandbox_profile) and the build
# type are parsed from the BASELINE build's build-info.txt so the second
# build mirrors the first exactly. SOURCE_DATE_EPOCH is pinned to the capsid
# commit date so CPack archive member timestamps are stable.

foreach(CAPSID_REPRO_REQUIRED
        CAPSID_BUILD_DIR CAPSID_SOURCE_DIR CAPSID_WORK_DIR
        CAPSID_CMAKE_COMMAND CAPSID_STRICT_WARNINGS
        CAPSID_ENABLE_FFI_CAPABILITY CAPSID_ENABLE_RAW_SOCKET_CAPABILITY)
    if(NOT DEFINED ${CAPSID_REPRO_REQUIRED})
        message(FATAL_ERROR "test_reproducibility requires "
            "${CAPSID_REPRO_REQUIRED}")
    endif()
endforeach()

file(REMOVE_RECURSE "${CAPSID_WORK_DIR}")
file(MAKE_DIRECTORY "${CAPSID_WORK_DIR}")
set(CAPSID_REPRO_DIFFS "${CAPSID_WORK_DIR}/repro-differences.txt")
file(WRITE "${CAPSID_REPRO_DIFFS}" "capsid reproducibility differences "
    "(allowed: toolchain not bit-reproducible; identity must match)\n")

# ---- baseline identity from the baseline build tree --------------------------
set(CAPSID_REPRO_BASELINE_INFO
    "${CAPSID_BUILD_DIR}/generated/build-info.txt")
if(NOT EXISTS "${CAPSID_REPRO_BASELINE_INFO}")
    message(FATAL_ERROR "baseline build-info.txt not found: "
        "${CAPSID_REPRO_BASELINE_INFO}")
endif()
file(STRINGS "${CAPSID_REPRO_BASELINE_INFO}" CAPSID_REPRO_BASELINE_INFO_LINES)
set(CAPSID_REPRO_BASELINE_FEATURES)
set(CAPSID_REPRO_BASELINE_BUILD_TYPE)
foreach(line IN LISTS CAPSID_REPRO_BASELINE_INFO_LINES)
    if(line MATCHES "^featureFlags=(.+)$")
        set(CAPSID_REPRO_BASELINE_FEATURES "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^cmakeBuildType=(.*)$")
        set(CAPSID_REPRO_BASELINE_BUILD_TYPE "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT CAPSID_REPRO_BASELINE_FEATURES)
    message(FATAL_ERROR "baseline build-info.txt has no featureFlags")
endif()

# featureFlags=lto=OFF asan=OFF ... -> -DCAPSID_ENABLE_LTO=OFF ...
# The value is a space-joined string, not a CMake list: split it first.
string(REPLACE " " ";" CAPSID_REPRO_FEATURE_PAIRS
    "${CAPSID_REPRO_BASELINE_FEATURES}")
set(CAPSID_REPRO_FLAG_ARGS)
foreach(pair IN LISTS CAPSID_REPRO_FEATURE_PAIRS)
    string(REPLACE "=" ";" pair_parts "${pair}")
    list(LENGTH pair_parts pair_len)
    if(NOT pair_len EQUAL 2)
        message(FATAL_ERROR "malformed featureFlags pair: ${pair}")
    endif()
    list(GET pair_parts 0 feature_name)
    list(GET pair_parts 1 feature_value)
    if(feature_name STREQUAL "worker")
        set(flag_name "CAPSID_BUILD_WORKER")
    elseif(feature_name STREQUAL "host")
        set(flag_name "CAPSID_BUILD_HOST")
    elseif(feature_name STREQUAL "lto")
        set(flag_name "CAPSID_ENABLE_LTO")
    elseif(feature_name STREQUAL "asan")
        set(flag_name "CAPSID_ENABLE_ASAN")
    elseif(feature_name STREQUAL "ubsan")
        set(flag_name "CAPSID_ENABLE_UBSAN")
    elseif(feature_name STREQUAL "tsan")
        set(flag_name "CAPSID_ENABLE_TSAN")
    elseif(feature_name STREQUAL "mimalloc")
        set(flag_name "CAPSID_USE_MIMALLOC")
    elseif(feature_name STREQUAL "sandbox_profile")
        if(NOT feature_value STREQUAL "production" AND
           NOT feature_value STREQUAL "sanitizer")
            message(FATAL_ERROR "invalid sandbox_profile feature flag: ${feature_value}")
        endif()
        set(flag_name "CAPSID_SANDBOX_PROFILE")
    else()
        message(FATAL_ERROR "unknown featureFlags entry: ${feature_name}")
    endif()
    list(APPEND CAPSID_REPRO_FLAG_ARGS
        "-D${flag_name}=${feature_value}")
endforeach()
list(APPEND CAPSID_REPRO_FLAG_ARGS
    "-DCAPSID_STRICT_WARNINGS=${CAPSID_STRICT_WARNINGS}"
    "-DCAPSID_ENABLE_FFI_CAPABILITY=${CAPSID_ENABLE_FFI_CAPABILITY}"
    "-DCAPSID_ENABLE_RAW_SOCKET_CAPABILITY=${CAPSID_ENABLE_RAW_SOCKET_CAPABILITY}"
)
if(CAPSID_REPRO_BASELINE_BUILD_TYPE)
    list(APPEND CAPSID_REPRO_FLAG_ARGS
        "-DCMAKE_BUILD_TYPE=${CAPSID_REPRO_BASELINE_BUILD_TYPE}")
endif()

# SOURCE_DATE_EPOCH = the capsid commit date (epoch), so the CPack archive
# member timestamps are stable and the archive is byte-comparable.
execute_process(
    COMMAND git -C "${CAPSID_SOURCE_DIR}" log -1 --format=%ct
    RESULT_VARIABLE epoch_result
    OUTPUT_VARIABLE epoch_out
    ERROR_VARIABLE epoch_err)
if(NOT epoch_result EQUAL 0)
    message(FATAL_ERROR "git log failed for SOURCE_DATE_EPOCH:\n"
        "${epoch_err}")
endif()
string(STRIP "${epoch_out}" CAPSID_REPRO_EPOCH)
if(NOT CAPSID_REPRO_EPOCH MATCHES "^[0-9]+$")
    message(FATAL_ERROR "invalid commit epoch: ${CAPSID_REPRO_EPOCH}")
endif()

# ---- second fresh configure + build + package --------------------------------
# The generator, parallelism and any toolchain prefix path mirror the
# baseline so the second build is not slower than necessary and finds the
# same dependencies (CI: the pinned OpenSSL 3.5 install).
if(NOT DEFINED CAPSID_REPRO_PARALLEL OR CAPSID_REPRO_PARALLEL STREQUAL "")
    set(CAPSID_REPRO_PARALLEL 2)
endif()
set(CAPSID_REPRO_BUILD_DIR "${CAPSID_WORK_DIR}/build")
set(CAPSID_REPRO_CONFIGURE_ARGS)
list(APPEND CAPSID_REPRO_CONFIGURE_ARGS "-DBUILD_TESTING=OFF")
if(DEFINED CAPSID_C_COMPILER AND
   NOT CAPSID_C_COMPILER STREQUAL "")
    list(APPEND CAPSID_REPRO_CONFIGURE_ARGS
        "-DCMAKE_C_COMPILER=${CAPSID_C_COMPILER}")
endif()
if(DEFINED CAPSID_CXX_COMPILER AND
   NOT CAPSID_CXX_COMPILER STREQUAL "")
    list(APPEND CAPSID_REPRO_CONFIGURE_ARGS
        "-DCMAKE_CXX_COMPILER=${CAPSID_CXX_COMPILER}")
endif()
if(DEFINED CAPSID_REPRO_GENERATOR AND
   NOT CAPSID_REPRO_GENERATOR STREQUAL "")
    list(APPEND CAPSID_REPRO_CONFIGURE_ARGS "-G" "${CAPSID_REPRO_GENERATOR}")
endif()
if(DEFINED CAPSID_REPRO_PREFIX_PATH AND
   NOT CAPSID_REPRO_PREFIX_PATH STREQUAL "")
    # execute_process expands a CMake list. Keep all prefix entries in the
    # single -D argument instead of turning later entries into extra paths.
    string(REPLACE ";" "\\;" CAPSID_REPRO_ESCAPED_PREFIX_PATH "${CAPSID_REPRO_PREFIX_PATH}")
    list(APPEND CAPSID_REPRO_CONFIGURE_ARGS
        "-DCMAKE_PREFIX_PATH=${CAPSID_REPRO_ESCAPED_PREFIX_PATH}")
endif()
if(DEFINED CAPSID_REPRO_ESBUILD_EXECUTABLE AND
   NOT CAPSID_REPRO_ESBUILD_EXECUTABLE STREQUAL "")
    list(APPEND CAPSID_REPRO_CONFIGURE_ARGS
        "-DCAPSID_ESBUILD_EXECUTABLE=${CAPSID_REPRO_ESBUILD_EXECUTABLE}")
endif()
if(DEFINED CAPSID_REPRO_OPENSSL_ROOT_DIR AND
   NOT CAPSID_REPRO_OPENSSL_ROOT_DIR STREQUAL "")
    list(APPEND CAPSID_REPRO_CONFIGURE_ARGS
        "-DOPENSSL_ROOT_DIR=${CAPSID_REPRO_OPENSSL_ROOT_DIR}")
endif()
if(DEFINED CAPSID_REPRO_BOOST_DIR AND
   NOT CAPSID_REPRO_BOOST_DIR STREQUAL "")
    list(APPEND CAPSID_REPRO_CONFIGURE_ARGS
        "-DBoost_DIR=${CAPSID_REPRO_BOOST_DIR}")
endif()
if(DEFINED CAPSID_REPRO_BOOST_INCLUDE_DIR AND
   NOT CAPSID_REPRO_BOOST_INCLUDE_DIR STREQUAL "")
    list(APPEND CAPSID_REPRO_CONFIGURE_ARGS
        "-DBoost_INCLUDE_DIR=${CAPSID_REPRO_BOOST_INCLUDE_DIR}")
endif()
if(DEFINED CAPSID_REPRO_BOOST_SYSTEM_LIBRARY AND
   NOT CAPSID_REPRO_BOOST_SYSTEM_LIBRARY STREQUAL "")
    list(APPEND CAPSID_REPRO_CONFIGURE_ARGS
        "-DBoost_SYSTEM_LIBRARY_RELEASE=${CAPSID_REPRO_BOOST_SYSTEM_LIBRARY}")
endif()
execute_process(
    COMMAND "${CAPSID_CMAKE_COMMAND}" -E env
        "SOURCE_DATE_EPOCH=${CAPSID_REPRO_EPOCH}"
        "${CAPSID_CMAKE_COMMAND}"
        -S "${CAPSID_SOURCE_DIR}"
        -B "${CAPSID_REPRO_BUILD_DIR}"
        ${CAPSID_REPRO_CONFIGURE_ARGS}
        ${CAPSID_REPRO_FLAG_ARGS}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_out
    ERROR_VARIABLE configure_err)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "reproducibility configure failed:\n"
        "${configure_out}\n${configure_err}")
endif()
if(DEFINED CAPSID_REPRO_PREFIX_PATH AND NOT CAPSID_REPRO_PREFIX_PATH STREQUAL "")
    load_cache("${CAPSID_REPRO_BUILD_DIR}" READ_WITH_PREFIX CAPSID_REPRO_EFFECTIVE_ CMAKE_PREFIX_PATH)
    if(NOT "${CAPSID_REPRO_EFFECTIVE_CMAKE_PREFIX_PATH}" STREQUAL "${CAPSID_REPRO_PREFIX_PATH}")
        message(FATAL_ERROR "reproducibility CMAKE_PREFIX_PATH did not preserve all baseline hints: "
            "expected [${CAPSID_REPRO_PREFIX_PATH}], got [${CAPSID_REPRO_EFFECTIVE_CMAKE_PREFIX_PATH}]")
    endif()
endif()

execute_process(
    COMMAND "${CAPSID_CMAKE_COMMAND}"
        --build "${CAPSID_REPRO_BUILD_DIR}"
        --target package
        --parallel "${CAPSID_REPRO_PARALLEL}"
    RESULT_VARIABLE package_result
    OUTPUT_VARIABLE package_out
    ERROR_VARIABLE package_err)
if(NOT package_result EQUAL 0)
    message(FATAL_ERROR "reproducibility package build failed:\n"
        "${package_out}\n${package_err}")
endif()

# ---- locate and extract both archives ----------------------------------------
function(capsid_repro_newest_archive build_dir out_var)
    set(newest_path)
    set(newest_mtime -1)
    # The Windows CPack generator produces a ZIP archive; POSIX builds a
    # tar.gz. Accept whichever the platform produced.
    file(GLOB archives
        "${build_dir}/capsid-*.tar.gz"
        "${build_dir}/capsid-*.zip")
    foreach(archive IN LISTS archives)
        file(TIMESTAMP "${archive}" archive_mtime "%s" UTC)
        if(archive_mtime GREATER newest_mtime)
            set(newest_mtime "${archive_mtime}")
            set(newest_path "${archive}")
        endif()
    endforeach()
    set(${out_var} "${newest_path}" PARENT_SCOPE)
endfunction()

capsid_repro_newest_archive("${CAPSID_BUILD_DIR}" CAPSID_REPRO_BASELINE_ARCHIVE)
if(NOT CAPSID_REPRO_BASELINE_ARCHIVE)
    message(FATAL_ERROR "baseline build produced no capsid archive")
endif()
capsid_repro_newest_archive("${CAPSID_REPRO_BUILD_DIR}"
    CAPSID_REPRO_ARCHIVE)
if(NOT CAPSID_REPRO_ARCHIVE)
    message(FATAL_ERROR "reproducibility build produced no capsid archive")
endif()

# Extract both archives; each root is the archive's single top-level dir
# (probed, not assumed).
function(capsid_repro_extract_root archive_path extract_dir out_var)
    file(ARCHIVE_EXTRACT INPUT "${archive_path}" DESTINATION "${extract_dir}")
    set(root "${extract_dir}")
    file(GLOB entries "${extract_dir}/*")
    list(LENGTH entries entry_count)
    if(entry_count EQUAL 1)
        list(GET entries 0 single_entry)
        if(IS_DIRECTORY "${single_entry}")
            set(root "${single_entry}")
        endif()
    endif()
    set(${out_var} "${root}" PARENT_SCOPE)
endfunction()

capsid_repro_extract_root("${CAPSID_REPRO_ARCHIVE}"
    "${CAPSID_WORK_DIR}/extracted-repro" CAPSID_REPRO_ROOT)
capsid_repro_extract_root("${CAPSID_REPRO_BASELINE_ARCHIVE}"
    "${CAPSID_WORK_DIR}/extracted-baseline" CAPSID_REPRO_BASELINE_ROOT)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ComparePackageReproducibility.cmake")
capsid_compare_package_trees("${CAPSID_REPRO_BASELINE_ROOT}"
    "${CAPSID_REPRO_ROOT}" "${CAPSID_REPRO_DIFFS}")

# Matching document names alone do not establish valid metadata. Both builds
# must independently describe their own actual files and locked dependencies.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ValidatePackageMetadata.cmake")
get_filename_component(CAPSID_REPRO_BASENAME "${CAPSID_REPRO_BASELINE_ARCHIVE}" NAME)
string(REGEX REPLACE "\\.(tar\\.gz|zip)$" "" CAPSID_REPRO_BASENAME "${CAPSID_REPRO_BASENAME}")
capsid_validate_package_metadata("${CAPSID_REPRO_BASELINE_ROOT}"
    "${CAPSID_SOURCE_DIR}" "${CAPSID_REPRO_BASENAME}")
capsid_validate_package_metadata("${CAPSID_REPRO_ROOT}"
    "${CAPSID_SOURCE_DIR}" "${CAPSID_REPRO_BASENAME}")
