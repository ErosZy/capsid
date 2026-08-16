# Restricted-worker binary audit.
#
# This is a fail-closed security gate: it must be impossible for this script to
# report success without having actually inspected the artifacts. Every probe
# validates that it produced usable evidence before drawing a conclusion from it.
# A silent no-op (stripped binary, empty symbol table, unreadable strings) is an
# audit FAILURE, not a pass.
#
# Two independent layers are checked:
#
#   1. Reachability   -- the dangerous module initialisers must not be reachable
#                        from the restricted bootstrap. Under LTO the compiler
#                        renames/inlines statics, so symbol names alone are not a
#                        reliable signal; per-translation-unit LTO markers
#                        (`mod_fs.c.<hash>`) are used instead when present.
#   2. Module loader  -- even a linked-but-unregistered module still needs its
#                        `tjs:*` specifier as a string literal for the loader to
#                        reach it. Absence of the string is corroborating
#                        evidence that the capability is unreachable from JS.
#
# NOTE on scope: the restricted build compiles the full txiki.js source set and
# gates registration with TJS_RESTRICTED_CORE. Dangerous objects therefore DO
# exist in libtjs_core.a; what must hold is that they are not linked into (or not
# reachable within) the final worker. The archive is audited for integrity only.

if(NOT DEFINED CAPSID_WORKER OR NOT EXISTS "${CAPSID_WORKER}")
    message(FATAL_ERROR "CAPSID_WORKER must name an existing worker executable")
endif()
if(NOT DEFINED CAPSID_TJS_ARCHIVE OR NOT EXISTS "${CAPSID_TJS_ARCHIVE}")
    message(FATAL_ERROR "CAPSID_TJS_ARCHIVE must name the restricted tjs archive")
endif()
if(NOT DEFINED CAPSID_NM OR NOT DEFINED CAPSID_AR)
    message(FATAL_ERROR "CAPSID_NM and CAPSID_AR are required")
endif()
if(NOT DEFINED CAPSID_EXPECT_LTO)
    message(FATAL_ERROR
        "CAPSID_EXPECT_LTO must explicitly describe the audited build")
endif()

# ---------------------------------------------------------------------------
# Collect the symbol table.
#
# `nm` exits 0 with empty stdout on a stripped binary, which would make every
# forbidden-symbol check trivially pass. Read both the regular and the dynamic
# table, then prove the result is usable before relying on it.
# ---------------------------------------------------------------------------

set(CAPSID_SYMBOLS "")
foreach(CAPSID_NM_MODE "" "-D")
    execute_process(
        COMMAND "${CAPSID_NM}" ${CAPSID_NM_MODE} "${CAPSID_WORKER}"
        RESULT_VARIABLE CAPSID_NM_RESULT
        OUTPUT_VARIABLE CAPSID_NM_OUTPUT
        ERROR_VARIABLE CAPSID_NM_ERROR
    )
    if(CAPSID_NM_RESULT EQUAL 0)
        string(APPEND CAPSID_SYMBOLS "${CAPSID_NM_OUTPUT}")
    endif()
endforeach()

string(LENGTH "${CAPSID_SYMBOLS}" CAPSID_SYMBOLS_LENGTH)

# Gate 1 -- the haystack must be substantial. An empty or near-empty symbol table
# makes every string(FIND) below return "clean" for the wrong reason.
if(CAPSID_SYMBOLS_LENGTH LESS 4096)
    message(FATAL_ERROR
        "restricted worker symbol audit cannot run: ${CAPSID_NM} produced only "
        "${CAPSID_SYMBOLS_LENGTH} bytes of symbol data for ${CAPSID_WORKER}.\n"
        "A stripped binary makes the forbidden-symbol checks vacuous, so this is "
        "treated as an audit failure. Audit the pre-strip artifact, or build the "
        "audited worker with BUILD_WITH_STRIP=OFF.")
endif()

# Gate 2 -- positive control. These symbols are part of the embedding layer, are
# never static, and survive both LTO and non-LTO builds. If they are missing, the
# symbol table is not the one we think it is (wrong file, unexpected nm format)
# and every negative result below would be meaningless.
set(CAPSID_REQUIRED_SYMBOLS
    capsid_run_worker
    tjs_module_loader
)
foreach(CAPSID_REQUIRED_SYMBOL IN LISTS CAPSID_REQUIRED_SYMBOLS)
    string(FIND "${CAPSID_SYMBOLS}" "${CAPSID_REQUIRED_SYMBOL}" CAPSID_POSITION)
    if(CAPSID_POSITION EQUAL -1)
        message(FATAL_ERROR
            "restricted worker symbol audit is not trustworthy: required symbol "
            "'${CAPSID_REQUIRED_SYMBOL}' was not found in the symbol table of "
            "${CAPSID_WORKER}.\n"
            "The forbidden-symbol checks would report a false pass, so this is "
            "treated as an audit failure.")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Layer 1a -- forbidden module initialisers by symbol name.
#
# Effective on non-LTO builds, where these non-static symbols survive verbatim.
# On LTO builds the names are gone entirely; layer 1b covers that case.
# ---------------------------------------------------------------------------

set(CAPSID_FORBIDDEN_SYMBOLS
    tjs__mod_ffi_init
    tjs__mod_httpserver_init
    tjs__mod_posix_socket_init
    tjs__mod_process_init
    tjs__mod_signals_init
    tjs__mod_worker_init
    tjs__worker_post_error
)
foreach(CAPSID_FORBIDDEN_SYMBOL IN LISTS CAPSID_FORBIDDEN_SYMBOLS)
    string(FIND "${CAPSID_SYMBOLS}" "${CAPSID_FORBIDDEN_SYMBOL}" CAPSID_SYMBOL_POSITION)
    if(NOT CAPSID_SYMBOL_POSITION EQUAL -1)
        message(FATAL_ERROR
            "forbidden symbol is present in worker: ${CAPSID_FORBIDDEN_SYMBOL}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Layer 1b -- forbidden translation units by LTO marker.
#
# GCC/Clang LTO emit a per-translation-unit marker symbol of the form
# `<basename>.c.<hash>` for every compilation unit that contributes code to the
# final image. This survives the renaming that erases tjs__mod_* names, so it is
# the reliable reachability signal on LTO builds.
#
# The configured build mode is explicit. In an expected LTO artifact, absence
# of all supported markers is a hard failure rather than a silent downgrade to
# the weaker non-LTO symbol-name check.
# ---------------------------------------------------------------------------

string(REGEX MATCHALL "[A-Za-z0-9_-]+\\.c\\.[0-9a-f]+" CAPSID_TU_MARKERS "${CAPSID_SYMBOLS}")
list(REMOVE_DUPLICATES CAPSID_TU_MARKERS)
list(LENGTH CAPSID_TU_MARKERS CAPSID_TU_MARKER_COUNT)

set(CAPSID_FORBIDDEN_UNITS
    mod_ffi
    mod_posix-socket
    mod_process
    httpserver
    signals
    worker
)

if(CAPSID_EXPECT_LTO AND CAPSID_TU_MARKER_COUNT EQUAL 0)
    message(FATAL_ERROR
        "restricted worker audit expected an LTO artifact, but no supported "
        "translation-unit markers were found. Treating this as non-LTO would "
        "make the forbidden-unit checks vacuous on an unfamiliar toolchain.")
endif()

if(CAPSID_TU_MARKER_COUNT GREATER 0)
    foreach(CAPSID_FORBIDDEN_UNIT IN LISTS CAPSID_FORBIDDEN_UNITS)
        foreach(CAPSID_TU_MARKER IN LISTS CAPSID_TU_MARKERS)
            if(CAPSID_TU_MARKER MATCHES "^${CAPSID_FORBIDDEN_UNIT}\\.c\\.")
                message(FATAL_ERROR
                    "forbidden translation unit is linked into the worker: "
                    "${CAPSID_FORBIDDEN_UNIT}.c (LTO marker ${CAPSID_TU_MARKER})")
            endif()
        endforeach()
    endforeach()
    set(CAPSID_LAYER1B_STATUS
        "${CAPSID_TU_MARKER_COUNT} LTO translation-unit markers checked")
else()
    # Non-LTO build. Layer 1a already covered these units by symbol name, and the
    # positive control above proved symbol names are present and meaningful.
    set(CAPSID_LAYER1B_STATUS
        "no LTO markers present (non-LTO build); covered by symbol-name layer")
endif()

# ---------------------------------------------------------------------------
# Archive integrity.
#
# The restricted build compiles the full source set, so dangerous objects are
# expected here. Only verify the archive is real and complete, so that the
# link-time evidence above is meaningful.
# ---------------------------------------------------------------------------

execute_process(
    COMMAND "${CAPSID_AR}" -t "${CAPSID_TJS_ARCHIVE}"
    RESULT_VARIABLE CAPSID_AR_RESULT
    OUTPUT_VARIABLE CAPSID_ARCHIVE_MEMBERS
    ERROR_VARIABLE CAPSID_AR_ERROR
)
if(NOT CAPSID_AR_RESULT EQUAL 0)
    message(FATAL_ERROR "ar failed: ${CAPSID_AR_ERROR}")
endif()
if(NOT CAPSID_ARCHIVE_MEMBERS)
    message(FATAL_ERROR "restricted tjs archive is empty")
endif()
foreach(CAPSID_REQUIRED_OBJECT
        builtins.c.o
        vm.c.o
        modules.c.o
        timers.c.o)
    string(FIND "${CAPSID_ARCHIVE_MEMBERS}" "${CAPSID_REQUIRED_OBJECT}" CAPSID_POSITION)
    if(CAPSID_POSITION EQUAL -1)
        message(FATAL_ERROR
            "restricted tjs archive audit is not trustworthy: required member "
            "'${CAPSID_REQUIRED_OBJECT}' is absent from ${CAPSID_TJS_ARCHIVE}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Layer 2 -- module-loader specifiers.
# ---------------------------------------------------------------------------

# Gate 3 -- positive controls for file(STRINGS). Every built utility module and
# its two fixed internal dependencies must be present. This proves the audit is
# inspecting the intended binary and prevents accidental dead-stripping of the
# reviewed utility surface.
set(CAPSID_REQUIRED_MODULE_NAMES
    "capsid:assert"
    "capsid:env"
    "capsid:fs"
    "capsid:getopts"
    "capsid:hashing"
    "capsid:ipaddr"
    "capsid:permissions"
    "capsid:stdio"
    "capsid:storage"
    "capsid:system"
    "capsid:utils"
    "capsid:uuid"
    "tjs:assert"
    "tjs:getopts"
    "tjs:hashing"
    "tjs:internal/core"
    "tjs:internal/path"
    "tjs:ipaddr"
    "tjs:path"
    "tjs:readline"
    "tjs:sqlite"
    "tjs:utils"
    "tjs:uuid"
    "tjs:wasi"
)
foreach(CAPSID_REQUIRED_MODULE_NAME IN LISTS CAPSID_REQUIRED_MODULE_NAMES)
    file(STRINGS "${CAPSID_WORKER}" CAPSID_EXPECTED_MODULE_NAME
        REGEX "^${CAPSID_REQUIRED_MODULE_NAME}$"
        LIMIT_COUNT 1
    )
    if(NOT CAPSID_EXPECTED_MODULE_NAME)
        message(FATAL_ERROR
            "restricted worker string audit is not trustworthy: expected "
            "module '${CAPSID_REQUIRED_MODULE_NAME}' was not found in "
            "${CAPSID_WORKER}")
    endif()
endforeach()

file(STRINGS "${CAPSID_WORKER}" CAPSID_FORBIDDEN_MODULE_NAMES
    REGEX "tjs:(process|worker|ffi|posix-socket)|tjs:internal/(process|worker|httpserver|posix)"
    LIMIT_COUNT 1
)
if(CAPSID_FORBIDDEN_MODULE_NAMES)
    message(FATAL_ERROR
        "forbidden txiki.js module name remains in worker: "
        "${CAPSID_FORBIDDEN_MODULE_NAMES}")
endif()

file(SIZE "${CAPSID_WORKER}" CAPSID_WORKER_SIZE)
list(LENGTH CAPSID_FORBIDDEN_SYMBOLS CAPSID_FORBIDDEN_SYMBOL_COUNT)
list(LENGTH CAPSID_FORBIDDEN_UNITS CAPSID_FORBIDDEN_UNIT_COUNT)
list(LENGTH CAPSID_REQUIRED_MODULE_NAMES CAPSID_REQUIRED_MODULE_COUNT)
message(STATUS
    "restricted worker audit PASSED\n"
    "  binary:            ${CAPSID_WORKER_SIZE} bytes\n"
    "  symbol data:       ${CAPSID_SYMBOLS_LENGTH} bytes inspected\n"
    "  positive controls: 2 required symbols + 4 archive members + "
        "${CAPSID_REQUIRED_MODULE_COUNT} module specifiers\n"
    "  layer 1a:          ${CAPSID_FORBIDDEN_SYMBOL_COUNT} forbidden symbols absent\n"
    "  layer 1b:          ${CAPSID_LAYER1B_STATUS}\n"
    "  layer 2:           ${CAPSID_FORBIDDEN_UNIT_COUNT} forbidden units, no loader specifiers present")
