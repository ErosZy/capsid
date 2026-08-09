# Capsid build identity and provenance.
#
# One generated data source shared by capsid_runtime, capsid-worker and
# capsid-bytecode-compile. Two canonical records are written at configure
# time, both pinned by include/capsid/runtime.h:
#
#   generated/build-identity-record.txt   "schema=capsid-bytecode-compatibility-v2"
#     the bytecode-compatibility record (spec §11.2): only the fields that
#     change whether one build's QuickJS bytecode loads in another build.
#     The compatibility ID is "sha256:" plus the lowercase hex SHA-256 of
#     exactly those bytes. Nobody recomputes the ID at runtime.
#
#   generated/build-provenance-record.txt "schema=capsid-build-provenance-v1"
#     the build provenance record (spec §11.3): git commit and clean-tree
#     state, toolchain and configuration, feature flags, and the
#     dependency lock. build_id is the SHA-256 of the record minus the
#     final buildId line, so any holder of the exposed fields can
#     recompute it. Release configures fail closed when the commit cannot
#     be resolved or the worktree is dirty; development configures record
#     a dirty provenance that release packaging must reject.
#
# Locked inputs (docs/txiki-upgrade-baseline.json):
#   vendor.commit            outer txiki.js commit (40 hex)
#   quickjs.commit           deps/quickjs gitlink inside txiki.js (40 hex)
#   overlay.key              overlay key from capsid_compute_txiki_overlay_key
#   overlay.manifest         overlay content manifest from the stamp
# With CAPSID_BUILD_WORKER=ON the locked values are compared against the
# actual prepared overlay (outer txiki.js HEAD, the QuickJS checkout and
# the stamped key/manifest) and the build fails on any mismatch, so the
# identity can never silently drift from the built worker.

function(capsid_load_locked_build_identity)
    set(ONE_VALUE_ARGS JSON_PATH
        OUT_VENDOR_COMMIT OUT_QUICKJS_COMMIT
        OUT_OVERLAY_KEY OUT_OVERLAY_MANIFEST)
    cmake_parse_arguments(CGBL "" "${ONE_VALUE_ARGS}" "" ${ARGN})
    # WP-07, spec §11.1: any stray argument is a caller bug that would
    # otherwise silently truncate a one-value argument. Fail the configure.
    if(CGBL_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "capsid_load_locked_build_identity received unparsed arguments "
            "(${CGBL_UNPARSED_ARGUMENTS}); pass one value per argument")
    endif()
    if(NOT EXISTS "${CGBL_JSON_PATH}")
        message(FATAL_ERROR
            "Locked build-identity manifest is missing: ${CGBL_JSON_PATH}")
    endif()
    file(READ "${CGBL_JSON_PATH}" CGBL_JSON)
    string(JSON CGBL_SCHEMA ERROR_VARIABLE CGBL_JSON_ERROR
        GET "${CGBL_JSON}" schema_version)
    if(NOT CGBL_JSON_ERROR STREQUAL "NOTFOUND" OR NOT CGBL_SCHEMA EQUAL 1)
        message(FATAL_ERROR
            "Locked build-identity manifest has an invalid schema_version")
    endif()
    string(JSON CGBL_VENDOR_COMMIT ERROR_VARIABLE CGBL_JSON_ERROR
        GET "${CGBL_JSON}" vendor commit)
    if(NOT CGBL_JSON_ERROR STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "Locked build-identity manifest is missing vendor.commit")
    endif()
    string(JSON CGBL_QUICKJS_COMMIT ERROR_VARIABLE CGBL_JSON_ERROR
        GET "${CGBL_JSON}" quickjs commit)
    if(NOT CGBL_JSON_ERROR STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "Locked build-identity manifest is missing quickjs.commit")
    endif()
    string(JSON CGBL_OVERLAY_KEY ERROR_VARIABLE CGBL_JSON_ERROR
        GET "${CGBL_JSON}" overlay key)
    if(NOT CGBL_JSON_ERROR STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "Locked build-identity manifest is missing overlay.key")
    endif()
    string(JSON CGBL_OVERLAY_MANIFEST ERROR_VARIABLE CGBL_JSON_ERROR
        GET "${CGBL_JSON}" overlay manifest)
    if(NOT CGBL_JSON_ERROR STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "Locked build-identity manifest is missing overlay.manifest")
    endif()
    # CMake regexes have no {n} repetition; validate shape with a class-only
    # pattern plus an exact length.
    string(LENGTH "${CGBL_VENDOR_COMMIT}" CGBL_VENDOR_COMMIT_LENGTH)
    string(LENGTH "${CGBL_QUICKJS_COMMIT}" CGBL_QUICKJS_COMMIT_LENGTH)
    string(LENGTH "${CGBL_OVERLAY_KEY}" CGBL_OVERLAY_KEY_LENGTH)
    string(LENGTH "${CGBL_OVERLAY_MANIFEST}" CGBL_OVERLAY_MANIFEST_LENGTH)
    if(NOT CGBL_VENDOR_COMMIT MATCHES "^[0-9a-f]+$" OR
       NOT CGBL_VENDOR_COMMIT_LENGTH EQUAL 40 OR
       NOT CGBL_QUICKJS_COMMIT MATCHES "^[0-9a-f]+$" OR
       NOT CGBL_QUICKJS_COMMIT_LENGTH EQUAL 40 OR
       NOT CGBL_OVERLAY_KEY MATCHES "^[0-9a-f]+$" OR
       NOT CGBL_OVERLAY_KEY_LENGTH EQUAL 64 OR
       NOT CGBL_OVERLAY_MANIFEST MATCHES "^[0-9a-f]+$" OR
       NOT CGBL_OVERLAY_MANIFEST_LENGTH EQUAL 64)
        message(FATAL_ERROR
            "Locked build-identity manifest must carry 40-hex vendor and "
            "quickjs commits and 64-hex overlay key and manifest")
    endif()
    set(${CGBL_OUT_VENDOR_COMMIT} "${CGBL_VENDOR_COMMIT}" PARENT_SCOPE)
    set(${CGBL_OUT_QUICKJS_COMMIT} "${CGBL_QUICKJS_COMMIT}" PARENT_SCOPE)
    set(${CGBL_OUT_OVERLAY_KEY} "${CGBL_OVERLAY_KEY}" PARENT_SCOPE)
    set(${CGBL_OUT_OVERLAY_MANIFEST} "${CGBL_OVERLAY_MANIFEST}" PARENT_SCOPE)
endfunction()

function(capsid_generate_build_identity)
    set(ONE_VALUE_ARGS
        OUT_HEADER
        LOCKED_MANIFEST
        CAPSID_SOURCE_DIR
        RUNTIME_VERSION
        ABI_VERSION
        FETCHRPC_VERSION
        COMPILE_FLAGS
        ARCHITECTURE
        ENDIANNESS
        POINTER_WIDTH_BITS
        BYTECODE_FORMAT
        CAPABILITY_MANIFEST_SHA256
        BUILD_TYPE
        LTO
        ASAN
        UBSAN
        TSAN
        MIMALLOC
        HOST
        WORKER
        COMPILER_ID
        COMPILER_VERSION
        TARGET_TRIPLE)
    cmake_parse_arguments(CGBI "" "${ONE_VALUE_ARGS}" "" ${ARGN})
    # WP-07, spec §11.1: any stray argument is a caller bug that would
    # otherwise silently truncate a one-value argument (a multi-argument
    # set() forms a semicolon list and only the first element arrives).
    # Fail the configure instead of building a wrong identity.
    if(CGBI_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "capsid_generate_build_identity received unparsed arguments "
            "(${CGBI_UNPARSED_ARGUMENTS}); pass one canonical string per "
            "one-value argument")
    endif()

    capsid_load_locked_build_identity(
        JSON_PATH "${CGBI_LOCKED_MANIFEST}"
        OUT_VENDOR_COMMIT CGBI_VENDOR_COMMIT
        OUT_QUICKJS_COMMIT CGBI_QUICKJS_COMMIT
        OUT_OVERLAY_KEY CGBI_OVERLAY_KEY
        OUT_OVERLAY_MANIFEST CGBI_OVERLAY_MANIFEST)

    if(CAPSID_BUILD_WORKER)
        # The overlay was prepared and stamped during configure
        # (build_worker.cmake); validate the locked values against the
        # actual build inputs: the outer txiki.js HEAD and the QuickJS
        # gitlink must both match their locked commits.
        find_program(CGBI_GIT git REQUIRED)
        execute_process(
            COMMAND "${CGBI_GIT}" -C
                "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js" rev-parse HEAD
            OUTPUT_VARIABLE CGBI_ACTUAL_VENDOR_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE CGBI_VENDOR_RESULT)
        if(NOT CGBI_VENDOR_RESULT EQUAL 0)
            message(FATAL_ERROR
                "Cannot resolve vendor HEAD revision for build identity")
        endif()
        if(NOT CGBI_ACTUAL_VENDOR_COMMIT STREQUAL CGBI_VENDOR_COMMIT)
            message(FATAL_ERROR
                "vendor/txiki.js HEAD ${CGBI_ACTUAL_VENDOR_COMMIT} differs "
                "from the locked commit ${CGBI_VENDOR_COMMIT}")
        endif()
        execute_process(
            COMMAND "${CGBI_GIT}" -C
                "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js/deps/quickjs"
                rev-parse HEAD
            OUTPUT_VARIABLE CGBI_ACTUAL_QUICKJS_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE CGBI_QUICKJS_RESULT)
        if(NOT CGBI_QUICKJS_RESULT EQUAL 0)
            message(FATAL_ERROR
                "Cannot resolve the QuickJS gitlink for build identity")
        endif()
        if(NOT CGBI_ACTUAL_QUICKJS_COMMIT STREQUAL CGBI_QUICKJS_COMMIT)
            message(FATAL_ERROR
                "vendor/txiki.js/deps/quickjs HEAD "
                "${CGBI_ACTUAL_QUICKJS_COMMIT} differs from the locked "
                "gitlink ${CGBI_QUICKJS_COMMIT}")
        endif()
        set(CGBI_STAMP "${CMAKE_CURRENT_BINARY_DIR}/vendor-overlay/txiki.js/.capsid-overlay-key")
        file(STRINGS "${CGBI_STAMP}" CGBI_STAMP_LINES)
        list(LENGTH CGBI_STAMP_LINES CGBI_STAMP_COUNT)
        if(NOT CGBI_STAMP_COUNT EQUAL 3)
            message(FATAL_ERROR
                "Overlay stamp is missing; the worker overlay must be "
                "prepared before the build identity is generated")
        endif()
        list(GET CGBI_STAMP_LINES 1 CGBI_STAMP_KEY_LINE)
        list(GET CGBI_STAMP_LINES 2 CGBI_STAMP_MANIFEST_LINE)
        string(REGEX REPLACE "^key=" "" CGBI_ACTUAL_KEY
            "${CGBI_STAMP_KEY_LINE}")
        string(REGEX REPLACE "^manifest=" "" CGBI_ACTUAL_MANIFEST
            "${CGBI_STAMP_MANIFEST_LINE}")
        if(NOT CGBI_ACTUAL_KEY STREQUAL CGBI_OVERLAY_KEY OR
           NOT CGBI_ACTUAL_MANIFEST STREQUAL CGBI_OVERLAY_MANIFEST)
            message(FATAL_ERROR
                "Prepared overlay key/manifest differs from the locked "
                "build identity; update docs/txiki-upgrade-baseline.json "
                "only after an audited vendor upgrade")
        endif()
    endif()

    # WP-07, spec §11.3: Capsid git provenance. Release configures fail
    # closed when the commit cannot be resolved or the worktree is dirty;
    # development configures record the truth ("unknown" commit / "false"
    # tree) and mark the provenance dirty so release packaging rejects it.
    set(CGBI_CAPSID_COMMIT "unknown")
    set(CGBI_TREE_CLEAN "false")
    find_program(CGBI_GIT git)
    if(CGBI_GIT)
        execute_process(
            COMMAND "${CGBI_GIT}" -C "${CGBI_CAPSID_SOURCE_DIR}"
                rev-parse HEAD
            OUTPUT_VARIABLE CGBI_CAPSID_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE CGBI_GIT_COMMIT_RESULT)
        if(NOT CGBI_GIT_COMMIT_RESULT EQUAL 0)
            set(CGBI_CAPSID_COMMIT "unknown")
        endif()
        if(CGBI_CAPSID_COMMIT MATCHES "^[0-9a-f]{40}$")
            execute_process(
                COMMAND "${CGBI_GIT}" -C "${CGBI_CAPSID_SOURCE_DIR}"
                    status --porcelain
                OUTPUT_VARIABLE CGBI_TREE_STATUS
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE CGBI_GIT_STATUS_RESULT)
            if(CGBI_GIT_STATUS_RESULT EQUAL 0 AND
               CGBI_TREE_STATUS STREQUAL "")
                set(CGBI_TREE_CLEAN "true")
            endif()
        endif()
    endif()
    if(CGBI_CAPSID_COMMIT MATCHES "^[0-9a-f]{40}$")
        set(CGBI_COMMIT_KNOWN TRUE)
    else()
        set(CGBI_COMMIT_KNOWN FALSE)
    endif()
    if(CGBI_BUILD_TYPE STREQUAL "Release")
        if(NOT CGBI_COMMIT_KNOWN)
            message(FATAL_ERROR
                "Release configure cannot resolve the Capsid git commit; "
                "release builds must come from a tagged checkout")
        endif()
        if(NOT CGBI_TREE_CLEAN STREQUAL "true")
            message(FATAL_ERROR
                "Release configure requires a clean worktree; "
                "git status --porcelain must be empty")
        endif()
    endif()
    if(CGBI_BUILD_TYPE STREQUAL "Release" AND CGBI_COMMIT_KNOWN AND
       CGBI_TREE_CLEAN STREQUAL "true")
        set(CGBI_PROVENANCE_DIRTY 0)
    else()
        set(CGBI_PROVENANCE_DIRTY 1)
    endif()

    # WP-07, spec §11.2: the compatibility record only carries the fields
    # that change whether one build's QuickJS bytecode reads in another
    # build. Anything that identifies the build but not the bytecode
    # (runtime/ABI/FetchRPC versions, overlay key, capability manifest)
    # moved to the provenance record. Record-schema bumps must be mirrored
    # in include/capsid/runtime.h and the identity matrix test.
    set(CGBI_RECORD "schema=capsid-bytecode-compatibility-v2\n")
    string(APPEND CGBI_RECORD "quickjsCommit=${CGBI_QUICKJS_COMMIT}\n")
    string(APPEND CGBI_RECORD
        "txikiOverlayManifest=${CGBI_OVERLAY_MANIFEST}\n")
    string(APPEND CGBI_RECORD "bytecodeCompileFlags=${CGBI_COMPILE_FLAGS}\n")
    string(APPEND CGBI_RECORD "targetArchitecture=${CGBI_ARCHITECTURE}\n")
    string(APPEND CGBI_RECORD "endianness=${CGBI_ENDIANNESS}\n")
    string(APPEND CGBI_RECORD "pointerWidthBits=${CGBI_POINTER_WIDTH_BITS}\n")
    string(APPEND CGBI_RECORD
        "bytecodeFormatIdentity=${CGBI_BYTECODE_FORMAT}\n")

    # The compatibility ID is the SHA-256 of exactly the record bytes above.
    set(CGBI_RECORD_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/build-identity-record.txt")
    file(WRITE "${CGBI_RECORD_FILE}" "${CGBI_RECORD}")
    file(SHA256 "${CGBI_RECORD_FILE}" CGBI_DIGEST)
    set(CGBI_COMPATIBILITY_ID "sha256:${CGBI_DIGEST}")

    # WP-07, spec §11.3: the build provenance record. Feature flags are one
    # canonical string (same §11.1 discipline as COMPILE_FLAGS). build_id
    # is the SHA-256 of the record WITHOUT the final buildId line, so the
    # digest stays recomputable by any holder of the exposed fields.
    set(CGBI_FEATURE_FLAGS
        "lto=${CGBI_LTO} asan=${CGBI_ASAN} ubsan=${CGBI_UBSAN} tsan=${CGBI_TSAN} mimalloc=${CGBI_MIMALLOC} host=${CGBI_HOST} worker=${CGBI_WORKER}")
    set(CGBI_PROV_RECORD "schema=capsid-build-provenance-v1\n")
    string(APPEND CGBI_PROV_RECORD "capsidCommit=${CGBI_CAPSID_COMMIT}\n")
    string(APPEND CGBI_PROV_RECORD "capsidTreeClean=${CGBI_TREE_CLEAN}\n")
    string(APPEND CGBI_PROV_RECORD
        "runtimeVersion=${CGBI_RUNTIME_VERSION}\n")
    string(APPEND CGBI_PROV_RECORD "abiVersion=${CGBI_ABI_VERSION}\n")
    string(APPEND CGBI_PROV_RECORD
        "fetchRpcVersion=${CGBI_FETCHRPC_VERSION}\n")
    string(APPEND CGBI_PROV_RECORD
        "compatibilityId=${CGBI_COMPATIBILITY_ID}\n")
    string(APPEND CGBI_PROV_RECORD
        "capabilityManifestSha256=${CGBI_CAPABILITY_MANIFEST_SHA256}\n")
    string(APPEND CGBI_PROV_RECORD "compilerId=${CGBI_COMPILER_ID}\n")
    string(APPEND CGBI_PROV_RECORD
        "compilerVersion=${CGBI_COMPILER_VERSION}\n")
    string(APPEND CGBI_PROV_RECORD "targetTriple=${CGBI_TARGET_TRIPLE}\n")
    string(APPEND CGBI_PROV_RECORD "cmakeBuildType=${CGBI_BUILD_TYPE}\n")
    string(APPEND CGBI_PROV_RECORD "featureFlags=${CGBI_FEATURE_FLAGS}\n")
    string(APPEND CGBI_PROV_RECORD
        "dependencyOverlayKey=${CGBI_OVERLAY_KEY}\n")
    string(SHA256 CGBI_PROV_DIGEST "${CGBI_PROV_RECORD}")
    string(APPEND CGBI_PROV_RECORD "buildId=sha256:${CGBI_PROV_DIGEST}\n")
    set(CGBI_PROV_RECORD_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/build-provenance-record.txt")
    file(WRITE "${CGBI_PROV_RECORD_FILE}" "${CGBI_PROV_RECORD}")
    set(CGBI_BUILD_ID "sha256:${CGBI_PROV_DIGEST}")

    set(CAPSID_IDENTITY_RUNTIME_VERSION "${CGBI_RUNTIME_VERSION}")
    set(CAPSID_IDENTITY_ABI_VERSION "${CGBI_ABI_VERSION}")
    set(CAPSID_IDENTITY_FETCHRPC_VERSION "${CGBI_FETCHRPC_VERSION}")
    set(CAPSID_IDENTITY_QUICKJS_COMMIT "${CGBI_QUICKJS_COMMIT}")
    set(CAPSID_IDENTITY_OVERLAY_KEY "${CGBI_OVERLAY_KEY}")
    set(CAPSID_IDENTITY_OVERLAY_MANIFEST "${CGBI_OVERLAY_MANIFEST}")
    set(CAPSID_IDENTITY_COMPILE_FLAGS "${CGBI_COMPILE_FLAGS}")
    set(CAPSID_IDENTITY_ARCHITECTURE "${CGBI_ARCHITECTURE}")
    set(CAPSID_IDENTITY_ENDIANNESS "${CGBI_ENDIANNESS}")
    set(CAPSID_IDENTITY_POINTER_WIDTH "${CGBI_POINTER_WIDTH_BITS}")
    set(CAPSID_IDENTITY_BYTECODE_FORMAT "${CGBI_BYTECODE_FORMAT}")
    set(CAPSID_IDENTITY_CAPABILITY_MANIFEST_SHA256
        "${CGBI_CAPABILITY_MANIFEST_SHA256}")
    set(CAPSID_IDENTITY_COMPATIBILITY_ID "${CGBI_COMPATIBILITY_ID}")
    set(CAPSID_IDENTITY_CAPSID_COMMIT "${CGBI_CAPSID_COMMIT}")
    if(CGBI_TREE_CLEAN STREQUAL "true")
        set(CAPSID_IDENTITY_TREE_CLEAN 1)
    else()
        set(CAPSID_IDENTITY_TREE_CLEAN 0)
    endif()
    set(CAPSID_IDENTITY_PROVENANCE_DIRTY "${CGBI_PROVENANCE_DIRTY}")
    set(CAPSID_IDENTITY_COMPILER_ID "${CGBI_COMPILER_ID}")
    set(CAPSID_IDENTITY_COMPILER_VERSION "${CGBI_COMPILER_VERSION}")
    set(CAPSID_IDENTITY_TARGET_TRIPLE "${CGBI_TARGET_TRIPLE}")
    set(CAPSID_IDENTITY_BUILD_TYPE "${CGBI_BUILD_TYPE}")
    set(CAPSID_IDENTITY_FEATURE_FLAGS "${CGBI_FEATURE_FLAGS}")
    set(CAPSID_IDENTITY_BUILD_ID "${CGBI_BUILD_ID}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_identity.h.in"
        "${CGBI_OUT_HEADER}"
        @ONLY)

    set(CAPSID_BUILD_COMPATIBILITY_ID
        "${CGBI_COMPATIBILITY_ID}" PARENT_SCOPE)
    set(CAPSID_BUILD_CANONICAL_RECORD "${CGBI_RECORD}" PARENT_SCOPE)
    set(CAPSID_BUILD_PROVENANCE_DIRTY
        "${CGBI_PROVENANCE_DIRTY}" PARENT_SCOPE)
    set(CAPSID_BUILD_BUILD_ID "${CGBI_BUILD_ID}" PARENT_SCOPE)
endfunction()
