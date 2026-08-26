# Shared overlay key computation used by both the build (build_worker.cmake)
# and the audit (AuditTxikiVendor.cmake). The two must call the same function;
# duplicating the algorithm silently allows the audit to accept a different key
# than what the build stamped.
#
# Usage:
#   capsid_compute_txiki_overlay_key(
#       OUT_KEY           <var>
#       OUT_PATCHES       <var>
#       OUT_GIT_DEPENDENCIES <var>  # optional
#       VENDOR_DIR         <path>
#       PATCH_DIR           <path>
#       PREPARE_SCRIPT      <path>
#   )
#
# Canonical input format (one line per entry, stable sort):
#
#   schema=capsid-txiki-overlay-v1
#   vendor=<raw HEAD commit>
#   submodule=<commit> <path>
#   submodule=...
#   prepare=<sha256>
#   patch=0001-build-system.patch <sha256>
#   patch=...
#
# Rules:
#   1. submodule lines are sorted by path.
#   2. patch lines are sorted by filename.
#   3. Both the patch filename and its content hash enter the key.
#   4. The (tag/describe) suffix from `git submodule status` is stripped;
#      it varies with local tag state and is not a stable input.
#   5. A single SHA256 is computed over the entire canonical input; the
#      key is NOT built by hashing four sub-sections separately.

function(capsid_compute_txiki_overlay_key)
    set(OPTIONS "")
    set(ONE_VALUE_ARGS
        OUT_KEY
        OUT_PATCHES
        OUT_GIT_DEPENDENCIES
        VENDOR_DIR
        PATCH_DIR
        PREPARE_SCRIPT)
    set(MULTI_VALUE_ARGS "")
    cmake_parse_arguments(CTOK "${OPTIONS}" "${ONE_VALUE_ARGS}" "${MULTI_VALUE_ARGS}" ${ARGN})

    foreach(CTOK_REQUIRED IN ITEMS OUT_KEY OUT_PATCHES VENDOR_DIR PATCH_DIR PREPARE_SCRIPT)
        if(NOT DEFINED CTOK_${CTOK_REQUIRED})
            message(FATAL_ERROR "capsid_compute_txiki_overlay_key requires ${CTOK_REQUIRED}")
        endif()
    endforeach()

    find_program(CTOK_GIT git REQUIRED)

    # --- vendor revision -----------------------------------------------------
    execute_process(
        COMMAND "${CTOK_GIT}" -C "${CTOK_VENDOR_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE CTOK_VENDOR_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE CTOK_VENDOR_RESULT
    )
    if(NOT CTOK_VENDOR_RESULT EQUAL 0)
        message(FATAL_ERROR "Cannot resolve vendor HEAD revision in ${CTOK_VENDOR_DIR}")
    endif()

    # --- submodule status ----------------------------------------------------
    execute_process(
        COMMAND "${CTOK_GIT}" -C "${CTOK_VENDOR_DIR}"
            submodule status --recursive
        OUTPUT_VARIABLE CTOK_SUBMOD_RAW
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE CTOK_SUBMOD_RESULT
    )
    if(NOT CTOK_SUBMOD_RESULT EQUAL 0)
        message(FATAL_ERROR "Cannot read submodule status in ${CTOK_VENDOR_DIR}")
    endif()

    # Parse submodule lines, strip (tag/describe) suffix, sort by path.
    # The initialization status prefix (space/-/+/U) is NOT part of the key —
    # same gitlink on initialized vs uninitialized checkouts must produce the
    # same key.
    set(CTOK_SORTABLE_SUBMOD_LINES "")
    set(CTOK_SUBMOD_PATHS "")
    set(CTOK_BAD_SUBMOD_LINES "")
    if(CTOK_SUBMOD_RAW)
        string(REPLACE "\n" ";" CTOK_SUBMOD_ENTRIES "${CTOK_SUBMOD_RAW}")
        foreach(CTOK_ENTRY IN LISTS CTOK_SUBMOD_ENTRIES)
            # Git submodule status lines: <prefix><commit> <path> [(tag)]
            # Prefix is one of: ' ' (clean), '-' (uninitialized),
            # '+' (checked-out revision differs), 'U' (merge conflict).
            # The prefix is NOT part of the key — same gitlink across
            # initialized/uninitialized checkouts must match.
            if(CTOK_ENTRY MATCHES "^[ ]([0-9a-f]+) (.+)$")
                set(CTOK_PREFIX " ")
                set(CTOK_COMMIT "${CMAKE_MATCH_1}")
                set(CTOK_PATH "${CMAKE_MATCH_2}")
            elseif(CTOK_ENTRY MATCHES "^-([0-9a-f]+) (.+)$")
                set(CTOK_PREFIX "-")
                set(CTOK_COMMIT "${CMAKE_MATCH_1}")
                set(CTOK_PATH "${CMAKE_MATCH_2}")
            elseif(CTOK_ENTRY MATCHES "^\\+([0-9a-f]+) (.+)$")
                set(CTOK_PREFIX "+")
                set(CTOK_COMMIT "${CMAKE_MATCH_1}")
                set(CTOK_PATH "${CMAKE_MATCH_2}")
            elseif(CTOK_ENTRY MATCHES "^U([0-9a-f]+) (.+)$")
                set(CTOK_PREFIX "U")
                set(CTOK_COMMIT "${CMAKE_MATCH_1}")
                set(CTOK_PATH "${CMAKE_MATCH_2}")
            else()
                message(FATAL_ERROR
                    "Cannot parse submodule status line: ${CTOK_ENTRY}")
            endif()
            # Strip trailing (tag/describe) if present.
            string(REGEX REPLACE " [(].*[)]$" "" CTOK_PATH "${CTOK_PATH}")
            if(CTOK_PREFIX STREQUAL "+" OR CTOK_PREFIX STREQUAL "U" OR
               (CTOK_PREFIX STREQUAL "-" AND
                NOT CTOK_PATH STREQUAL "deps/quickjs/test262"))
                list(APPEND CTOK_BAD_SUBMOD_LINES "${CTOK_ENTRY}")
            endif()
            list(APPEND CTOK_SORTABLE_SUBMOD_LINES
                "${CTOK_PATH}|${CTOK_COMMIT}")
            list(APPEND CTOK_SUBMOD_PATHS "${CTOK_PATH}")
        endforeach()
    endif()
    if(CTOK_BAD_SUBMOD_LINES)
        string(REPLACE ";" "\n  " CTOK_BAD_SUBMOD_REPORT
            "${CTOK_BAD_SUBMOD_LINES}")
        message(FATAL_ERROR
            "txiki.js submodules are not at their recorded revisions:\n"
            "  ${CTOK_BAD_SUBMOD_REPORT}")
    endif()

    # Prefix with the path before sorting, then render the public
    # "<commit> <path>" form. Sorting that rendered form sorts by commit.
    list(SORT CTOK_SORTABLE_SUBMOD_LINES)
    set(CTOK_SUBMOD_LINES "")
    foreach(CTOK_SORTABLE IN LISTS CTOK_SORTABLE_SUBMOD_LINES)
        string(REGEX REPLACE "^([^|]+)[|](.+)$" "\\1" CTOK_PATH
            "${CTOK_SORTABLE}")
        string(REGEX REPLACE "^([^|]+)[|](.+)$" "\\2" CTOK_COMMIT
            "${CTOK_SORTABLE}")
        list(APPEND CTOK_SUBMOD_LINES "${CTOK_COMMIT} ${CTOK_PATH}")
    endforeach()

    # Refuse to fingerprint a dirty source tree. A clean revision plus clean
    # submodule revisions is the only state for which the key is a complete
    # description of the overlay inputs.
    execute_process(
        COMMAND "${CTOK_GIT}" -C "${CTOK_VENDOR_DIR}"
            status --porcelain=v1 --untracked-files=all
            --ignore-submodules=none
        OUTPUT_VARIABLE CTOK_VENDOR_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE CTOK_VENDOR_STATUS_ERROR
        RESULT_VARIABLE CTOK_VENDOR_STATUS_RESULT
    )
    if(NOT CTOK_VENDOR_STATUS_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Cannot inspect vendor status: ${CTOK_VENDOR_STATUS_ERROR}")
    endif()
    if(CTOK_VENDOR_STATUS)
        string(REPLACE "\n" "\n  " CTOK_VENDOR_STATUS_REPORT
            "${CTOK_VENDOR_STATUS}")
        message(FATAL_ERROR
            "txiki.js vendor checkout is dirty; the overlay key would be "
            "incomplete:\n  ${CTOK_VENDOR_STATUS_REPORT}")
    endif()

    # --- prepare script hash ------------------------------------------------
    capsid_sha256_normalized("${CTOK_PREPARE_SCRIPT}" CTOK_PREPARE_HASH)

    # --- patches -------------------------------------------------------------
    # The count is a sanity guard: adding or removing a patch file changes
    # the overlay key (each patch is hashed below), and the count ensures a
    # patch set change is noticed even before the hashes are compared.
    file(GLOB CTOK_PATCH_LIST "${CTOK_PATCH_DIR}/*.patch")
    list(SORT CTOK_PATCH_LIST)
    list(LENGTH CTOK_PATCH_LIST CTOK_PATCH_COUNT)
    # Binding v1: 0016 shared loop; 0017 raw egress; 0018 FS native gate;
    # 0019 Binding fs module gate; 0020 grantable module surface;
    # 0021 WASI FS/stdio native gates; 0022 SQLite gates;
    # 0023 fd-adoption gates; 0024 immutable native-resource owners;
    # 0025 side-effect-free Date slot access; 0029 WebSocket Binding gates;
    # 0030 fetch system-resolver pre-resolution (bypasses lws raw DNS);
    # 0031 fetch connection reuse (LCCSCF_PIPELINE, per-endpoint warm pool);
    # 0032 keepalive-evicted queued fetch requests fail closed with their
    #      user_space intact; 0033 only pool onto connections whose peer
    #      already proved keepalive; 0034 per-token pending-job probe;
    # 0035 correct libwebsockets Secure/HttpOnly cookie attribute mapping;
    # 0036 CONFIG_OPCODE_PROFILE opcode counters (measurement build only;
    #      compiled out of production builds — zero-tax verified at the
    #      object and linked-binary level for the OFF configuration).
    # 0037 backports quickjs-ng 377a25e:
    #      mixed int/float add/sub/mul/div and int-indexed fast-array reads
    #      stay inside the ordinary opcode handlers, with no BC format change.
    # 0038 adds exact-site/source-aware counters and stable shape identities
    #      to CONFIG_OPCODE_PROFILE; production builds compile the entire
    #      profiler out.
    # 0039 backports quickjs-ng b16e7bd: drop allocation-slack feedback and
    #      its redundant usable-size query; the BC26 format is unchanged.
    if(NOT CTOK_PATCH_COUNT EQUAL 40)
        message(FATAL_ERROR
            "expected 40 patches, found ${CTOK_PATCH_COUNT} in ${CTOK_PATCH_DIR}")
    endif()

    set(CTOK_PATCH_LINES "")
    foreach(CTOK_PATCH IN LISTS CTOK_PATCH_LIST)
        if(NOT EXISTS "${CTOK_PATCH}")
            message(FATAL_ERROR "Patch file is not readable: ${CTOK_PATCH}")
        endif()
        get_filename_component(CTOK_PATCH_NAME "${CTOK_PATCH}" NAME)
        capsid_sha256_normalized("${CTOK_PATCH}" CTOK_PATCH_HASH)
        list(APPEND CTOK_PATCH_LINES "patch=${CTOK_PATCH_NAME} ${CTOK_PATCH_HASH}")
    endforeach()

    # --- canonical input -----------------------------------------------------
    set(CTOK_CANONICAL_INPUT "schema=capsid-txiki-overlay-v1\n")
    string(APPEND CTOK_CANONICAL_INPUT "vendor=${CTOK_VENDOR_REVISION}\n")
    foreach(CTOK_LINE IN LISTS CTOK_SUBMOD_LINES)
        string(APPEND CTOK_CANONICAL_INPUT "submodule=${CTOK_LINE}\n")
    endforeach()
    string(APPEND CTOK_CANONICAL_INPUT "prepare=${CTOK_PREPARE_HASH}\n")
    foreach(CTOK_LINE IN LISTS CTOK_PATCH_LINES)
        string(APPEND CTOK_CANONICAL_INPUT "${CTOK_LINE}\n")
    endforeach()

    string(SHA256 CTOK_OVERLAY_KEY "${CTOK_CANONICAL_INPUT}")

    set("${CTOK_OUT_KEY}" "${CTOK_OVERLAY_KEY}" PARENT_SCOPE)
    set("${CTOK_OUT_PATCHES}" "${CTOK_PATCH_LIST}" PARENT_SCOPE)

    if(CTOK_OUT_GIT_DEPENDENCIES)
        set(CTOK_GIT_DEPENDENCIES "")
        set(CTOK_REPOSITORIES "${CTOK_VENDOR_DIR}")
        foreach(CTOK_SUBMOD_PATH IN LISTS CTOK_SUBMOD_PATHS)
            if(EXISTS "${CTOK_VENDOR_DIR}/${CTOK_SUBMOD_PATH}/.git")
                list(APPEND CTOK_GIT_DEPENDENCIES
                    "${CTOK_VENDOR_DIR}/${CTOK_SUBMOD_PATH}/.git")
                list(APPEND CTOK_REPOSITORIES
                    "${CTOK_VENDOR_DIR}/${CTOK_SUBMOD_PATH}")
            endif()
        endforeach()

        foreach(CTOK_REPOSITORY IN LISTS CTOK_REPOSITORIES)
            execute_process(
                COMMAND "${CTOK_GIT}" -C "${CTOK_REPOSITORY}"
                    rev-parse --absolute-git-dir
                OUTPUT_VARIABLE CTOK_GIT_DIR
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE CTOK_GIT_DIR_RESULT
            )
            if(NOT CTOK_GIT_DIR_RESULT EQUAL 0)
                message(FATAL_ERROR
                    "Cannot resolve git directory for ${CTOK_REPOSITORY}")
            endif()
            foreach(CTOK_GIT_STATE_NAME IN ITEMS HEAD index packed-refs)
                set(CTOK_GIT_STATE
                    "${CTOK_GIT_DIR}/${CTOK_GIT_STATE_NAME}")
                if(EXISTS "${CTOK_GIT_STATE}")
                    list(APPEND CTOK_GIT_DEPENDENCIES
                        "${CTOK_GIT_STATE}")
                endif()
            endforeach()

            set(CTOK_HEAD_FILE "${CTOK_GIT_DIR}/HEAD")
            if(EXISTS "${CTOK_HEAD_FILE}")
                file(READ "${CTOK_HEAD_FILE}" CTOK_HEAD_CONTENT)
                string(STRIP "${CTOK_HEAD_CONTENT}" CTOK_HEAD_CONTENT)
                if(CTOK_HEAD_CONTENT MATCHES "^ref: (.+)$")
                    set(CTOK_HEAD_REF
                        "${CTOK_GIT_DIR}/${CMAKE_MATCH_1}")
                    if(EXISTS "${CTOK_HEAD_REF}")
                        list(APPEND CTOK_GIT_DEPENDENCIES
                            "${CTOK_HEAD_REF}")
                    endif()
                endif()
            endif()
        endforeach()
        list(REMOVE_DUPLICATES CTOK_GIT_DEPENDENCIES)
        set("${CTOK_OUT_GIT_DEPENDENCIES}"
            "${CTOK_GIT_DEPENDENCIES}" PARENT_SCOPE)
    endif()
endfunction()

# Hash file content with CRLF normalized to LF. The key/manifest inputs
# (prepare script, patch stack, prepared overlay files) come out of a git
# checkout whose line endings follow the platform's autocrlf setting;
# without normalization the lock computed on Windows (CRLF) can never match
# the Linux CI checkout (LF) even though the content is identical.
function(capsid_sha256_normalized path out_var)
    file(READ "${path}" capsid_normalized_content)
    string(REPLACE "\r\n" "\n"
        capsid_normalized_content "${capsid_normalized_content}")
    string(SHA256 capsid_normalized_hash "${capsid_normalized_content}")
    set("${out_var}" "${capsid_normalized_hash}" PARENT_SCOPE)
endfunction()

# Hash the files whose final contents are produced by the patch stack.  This
# binds the stamp to a concrete prepared overlay instead of merely proving
# that somebody copied the right key into an arbitrary file.
function(capsid_compute_txiki_overlay_manifest)
    set(OPTIONS ALLOW_MISSING)
    set(ONE_VALUE_ARGS OUT_MANIFEST OUT_PATHS OVERLAY_DIR)
    set(MULTI_VALUE_ARGS PATCHES)
    cmake_parse_arguments(CTOM
        "${OPTIONS}" "${ONE_VALUE_ARGS}" "${MULTI_VALUE_ARGS}" ${ARGN})

    foreach(CTOM_REQUIRED IN ITEMS OUT_MANIFEST OVERLAY_DIR)
        if(NOT DEFINED CTOM_${CTOM_REQUIRED})
            message(FATAL_ERROR
                "capsid_compute_txiki_overlay_manifest requires "
                "${CTOM_REQUIRED}")
        endif()
    endforeach()
    if(NOT CTOM_PATCHES)
        message(FATAL_ERROR
            "capsid_compute_txiki_overlay_manifest requires PATCHES")
    endif()

    set(CTOM_PATHS "")
    foreach(CTOM_PATCH IN LISTS CTOM_PATCHES)
        file(STRINGS "${CTOM_PATCH}" CTOM_HEADERS
            REGEX "^\\+\\+\\+ b/")
        foreach(CTOM_HEADER IN LISTS CTOM_HEADERS)
            string(REGEX REPLACE
                "^\\+\\+\\+ b/([^ \t]+).*$" "\\1"
                CTOM_PATH "${CTOM_HEADER}")
            if(IS_ABSOLUTE "${CTOM_PATH}" OR
               CTOM_PATH MATCHES "(^|/)\\.\\.(/|$)")
                message(FATAL_ERROR
                    "unsafe path in txiki.js patch: ${CTOM_PATH}")
            endif()
            list(APPEND CTOM_PATHS "${CTOM_PATH}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES CTOM_PATHS)
    list(SORT CTOM_PATHS)
    if(NOT CTOM_PATHS)
        message(FATAL_ERROR
            "txiki.js patch stack does not contain output paths")
    endif()

    set(CTOM_CANONICAL
        "schema=capsid-txiki-overlay-manifest-v1\n")
    foreach(CTOM_PATH IN LISTS CTOM_PATHS)
        set(CTOM_FILE "${CTOM_OVERLAY_DIR}/${CTOM_PATH}")
        if(NOT EXISTS "${CTOM_FILE}" OR IS_DIRECTORY "${CTOM_FILE}")
            if(CTOM_ALLOW_MISSING)
                set("${CTOM_OUT_MANIFEST}" "" PARENT_SCOPE)
                if(CTOM_OUT_PATHS)
                    set("${CTOM_OUT_PATHS}" "${CTOM_PATHS}" PARENT_SCOPE)
                endif()
                return()
            endif()
            message(FATAL_ERROR
                "prepared txiki.js overlay is missing patched file: "
                "${CTOM_PATH}")
        endif()
        capsid_sha256_normalized("${CTOM_FILE}" CTOM_HASH)
        string(APPEND CTOM_CANONICAL
            "file=${CTOM_PATH} ${CTOM_HASH}\n")
    endforeach()
    string(SHA256 CTOM_MANIFEST "${CTOM_CANONICAL}")

    set("${CTOM_OUT_MANIFEST}" "${CTOM_MANIFEST}" PARENT_SCOPE)
    if(CTOM_OUT_PATHS)
        set("${CTOM_OUT_PATHS}" "${CTOM_PATHS}" PARENT_SCOPE)
    endif()
endfunction()
