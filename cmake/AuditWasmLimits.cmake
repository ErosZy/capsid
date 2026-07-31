# WebAssembly resource-limit constant audit.
#
# The resource caps are declared independently in C and JavaScript:
#
#   C  : TJS__WASM_MAX_MEMORY_PAGES and
#        TJS__WASM_MAX_TABLE_ELEMENTS in src/wasm.c
#   JS : CAPSID_WASM_MAX_MEMORY_PAGES and
#        CAPSID_WASM_MAX_TABLE_ELEMENTS in polyfills/wasm.js
#
# Each pair must agree. If a JS limit is higher, the wrapper accepts a descriptor
# that the native mirror clamps. If it is lower, otherwise admissible modules are
# rejected. Nothing at build time otherwise ties these declarations together.

if(NOT DEFINED CAPSID_WASM_C_SOURCE OR NOT EXISTS "${CAPSID_WASM_C_SOURCE}")
    message(FATAL_ERROR "CAPSID_WASM_C_SOURCE must name the existing src/wasm.c")
endif()
if(NOT DEFINED CAPSID_WASM_JS_SOURCE OR NOT EXISTS "${CAPSID_WASM_JS_SOURCE}")
    message(FATAL_ERROR
        "CAPSID_WASM_JS_SOURCE must name the existing polyfills/wasm.js")
endif()

# --- C side ----------------------------------------------------------------

file(STRINGS "${CAPSID_WASM_C_SOURCE}" CAPSID_C_DEFINE_LINES
    REGEX "^[ \t]*#define[ \t]+TJS__WASM_MAX_MEMORY_PAGES[ \t]+[0-9]+")
list(LENGTH CAPSID_C_DEFINE_LINES CAPSID_C_DEFINE_COUNT)
if(NOT CAPSID_C_DEFINE_COUNT EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one TJS__WASM_MAX_MEMORY_PAGES definition in "
        "${CAPSID_WASM_C_SOURCE}, found ${CAPSID_C_DEFINE_COUNT}.\n"
        "This audit cannot verify the limit it cannot uniquely locate.")
endif()
string(REGEX MATCH "[0-9]+$" CAPSID_C_MAX_PAGES "${CAPSID_C_DEFINE_LINES}")
if(NOT CAPSID_C_MAX_PAGES)
    message(FATAL_ERROR
        "could not parse TJS__WASM_MAX_MEMORY_PAGES from: ${CAPSID_C_DEFINE_LINES}")
endif()

file(STRINGS "${CAPSID_WASM_C_SOURCE}" CAPSID_C_TABLE_DEFINE_LINES
    REGEX "^[ \t]*#define[ \t]+TJS__WASM_MAX_TABLE_ELEMENTS[ \t]+[0-9]+")
list(LENGTH CAPSID_C_TABLE_DEFINE_LINES CAPSID_C_TABLE_DEFINE_COUNT)
if(NOT CAPSID_C_TABLE_DEFINE_COUNT EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one TJS__WASM_MAX_TABLE_ELEMENTS definition in "
        "${CAPSID_WASM_C_SOURCE}, found ${CAPSID_C_TABLE_DEFINE_COUNT}.\n"
        "This audit cannot verify the limit it cannot uniquely locate.")
endif()
string(REGEX MATCH "[0-9]+$" CAPSID_C_MAX_TABLE
    "${CAPSID_C_TABLE_DEFINE_LINES}")
if(NOT CAPSID_C_MAX_TABLE)
    message(FATAL_ERROR
        "could not parse TJS__WASM_MAX_TABLE_ELEMENTS from: "
        "${CAPSID_C_TABLE_DEFINE_LINES}")
endif()

# --- JS side ---------------------------------------------------------------

file(STRINGS "${CAPSID_WASM_JS_SOURCE}" CAPSID_JS_PAGE_LINES
    REGEX "^const CAPSID_WASM_MAX_MEMORY_PAGES = [0-9]+;")
list(LENGTH CAPSID_JS_PAGE_LINES CAPSID_JS_PAGE_COUNT)
if(NOT CAPSID_JS_PAGE_COUNT EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one CAPSID_WASM_MAX_MEMORY_PAGES declaration in "
        "${CAPSID_WASM_JS_SOURCE}, found ${CAPSID_JS_PAGE_COUNT}")
endif()
string(REGEX MATCH "[0-9]+" CAPSID_JS_MAX_PAGES "${CAPSID_JS_PAGE_LINES}")

file(STRINGS "${CAPSID_WASM_JS_SOURCE}" CAPSID_JS_TABLE_LINES
    REGEX "^const CAPSID_WASM_MAX_TABLE_ELEMENTS = [0-9]+;")
list(LENGTH CAPSID_JS_TABLE_LINES CAPSID_JS_TABLE_COUNT)
if(NOT CAPSID_JS_TABLE_COUNT EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one CAPSID_WASM_MAX_TABLE_ELEMENTS declaration in "
        "${CAPSID_WASM_JS_SOURCE}, found ${CAPSID_JS_TABLE_COUNT}")
endif()
string(REGEX MATCH "[0-9]+" CAPSID_JS_MAX_TABLE "${CAPSID_JS_TABLE_LINES}")

# --- agreement -------------------------------------------------------------

if(NOT CAPSID_C_MAX_PAGES EQUAL CAPSID_JS_MAX_PAGES)
    message(FATAL_ERROR
        "WebAssembly memory cap mismatch: C declares "
        "TJS__WASM_MAX_MEMORY_PAGES=${CAPSID_C_MAX_PAGES} but JS declares "
        "CAPSID_WASM_MAX_MEMORY_PAGES=${CAPSID_JS_MAX_PAGES}.\n"
        "A JS limit above the C limit lets WebAssembly.Memory accept a "
        "descriptor the runtime then clamps, returning less memory than "
        "requested. A JS limit below it rejects conforming modules. Keep both "
        "in sync, and update the grow/maximum cases in "
        "tests/fixtures/wasm-minimal.js to match.")
endif()
if(NOT CAPSID_C_MAX_TABLE EQUAL CAPSID_JS_MAX_TABLE)
    message(FATAL_ERROR
        "WebAssembly table cap mismatch: C declares "
        "TJS__WASM_MAX_TABLE_ELEMENTS=${CAPSID_C_MAX_TABLE} but JS declares "
        "CAPSID_WASM_MAX_TABLE_ELEMENTS=${CAPSID_JS_MAX_TABLE}.\n"
        "The native imported-Table mirror and JavaScript wrapper must enforce "
        "the same limit.")
endif()

# The fixture pins the boundary cases at cap+1; a cap below 2 pages leaves no
# room for the grow-to-cap assertions and would make those checks vacuous.
if(CAPSID_C_MAX_PAGES LESS 2)
    message(FATAL_ERROR
        "WebAssembly memory cap of ${CAPSID_C_MAX_PAGES} page(s) is too small for "
        "the grow-to-cap assertions in tests/fixtures/wasm-minimal.js")
endif()
if(CAPSID_C_MAX_TABLE LESS 2)
    message(FATAL_ERROR
        "WebAssembly table cap of ${CAPSID_C_MAX_TABLE} element(s) is too small "
        "for the grow-to-cap assertions in tests/fixtures/wasm-minimal.js")
endif()

# --- fixture pins the same numbers -----------------------------------------
#
# The boundary tests hardcode cap+1. If the cap moves and the fixture does not,
# the limit tests stop testing the boundary.

if(DEFINED CAPSID_WASM_FIXTURE_SOURCE AND EXISTS "${CAPSID_WASM_FIXTURE_SOURCE}")
    math(EXPR CAPSID_PAGES_OVER "${CAPSID_C_MAX_PAGES} + 1")
    math(EXPR CAPSID_TABLE_OVER "${CAPSID_C_MAX_TABLE} + 1")

    file(READ "${CAPSID_WASM_FIXTURE_SOURCE}" CAPSID_FIXTURE_TEXT)

    # The fixture mirrors the caps as named constants. Verify those mirrors carry
    # the same values as the sources of truth above; the boundary cases are then
    # expressed as `MAX_* + 1` and stay correct automatically.
    file(STRINGS "${CAPSID_WASM_FIXTURE_SOURCE}" CAPSID_FIXTURE_PAGE_LINES
        REGEX "^const MAX_MEMORY_PAGES = [0-9]+;")
    string(REGEX MATCH "[0-9]+" CAPSID_FIXTURE_MAX_PAGES "${CAPSID_FIXTURE_PAGE_LINES}")
    if(NOT CAPSID_FIXTURE_MAX_PAGES)
        message(FATAL_ERROR
            "tests/fixtures/wasm-minimal.js must declare "
            "'const MAX_MEMORY_PAGES = <n>;' so the cap boundary can be audited")
    endif()
    if(NOT CAPSID_FIXTURE_MAX_PAGES EQUAL CAPSID_C_MAX_PAGES)
        message(FATAL_ERROR
            "wasm fixture memory cap mirror is stale: fixture declares "
            "MAX_MEMORY_PAGES=${CAPSID_FIXTURE_MAX_PAGES} but the runtime cap is "
            "${CAPSID_C_MAX_PAGES}.\n"
            "The memory-resource-limit assertions would no longer test the "
            "boundary they were written for.")
    endif()

    file(STRINGS "${CAPSID_WASM_FIXTURE_SOURCE}" CAPSID_FIXTURE_TABLE_LINES
        REGEX "^const MAX_TABLE_ELEMENTS = [0-9]+;")
    string(REGEX MATCH "[0-9]+" CAPSID_FIXTURE_MAX_TABLE
        "${CAPSID_FIXTURE_TABLE_LINES}")
    if(NOT CAPSID_FIXTURE_MAX_TABLE)
        message(FATAL_ERROR
            "tests/fixtures/wasm-minimal.js must declare "
            "'const MAX_TABLE_ELEMENTS = <n>;' so the cap boundary can be audited")
    endif()
    if(NOT CAPSID_FIXTURE_MAX_TABLE EQUAL CAPSID_C_MAX_TABLE)
        message(FATAL_ERROR
            "wasm fixture table cap mirror is stale: fixture declares "
            "MAX_TABLE_ELEMENTS=${CAPSID_FIXTURE_MAX_TABLE} but the runtime cap is "
            "${CAPSID_C_MAX_TABLE}.")
    endif()

    # And that the boundary is actually expressed, not merely mirrored.
    foreach(CAPSID_BOUNDARY_EXPR
            "MAX_MEMORY_PAGES + 1"
            "MAX_TABLE_ELEMENTS + 1")
        string(FIND "${CAPSID_FIXTURE_TEXT}" "${CAPSID_BOUNDARY_EXPR}" CAPSID_POS)
        if(CAPSID_POS EQUAL -1)
            message(FATAL_ERROR
                "tests/fixtures/wasm-minimal.js no longer exercises the cap "
                "boundary: expected an assertion using '${CAPSID_BOUNDARY_EXPR}'")
        endif()
    endforeach()
endif()

message(STATUS
    "WebAssembly limits consistent: memory cap ${CAPSID_C_MAX_PAGES} pages "
    "and table cap ${CAPSID_C_MAX_TABLE} elements "
    "(C, JS, and fixture agree)")
