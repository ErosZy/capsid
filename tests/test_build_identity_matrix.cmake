# Build identity matrix (WP-00 / PR-01 RED gate for P0-7, hardened by
# WP-07 per spec §11.2/§11.4).
#
# Two records are generated at configure time into <build>/generated/:
#   build-identity-record.txt    bytecode compatibility v2
#   build-provenance-record.txt  build provenance v1 (build_id)
#
# Assertions (spec §11.4):
#   - every controlled build difference changes the build ID;
#   - a real bytecode-ABI difference (different QuickJS commit) changes the
#     compatibility ID;
#   - identical configure twice produces identical records;
#   - every required record field is present (the per-line regexes detect
#     a missing field, not just a self-consistent hash);
#   - the compile-flags line carries every key (spec §11.1: a CMake list
#     truncation drops asan/ubsan/mimalloc — that is the pre-WP-07 bug);
#   - the Release fail-closed path yields clean provenance (known commit,
#     clean tree) — which requires a clean worktree to run at all.
#
# Each matrix entry uses a FRESH configure directory; reusing an old build
# directory must never be accepted as evidence.
#
# Usage (script mode):
#   cmake -DCAPSID_SOURCE_DIR=... -DCAPSID_CMAKE_COMMAND=...
#        [-DCAPSID_MATRIX_VARIANTS="plain;asan;ubsan;mimalloc;lto-off;ext-fusion34;quickjs-diff"]
#        [-DCAPSID_MATRIX_WORK_DIR=...]
#        -P tests/test_build_identity_matrix.cmake

if(NOT CAPSID_SOURCE_DIR OR NOT CAPSID_CMAKE_COMMAND)
    message(FATAL_ERROR
        "CAPSID_SOURCE_DIR and CAPSID_CMAKE_COMMAND are required")
endif()

if(NOT CAPSID_MATRIX_VARIANTS)
    set(CAPSID_MATRIX_VARIANTS
        "plain;asan;ubsan;mimalloc;lto-off;ext-fusion34;quickjs-diff")
endif()
# MSVC has no UBSan/TSan runtime, and LTO is forced off (MSVC /GL binds
# replaceable operator new/delete); those variants cannot configure or
# cannot differ, so they are not part of the Windows matrix.
if(WIN32)
    list(REMOVE_ITEM CAPSID_MATRIX_VARIANTS "ubsan" "tsan" "lto-off")
endif()

if(NOT CAPSID_MATRIX_WORK_DIR)
    set(CAPSID_MATRIX_WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/identity-matrix")
endif()

# The Release variants configure with CMAKE_BUILD_TYPE=Release, which fails
# closed on a dirty worktree (spec §11.3). A clear precondition beats six
# confusing per-variant configure errors.
find_program(MATRIX_GIT git REQUIRED)
execute_process(
    COMMAND "${MATRIX_GIT}" -C "${CAPSID_SOURCE_DIR}" status --porcelain
    OUTPUT_VARIABLE MATRIX_TREE_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE MATRIX_GIT_RESULT)
if(NOT MATRIX_GIT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "build identity matrix cannot check the worktree; the Release "
        "variants fail closed without one")
endif()
if(NOT MATRIX_TREE_STATUS STREQUAL "")
    message(FATAL_ERROR
        "build identity matrix requires a clean worktree (git status "
        "--porcelain must be empty); commit or stash first, then re-run")
endif()

# Every variant must be a fresh configure. The work root is wiped first so
# a reused directory can never leak into the comparison.
file(REMOVE_RECURSE "${CAPSID_MATRIX_WORK_DIR}")
file(MAKE_DIRECTORY "${CAPSID_MATRIX_WORK_DIR}")

# A second locked manifest whose QuickJS commit differs: a real bytecode
# ABI difference. Both vendor and quickjs commits are rewritten so the
# shape validation still passes; the fake file must actually differ.
set(MATRIX_FAKE_MANIFEST "${CAPSID_MATRIX_WORK_DIR}/locked-quickjs-diff.json")
file(READ "${CAPSID_SOURCE_DIR}/docs/txiki-upgrade-baseline.json"
    MATRIX_BASE_JSON)
string(REGEX REPLACE
    "\"commit\"[ ]*:[ ]*\"[0-9a-f]+\""
    "\"commit\":\"0123456789abcdef0123456789abcdef01234567\""
    MATRIX_FAKE_JSON "${MATRIX_BASE_JSON}")
if(MATRIX_FAKE_JSON STREQUAL MATRIX_BASE_JSON)
    message(FATAL_ERROR
        "could not build a different locked manifest for the "
        "quickjs-diff variant")
endif()
file(WRITE "${MATRIX_FAKE_MANIFEST}" "${MATRIX_FAKE_JSON}")

function(capsid_matrix_configure variant build_dir)
    if("${variant}" STREQUAL "plain")
        set(extra_flags)
    elseif("${variant}" STREQUAL "asan")
        set(extra_flags "-DCAPSID_ENABLE_ASAN=ON")
    elseif("${variant}" STREQUAL "ubsan")
        set(extra_flags "-DCAPSID_ENABLE_UBSAN=ON")
    elseif("${variant}" STREQUAL "mimalloc")
        set(extra_flags "-DCAPSID_USE_MIMALLOC=ON")
    elseif("${variant}" STREQUAL "lto-off")
        set(extra_flags "-DCAPSID_ENABLE_LTO=OFF")
    elseif("${variant}" STREQUAL "ext-fusion34")
        set(extra_flags "-DCAPSID_ENABLE_EXT_FUSION34=ON")
    elseif("${variant}" STREQUAL "quickjs-diff")
        set(extra_flags
            "-DCAPSID_LOCKED_IDENTITY_MANIFEST=${MATRIX_FAKE_MANIFEST}")
    else()
        message(FATAL_ERROR "unknown matrix variant: ${variant}")
    endif()
    execute_process(
        COMMAND "${CAPSID_CMAKE_COMMAND}"
            -S "${CAPSID_SOURCE_DIR}"
            -B "${build_dir}"
            -DCAPSID_BUILD_WORKER=OFF
            -DBUILD_TESTING=OFF
            -DCMAKE_BUILD_TYPE=Release
            ${extra_flags}
        RESULT_VARIABLE configure_result
        OUTPUT_VARIABLE configure_output
        ERROR_VARIABLE configure_error)
    if(NOT configure_result EQUAL 0)
        message(FATAL_ERROR
            "variant ${variant} configure failed:\n${configure_output}"
            "\n${configure_error}")
    endif()
endfunction()

# Full per-line validation of both records. Any missing or misordered field
# fails the regex — a record that merely hashes self-consistently but lost a
# field is a FAIL here.
function(capsid_matrix_record variant build_dir
        out_compat_record out_compat_digest
        out_prov_record out_prov_digest out_build_id)
    set(compat_file "${build_dir}/generated/build-identity-record.txt")
    if(NOT EXISTS "${compat_file}")
        message(FATAL_ERROR
            "variant ${variant} produced no compatibility record: "
            "${compat_file}")
    endif()
    file(READ "${compat_file}" compat_record)
    # Hash the in-memory record: the generator hashes the canonical string
    # (string(SHA256)), and the on-disk file carries CRLF on Windows.
    string(SHA256 compat_digest "${compat_record}")
    if(NOT compat_record MATCHES
       "^schema=capsid-bytecode-compatibility-v2\n")
        message(FATAL_ERROR
            "variant ${variant} record has the wrong schema header")
    endif()
    # Every bytecode-affecting field, in the fixed order of §11.2. CMake
    # regexes have no {n} repetition, so the hex fields match as [0-9a-f]+
    # and their exact lengths are verified below by extraction. The regex
    # is assembled with string(CONCAT): if() takes exactly one argument
    # after MATCHES and does not concatenate adjacent quoted strings.
    string(CONCAT compat_regex
        "^schema=capsid-bytecode-compatibility-v2\nquickjsCommit=[0-9a-f]+\n"
        "txikiOverlayManifest=[0-9a-f]+\n"
        "bytecodeCompileFlags=build_type=[^ ]* lto=(ON|OFF) asan=(ON|OFF) "
        "ubsan=(ON|OFF) mimalloc=(ON|OFF) ext_fusion34=(ON|OFF)\n"
        "targetArchitecture=[^\n]+\nendianness=(little|big)\n"
        "pointerWidthBits=[0-9]+\n"
        "bytecodeFormatIdentity=quickjs-ng-bytecode-v1\n$")
    if(NOT compat_record MATCHES "${compat_regex}")
        message(FATAL_ERROR
            "variant ${variant} compatibility record is missing a required "
            "field or the compile flags are incomplete; record:\n"
            "${compat_record}")
    endif()
    # Exact hex lengths, verified by extraction (CMake regexes have no {n}
    # repetition, so the + classes above accept any positive length).
    string(REGEX MATCH "quickjsCommit=[0-9a-f]+" quickjs_commit_match
        "${compat_record}")
    string(REGEX REPLACE "^quickjsCommit=" "" quickjs_commit
        "${quickjs_commit_match}")
    string(LENGTH "${quickjs_commit}" quickjs_commit_len)
    if(NOT quickjs_commit_len EQUAL 40)
        message(FATAL_ERROR
            "variant ${variant} quickjsCommit is not 40 hex: "
            "${quickjs_commit}")
    endif()
    string(REGEX MATCH "txikiOverlayManifest=[0-9a-f]+"
        overlay_manifest_match "${compat_record}")
    string(REGEX REPLACE "^txikiOverlayManifest=" "" overlay_manifest
        "${overlay_manifest_match}")
    string(LENGTH "${overlay_manifest}" overlay_manifest_len)
    if(NOT overlay_manifest_len EQUAL 64)
        message(FATAL_ERROR
            "variant ${variant} txikiOverlayManifest is not 64 hex: "
            "${overlay_manifest}")
    endif()

    set(prov_file "${build_dir}/generated/build-provenance-record.txt")
    if(NOT EXISTS "${prov_file}")
        message(FATAL_ERROR
            "variant ${variant} produced no provenance record: ${prov_file}")
    endif()
    file(READ "${prov_file}" prov_record)
    string(SHA256 prov_digest "${prov_record}")
    # Release fail-closed in a clean worktree: the provenance is clean.
    string(CONCAT prov_regex
        "^schema=capsid-build-provenance-v1\n"
        "capsidCommit=[0-9a-f]+\n"
        "capsidTreeClean=true\n"
        "runtimeVersion=[^\n]+\nabiVersion=[0-9]+\nfetchRpcVersion=[0-9]+\n"
        "compatibilityId=sha256:[0-9a-f]+\n"
        "capabilityManifestSha256=[0-9a-f]+\n"
        "compilerId=[^\n]+\ncompilerVersion=[^\n]+\n"
        "targetTriple=[^\n]+\ncmakeBuildType=Release\n"
        "featureFlags=lto=(ON|OFF) asan=(ON|OFF) ubsan=(ON|OFF) "
        "tsan=(ON|OFF) mimalloc=(ON|OFF) host=(ON|OFF) worker=(ON|OFF)\n"
        "dependencyOverlayKey=[0-9a-f]+\n"
        "buildId=sha256:[0-9a-f]+\n$")
    if(NOT prov_record MATCHES "${prov_regex}")
        message(FATAL_ERROR
            "variant ${variant} provenance record is missing a required "
            "field or the feature flags are incomplete; record:\n"
            "${prov_record}")
    endif()
    # Exact hex lengths, verified by extraction (the + classes above accept
    # any positive length; a truncated hash must fail here).
    string(REGEX MATCH "capsidCommit=[0-9a-f]+" capsid_commit_match
        "${prov_record}")
    string(REGEX REPLACE "^capsidCommit=" "" capsid_commit
        "${capsid_commit_match}")
    string(LENGTH "${capsid_commit}" capsid_commit_len)
    if(NOT capsid_commit_len EQUAL 40)
        message(FATAL_ERROR
            "variant ${variant} capsidCommit is not 40 hex: ${capsid_commit}")
    endif()
    string(REGEX MATCH "capabilityManifestSha256=[0-9a-f]+"
        cap_manifest_match "${prov_record}")
    string(REGEX REPLACE "^capabilityManifestSha256=" "" cap_manifest
        "${cap_manifest_match}")
    string(LENGTH "${cap_manifest}" cap_manifest_len)
    if(NOT cap_manifest_len EQUAL 64)
        message(FATAL_ERROR
            "variant ${variant} capabilityManifestSha256 is not 64 hex: "
            "${cap_manifest}")
    endif()
    string(REGEX MATCH "dependencyOverlayKey=[0-9a-f]+" overlay_key_match
        "${prov_record}")
    string(REGEX REPLACE "^dependencyOverlayKey=" "" overlay_key
        "${overlay_key_match}")
    string(LENGTH "${overlay_key}" overlay_key_len)
    if(NOT overlay_key_len EQUAL 64)
        message(FATAL_ERROR
            "variant ${variant} dependencyOverlayKey is not 64 hex: "
            "${overlay_key}")
    endif()
    # build_id must be the SHA-256 of the record minus its final buildId
    # line, recomputed here (spec §11.4: not just a self-consistent hash).
    string(REGEX MATCH "buildId=sha256:[0-9a-f]+" claimed_id
        "${prov_record}")
    string(REGEX REPLACE
        "buildId=sha256:[0-9a-f]+\n$" "" prov_without_id
        "${prov_record}")
    string(SHA256 recomputed_id "${prov_without_id}")
    set(expected_id "buildId=sha256:${recomputed_id}")
    if(NOT claimed_id STREQUAL expected_id)
        message(FATAL_ERROR
            "variant ${variant} build ID does not cover the provenance "
            "record minus the buildId line: ${claimed_id} vs ${expected_id}")
    endif()
    # The provenance record must reference the compatibility record of the
    # same configure.
    string(REGEX MATCH "compatibilityId=sha256:[0-9a-f]+"
        prov_compat_id "${prov_record}")
    set(expected_compat "compatibilityId=${compat_digest}")
    string(REGEX REPLACE "^compatibilityId=" "" expected_compat
        "${expected_compat}")
    set(expected_compat_line "compatibilityId=sha256:${expected_compat}")
    if(NOT prov_compat_id STREQUAL expected_compat_line)
        message(FATAL_ERROR
            "variant ${variant} provenance references a different "
            "compatibility record: ${prov_compat_id} vs "
            "${expected_compat_line}")
    endif()

    set(${out_compat_record} "${compat_record}" PARENT_SCOPE)
    set(${out_compat_digest} "${compat_digest}" PARENT_SCOPE)
    set(${out_prov_record} "${prov_record}" PARENT_SCOPE)
    set(${out_prov_digest} "${prov_digest}" PARENT_SCOPE)
    string(REGEX REPLACE "^buildId=sha256:" "" build_id "${claimed_id}")
    set(${out_build_id} "${build_id}" PARENT_SCOPE)
endfunction()

set(CAPSID_MATRIX_COMPAT_DIGESTS)
set(CAPSID_MATRIX_PROV_DIGESTS)
set(CAPSID_MATRIX_BUILD_IDS)
set(CAPSID_MATRIX_DIRS)
foreach(variant IN LISTS CAPSID_MATRIX_VARIANTS)
    set(build_dir
        "${CAPSID_MATRIX_WORK_DIR}/configure-${variant}")
    capsid_matrix_configure("${variant}" "${build_dir}")
    capsid_matrix_record("${variant}" "${build_dir}"
        compat_record compat_digest prov_record prov_digest build_id)
    list(APPEND CAPSID_MATRIX_COMPAT_DIGESTS "${compat_digest}")
    list(APPEND CAPSID_MATRIX_PROV_DIGESTS "${prov_digest}")
    list(APPEND CAPSID_MATRIX_BUILD_IDS "${build_id}")
    list(APPEND CAPSID_MATRIX_DIRS "${build_dir}")
    string(LENGTH "${compat_record}" compat_length)
    if(compat_length EQUAL 0)
        message(FATAL_ERROR
            "variant ${variant} compatibility record must not be empty")
    endif()
endforeach()

# Every controlled build difference must change the build ID; every real
# bytecode-ABI difference must change the compatibility ID. All matrix
# variants here differ in flags or in the QuickJS commit, so both record
# sets must be pairwise distinct.
list(LENGTH CAPSID_MATRIX_VARIANTS variant_count)
if(variant_count LESS 2)
    message(FATAL_ERROR
        "identity matrix needs at least two variants")
endif()
set(CAPSID_MATRIX_INDEX 0)
while(CAPSID_MATRIX_INDEX LESS variant_count)
    list(GET CAPSID_MATRIX_COMPAT_DIGESTS ${CAPSID_MATRIX_INDEX} compat)
    list(GET CAPSID_MATRIX_BUILD_IDS ${CAPSID_MATRIX_INDEX} build_id)
    set(CAPSID_MATRIX_OTHER 0)
    while(CAPSID_MATRIX_OTHER LESS variant_count)
        if(NOT CAPSID_MATRIX_OTHER EQUAL CAPSID_MATRIX_INDEX)
            list(GET CAPSID_MATRIX_COMPAT_DIGESTS ${CAPSID_MATRIX_OTHER}
                other_compat)
            list(GET CAPSID_MATRIX_BUILD_IDS ${CAPSID_MATRIX_OTHER}
                other_build_id)
            list(GET CAPSID_MATRIX_VARIANTS ${CAPSID_MATRIX_INDEX} variant)
            list(GET CAPSID_MATRIX_VARIANTS ${CAPSID_MATRIX_OTHER} other)
            if(build_id STREQUAL other_build_id)
                message(FATAL_ERROR
                    "build ID collision: ${variant} and ${other} both "
                    "produce ${build_id}")
            endif()
            if(compat STREQUAL other_compat)
                message(FATAL_ERROR
                    "compatibility ID collision: ${variant} and ${other} "
                    "both produce ${compat}")
            endif()
        endif()
        math(EXPR CAPSID_MATRIX_OTHER "${CAPSID_MATRIX_OTHER} + 1")
    endwhile()
    math(EXPR CAPSID_MATRIX_INDEX "${CAPSID_MATRIX_INDEX} + 1")
endwhile()

# Identical configure twice must produce identical records.
set(plain_index -1)
set(CAPSID_MATRIX_INDEX 0)
foreach(variant IN LISTS CAPSID_MATRIX_VARIANTS)
    if(variant STREQUAL "plain")
        set(plain_index ${CAPSID_MATRIX_INDEX})
    endif()
    math(EXPR CAPSID_MATRIX_INDEX "${CAPSID_MATRIX_INDEX} + 1")
endforeach()
if(plain_index LESS 0)
    message(FATAL_ERROR
        "identity matrix must include the plain variant for the "
        "repeatability check")
endif()
list(GET CAPSID_MATRIX_COMPAT_DIGESTS ${plain_index} plain_compat)
list(GET CAPSID_MATRIX_PROV_DIGESTS ${plain_index} plain_prov)
set(repeat_dir "${CAPSID_MATRIX_WORK_DIR}/configure-plain-repeat")
capsid_matrix_configure("plain" "${repeat_dir}")
capsid_matrix_record("plain" "${repeat_dir}"
    repeat_compat repeat_compat_digest
    repeat_prov repeat_prov_digest repeat_build_id)
if(NOT repeat_compat_digest STREQUAL plain_compat OR
   NOT repeat_prov_digest STREQUAL plain_prov)
    message(FATAL_ERROR
        "identical plain configure produced a different identity: "
        "${repeat_compat_digest}/${repeat_prov_digest} vs "
        "${plain_compat}/${plain_prov}")
endif()
list(APPEND CAPSID_MATRIX_DIRS "${repeat_dir}")

file(REMOVE_RECURSE ${CAPSID_MATRIX_DIRS})
file(REMOVE_RECURSE "${CAPSID_MATRIX_WORK_DIR}")
message(STATUS "PASS: ${variant_count} identity variants all distinct and "
        "reproducible")
