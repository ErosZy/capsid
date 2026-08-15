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
#        -P tests/test_reproducibility.cmake
#
# The feature flags (worker/host/lto/asan/ubsan/tsan/mimalloc) and the build
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
if(DEFINED CAPSID_REPRO_GENERATOR AND
   NOT CAPSID_REPRO_GENERATOR STREQUAL "")
    list(APPEND CAPSID_REPRO_CONFIGURE_ARGS "-G" "${CAPSID_REPRO_GENERATOR}")
endif()
if(DEFINED CAPSID_REPRO_PREFIX_PATH AND
   NOT CAPSID_REPRO_PREFIX_PATH STREQUAL "")
    list(APPEND CAPSID_REPRO_CONFIGURE_ARGS
        "-DCMAKE_PREFIX_PATH=${CAPSID_REPRO_PREFIX_PATH}")
endif()
if(DEFINED CAPSID_REPRO_OPENSSL_ROOT_DIR AND
   NOT CAPSID_REPRO_OPENSSL_ROOT_DIR STREQUAL "")
    list(APPEND CAPSID_REPRO_CONFIGURE_ARGS
        "-DOPENSSL_ROOT_DIR=${CAPSID_REPRO_OPENSSL_ROOT_DIR}")
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

# ---- compare file NAME lists -------------------------------------------------
# FILE-MANIFEST.txt lists every package file except itself and the SBOM
# (both embed per-build binary hashes by design).
file(STRINGS "${CAPSID_REPRO_ROOT}/share/capsid/FILE-MANIFEST.txt"
    CAPSID_REPRO_MANIFEST_LINES)
file(STRINGS "${CAPSID_REPRO_BASELINE_ROOT}/share/capsid/FILE-MANIFEST.txt"
    CAPSID_REPRO_BASELINE_MANIFEST_LINES)
set(CAPSID_REPRO_NAMES)
foreach(manifest_line IN LISTS CAPSID_REPRO_MANIFEST_LINES)
    string(REPLACE "\t" ";" manifest_parts "${manifest_line}")
    list(GET manifest_parts 0 manifest_name)
    list(APPEND CAPSID_REPRO_NAMES "${manifest_name}")
endforeach()
set(CAPSID_REPRO_BASELINE_NAMES)
foreach(manifest_line IN LISTS CAPSID_REPRO_BASELINE_MANIFEST_LINES)
    string(REPLACE "\t" ";" manifest_parts "${manifest_line}")
    list(GET manifest_parts 0 manifest_name)
    list(APPEND CAPSID_REPRO_BASELINE_NAMES "${manifest_name}")
endforeach()
list(LENGTH CAPSID_REPRO_NAMES repro_name_count)
list(LENGTH CAPSID_REPRO_BASELINE_NAMES baseline_name_count)
if(NOT repro_name_count EQUAL baseline_name_count)
    message(FATAL_ERROR "package file counts differ: baseline "
        "${baseline_name_count} vs repro ${repro_name_count}")
endif()
list(SORT CAPSID_REPRO_NAMES)
list(SORT CAPSID_REPRO_BASELINE_NAMES)
if(NOT CAPSID_REPRO_NAMES STREQUAL CAPSID_REPRO_BASELINE_NAMES)
    message(FATAL_ERROR "package file name lists differ:\nbaseline: "
        "${CAPSID_REPRO_BASELINE_NAMES}\nrepro: ${CAPSID_REPRO_NAMES}")
endif()

# ---- compare identity records ------------------------------------------------
set(CAPSID_REPRO_RECORDS
    version commit commitDate buildId compatibilityId featureFlags
    cmakeBuildType)
foreach(record IN LISTS CAPSID_REPRO_RECORDS)
    set(baseline_value)
    set(repro_value)
    # cmakeBuildType may legitimately be empty; the other records never are.
    if(record STREQUAL "cmakeBuildType")
        set(record_regex "^${record}=(.*)$")
    else()
        set(record_regex "^${record}=(.+)$")
    endif()
    file(STRINGS "${CAPSID_REPRO_BASELINE_ROOT}/share/capsid/build-info.txt"
        CAPSID_REPRO_BASELINE_INFO_LINES)
    foreach(line IN LISTS CAPSID_REPRO_BASELINE_INFO_LINES)
        if(line MATCHES "${record_regex}")
            set(baseline_value "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    file(STRINGS "${CAPSID_REPRO_ROOT}/share/capsid/build-info.txt"
        CAPSID_REPRO_INFO_LINES)
    foreach(line IN LISTS CAPSID_REPRO_INFO_LINES)
        if(line MATCHES "${record_regex}")
            set(repro_value "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    # Quoted comparison: an unquoted empty value would collapse the STREQUAL
    # expression into a truthy variable-name check.
    if(NOT "${baseline_value}" STREQUAL "${repro_value}")
        message(FATAL_ERROR "identity record ${record} differs: baseline "
            "[${baseline_value}] vs repro [${repro_value}]")
    endif()
endforeach()

# The whole file must also be byte-identical: this subsumes the field checks
# and covers the bytecode-compatibility and build-provenance records too.
file(SHA256 "${CAPSID_REPRO_BASELINE_ROOT}/share/capsid/build-info.txt"
    CAPSID_REPRO_BASELINE_INFO_SHA)
file(SHA256 "${CAPSID_REPRO_ROOT}/share/capsid/build-info.txt"
    CAPSID_REPRO_INFO_SHA)
if(NOT CAPSID_REPRO_BASELINE_INFO_SHA STREQUAL CAPSID_REPRO_INFO_SHA)
    message(FATAL_ERROR "build-info.txt differs beyond the identity records:\n"
        "baseline ${CAPSID_REPRO_BASELINE_INFO_SHA}\n"
        "repro    ${CAPSID_REPRO_INFO_SHA}")
endif()

# ---- compare SBOM identity fields --------------------------------------------
# The SBOM itself embeds per-build binary hashes (package verification code),
# so only its identity fields are byte-compared.
file(READ "${CAPSID_REPRO_ROOT}/share/capsid/SBOM.spdx.json"
    CAPSID_REPRO_SBOM)
file(READ "${CAPSID_REPRO_BASELINE_ROOT}/share/capsid/SBOM.spdx.json"
    CAPSID_REPRO_BASELINE_SBOM)
foreach(sbom_field IN ITEMS name spdxVersion documentNamespace)
    string(JSON baseline_value GET "${CAPSID_REPRO_BASELINE_SBOM}"
        "${sbom_field}")
    string(JSON repro_value GET "${CAPSID_REPRO_SBOM}" "${sbom_field}")
    if(NOT baseline_value STREQUAL repro_value)
        message(FATAL_ERROR "SBOM field ${sbom_field} differs: baseline "
            "[${baseline_value}] vs repro [${repro_value}]")
    endif()
endforeach()
string(JSON baseline_created GET "${CAPSID_REPRO_BASELINE_SBOM}"
    creationInfo created)
string(JSON repro_created GET "${CAPSID_REPRO_SBOM}" creationInfo created)
if(NOT baseline_created STREQUAL repro_created)
    message(FATAL_ERROR "SBOM creationInfo.created differs: baseline "
        "[${baseline_created}] vs repro [${repro_created}]")
endif()

# ---- deterministic text files must hash identically --------------------------
# Everything except FILE-MANIFEST.txt and SBOM.spdx.json (self-listed binary
# hash carriers).
set(CAPSID_REPRO_TEXT_FILES)
foreach(manifest_name IN LISTS CAPSID_REPRO_NAMES)
    if(manifest_name MATCHES
            "^(include/|share/doc/|share/licenses/|lib/cmake/)"
            OR manifest_name STREQUAL "share/capsid/build-info.txt")
        list(APPEND CAPSID_REPRO_TEXT_FILES "${manifest_name}")
    endif()
endforeach()
foreach(text_file IN LISTS CAPSID_REPRO_TEXT_FILES)
    file(SHA256 "${CAPSID_REPRO_ROOT}/${text_file}" repro_sha)
    file(SHA256 "${CAPSID_REPRO_BASELINE_ROOT}/${text_file}" baseline_sha)
    if(NOT repro_sha STREQUAL baseline_sha)
        message(FATAL_ERROR "deterministic file differs: ${text_file}\n"
            "baseline ${baseline_sha}\nrepro ${repro_sha}")
    endif()
endforeach()

# ---- record binary differences -----------------------------------------------
set(CAPSID_REPRO_DIFF_COUNT 0)
foreach(manifest_name IN LISTS CAPSID_REPRO_NAMES)
    if(manifest_name MATCHES
            "^(include/|share/doc/|share/licenses/|lib/cmake/)"
            OR manifest_name STREQUAL "share/capsid/build-info.txt")
        continue()
    endif()
    file(SHA256 "${CAPSID_REPRO_ROOT}/${manifest_name}" repro_sha)
    file(SHA256 "${CAPSID_REPRO_BASELINE_ROOT}/${manifest_name}"
        baseline_sha)
    if(NOT repro_sha STREQUAL baseline_sha)
        math(EXPR CAPSID_REPRO_DIFF_COUNT "${CAPSID_REPRO_DIFF_COUNT} + 1")
        file(SIZE "${CAPSID_REPRO_ROOT}/${manifest_name}" repro_size)
        file(SIZE "${CAPSID_REPRO_BASELINE_ROOT}/${manifest_name}"
            baseline_size)
        string(APPEND CAPSID_REPRO_DIFFS
            "${manifest_name}\n  baseline ${baseline_sha} (${baseline_size}B)\n"
            "  repro    ${repro_sha} (${repro_size}B)\n")
    endif()
endforeach()

message(STATUS "PASS: reproducibility — ${baseline_name_count} files, "
    "identity and deterministic text identical; "
    "${CAPSID_REPRO_DIFF_COUNT} binary hash differences recorded at "
    "${CAPSID_REPRO_DIFFS}")
