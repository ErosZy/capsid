# Capsid bytecode-compatibility identity.
#
# One generated data source shared by capsid_runtime, capsid-worker and
# capsid-bytecode-compile. The canonical record format is pinned by
# include/capsid/runtime.h ("schema=capsid-bytecode-compatibility-v1" plus
# the field lines below, each terminated by a newline, final newline
# included); the compatibility ID is "sha256:" plus the lowercase hex
# SHA-256 of exactly those bytes. Nobody recomputes the ID at runtime.
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
        RUNTIME_VERSION
        ABI_VERSION
        FETCHRPC_VERSION
        COMPILE_FLAGS
        ARCHITECTURE
        ENDIANNESS
        POINTER_WIDTH_BITS
        BYTECODE_FORMAT
        CAPABILITY_MANIFEST_SHA256)
    cmake_parse_arguments(CGBI "" "${ONE_VALUE_ARGS}" "" ${ARGN})

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

    # Assemble the canonical record exactly as documented in runtime.h.
    set(CGBI_RECORD "schema=capsid-bytecode-compatibility-v1\n")
    string(APPEND CGBI_RECORD "runtimeVersion=${CGBI_RUNTIME_VERSION}\n")
    string(APPEND CGBI_RECORD "abiVersion=${CGBI_ABI_VERSION}\n")
    string(APPEND CGBI_RECORD "fetchRpcVersion=${CGBI_FETCHRPC_VERSION}\n")
    string(APPEND CGBI_RECORD "quickjsCommit=${CGBI_QUICKJS_COMMIT}\n")
    string(APPEND CGBI_RECORD "txikiOverlayKey=${CGBI_OVERLAY_KEY}\n")
    string(APPEND CGBI_RECORD
        "txikiOverlayManifest=${CGBI_OVERLAY_MANIFEST}\n")
    string(APPEND CGBI_RECORD "bytecodeCompileFlags=${CGBI_COMPILE_FLAGS}\n")
    string(APPEND CGBI_RECORD "targetArchitecture=${CGBI_ARCHITECTURE}\n")
    string(APPEND CGBI_RECORD "endianness=${CGBI_ENDIANNESS}\n")
    string(APPEND CGBI_RECORD "pointerWidthBits=${CGBI_POINTER_WIDTH_BITS}\n")
    string(APPEND CGBI_RECORD
        "bytecodeFormatIdentity=${CGBI_BYTECODE_FORMAT}\n")
    string(APPEND CGBI_RECORD
        "capabilityManifestSha256=${CGBI_CAPABILITY_MANIFEST_SHA256}\n")

    # The ID is the SHA-256 of exactly the record bytes above.
    set(CGBI_RECORD_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/build-identity-record.txt")
    file(WRITE "${CGBI_RECORD_FILE}" "${CGBI_RECORD}")
    file(SHA256 "${CGBI_RECORD_FILE}" CGBI_DIGEST)
    set(CGBI_COMPATIBILITY_ID "sha256:${CGBI_DIGEST}")

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
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_identity.h.in"
        "${CGBI_OUT_HEADER}"
        @ONLY)

    set(CAPSID_BUILD_COMPATIBILITY_ID
        "${CGBI_COMPATIBILITY_ID}" PARENT_SCOPE)
    set(CAPSID_BUILD_CANONICAL_RECORD "${CGBI_RECORD}" PARENT_SCOPE)
endfunction()
