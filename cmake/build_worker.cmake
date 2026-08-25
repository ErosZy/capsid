# The bytecode fuzzer must compile against the exact pinned QuickJS opcode
# schema even when the worker itself is disabled. Preparing the overlay here
# keeps quickjs-opcode.h single-sourced for the product rewriter.
if(CAPSID_BUILD_WORKER OR CAPSID_BUILD_FUZZERS)
    set(CAPSID_TXIKI_OVERLAY "${CMAKE_CURRENT_BINARY_DIR}/vendor-overlay/txiki.js")
    set(CAPSID_OVERLAY_STAMP "${CAPSID_TXIKI_OVERLAY}/.capsid-overlay-key")

    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ComputeTxikiOverlayKey.cmake")
    capsid_compute_txiki_overlay_key(
        OUT_KEY CAPSID_OVERLAY_KEY
        OUT_PATCHES CAPSID_TXIKI_PATCHES
        OUT_GIT_DEPENDENCIES CAPSID_TXIKI_GIT_DEPENDENCIES
        VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js"
        PATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/patches/txiki"
        PREPARE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareTxiki.cmake"
    )

    set(CAPSID_EXISTING_OVERLAY_KEY "")
    set(CAPSID_EXISTING_OVERLAY_MANIFEST "")
    set(CAPSID_OVERLAY_STAMP_VALID FALSE)
    if(EXISTS "${CAPSID_OVERLAY_STAMP}")
        file(STRINGS "${CAPSID_OVERLAY_STAMP}" CAPSID_STAMP_LINES)
        list(LENGTH CAPSID_STAMP_LINES CAPSID_STAMP_LINE_COUNT)
        if(CAPSID_STAMP_LINE_COUNT EQUAL 3)
            list(GET CAPSID_STAMP_LINES 0 CAPSID_STAMP_SCHEMA)
            list(GET CAPSID_STAMP_LINES 1 CAPSID_STAMP_KEY_LINE)
            list(GET CAPSID_STAMP_LINES 2 CAPSID_STAMP_MANIFEST_LINE)
            if(CAPSID_STAMP_SCHEMA STREQUAL
                    "schema=capsid-txiki-overlay-stamp-v1" AND
               CAPSID_STAMP_KEY_LINE MATCHES "^key=[0-9a-f]+$" AND
               CAPSID_STAMP_MANIFEST_LINE MATCHES
                    "^manifest=[0-9a-f]+$")
                string(REGEX REPLACE "^key=" ""
                    CAPSID_EXISTING_OVERLAY_KEY
                    "${CAPSID_STAMP_KEY_LINE}")
                string(REGEX REPLACE "^manifest=" ""
                    CAPSID_EXISTING_OVERLAY_MANIFEST
                    "${CAPSID_STAMP_MANIFEST_LINE}")
                string(LENGTH "${CAPSID_EXISTING_OVERLAY_KEY}"
                    CAPSID_STAMP_KEY_LENGTH)
                string(LENGTH "${CAPSID_EXISTING_OVERLAY_MANIFEST}"
                    CAPSID_STAMP_MANIFEST_LENGTH)
                if(CAPSID_STAMP_KEY_LENGTH EQUAL 64 AND
                   CAPSID_STAMP_MANIFEST_LENGTH EQUAL 64)
                    set(CAPSID_OVERLAY_STAMP_VALID TRUE)
                endif()
            endif()
        endif()
    endif()

    set(CAPSID_OVERLAY_IS_CURRENT FALSE)
    if(CAPSID_OVERLAY_STAMP_VALID AND
       CAPSID_EXISTING_OVERLAY_KEY STREQUAL CAPSID_OVERLAY_KEY)
        capsid_compute_txiki_overlay_manifest(
            OUT_MANIFEST CAPSID_CURRENT_OVERLAY_MANIFEST
            OVERLAY_DIR "${CAPSID_TXIKI_OVERLAY}"
            PATCHES ${CAPSID_TXIKI_PATCHES}
            ALLOW_MISSING
        )
        if(CAPSID_CURRENT_OVERLAY_MANIFEST STREQUAL
           CAPSID_EXISTING_OVERLAY_MANIFEST)
            set(CAPSID_OVERLAY_IS_CURRENT TRUE)
        endif()
    endif()

    if(NOT CAPSID_OVERLAY_IS_CURRENT)
        # Remove stale overlay before rebuilding.
        file(REMOVE_RECURSE "${CAPSID_TXIKI_OVERLAY}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_VENDOR_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js"
                "-DCAPSID_VENDOR_OVERLAY=${CAPSID_TXIKI_OVERLAY}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareTxiki.cmake"
            RESULT_VARIABLE CAPSID_PREPARE_RESULT
        )
        if(NOT CAPSID_PREPARE_RESULT EQUAL 0)
            message(FATAL_ERROR "Could not prepare the txiki.js build overlay")
        endif()
        capsid_compute_txiki_overlay_manifest(
            OUT_MANIFEST CAPSID_OVERLAY_MANIFEST
            OVERLAY_DIR "${CAPSID_TXIKI_OVERLAY}"
            PATCHES ${CAPSID_TXIKI_PATCHES}
        )
        # Write stamp via temp file then rename — avoids a truncated stamp
        # if configure is interrupted mid-write.
        set(CAPSID_STAMP_TMP "${CAPSID_OVERLAY_STAMP}.tmp")
        file(WRITE "${CAPSID_STAMP_TMP}"
            "schema=capsid-txiki-overlay-stamp-v1\n"
            "key=${CAPSID_OVERLAY_KEY}\n"
            "manifest=${CAPSID_OVERLAY_MANIFEST}\n")
        file(RENAME "${CAPSID_STAMP_TMP}" "${CAPSID_OVERLAY_STAMP}")
    endif()

    # Re-read the freshly globbed patch list from the key function. Use
    # CONFIGURE_DEPENDS on the patch glob for addition/removal detection.
    file(GLOB CAPSID_TXIKI_PATCHES_GLOB CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/txiki/*.patch")
    file(GLOB_RECURSE CAPSID_TXIKI_COPIED_VENDOR_INPUTS
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES FALSE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js/src/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js/tests/fixtures/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js/deps/wamr/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js/deps/libwebsockets/include/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js/deps/libwebsockets/lib/*"
    )

    # Use APPEND so this does not overwrite the CMAKE_CONFIGURE_DEPENDS entries
    # already set by the parent CMakeLists.txt (e.g. docs/capability-manifest.json).
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareTxiki.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ComputeTxikiOverlayKey.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js/CMakeLists.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js/LICENSE"
        ${CAPSID_TXIKI_PATCHES}
        ${CAPSID_TXIKI_GIT_DEPENDENCIES}
        ${CAPSID_TXIKI_COPIED_VENDOR_INPUTS}
    )

endif()

if(CAPSID_BUILD_WORKER)
    set(BUILD_WITH_FFI OFF CACHE BOOL "" FORCE)
    # Binding v1 §3.3: tjs:sqlite is a grantable module in the restricted
    # build; its paths stay behind the per-origin FS gate (patch 0018).
    set(BUILD_WITH_SQLITE ON CACHE BOOL "" FORCE)
    set(BUILD_WITH_MIMALLOC ${CAPSID_USE_MIMALLOC} CACHE BOOL "" FORCE)
    # txiki's QuickJS allocator (vendor/txiki.js/src/mem.c) calls mi_*
    # directly, so the heap benefit does not need mimalloc's global malloc
    # override. Keeping the override off avoids its ~4GiB virtual reserve
    # colliding with the worker's address-space limit (std::bad_alloc in
    # static builds) and leaves libuv/libstdc++ on the system malloc.
    set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
    set(BUILD_WITH_ASAN OFF CACHE BOOL "" FORCE)
    set(BUILD_WITH_UBSAN OFF CACHE BOOL "" FORCE)
    set(BUILD_WITH_WASM ON CACHE BOOL "" FORCE)
    set(BUILD_WITH_LTO ${CAPSID_ENABLE_LTO} CACHE BOOL "" FORCE)
    set(BUILD_WITH_GC_SECTIONS ON CACHE BOOL "" FORCE)
    set(BUILD_TJS_CLI OFF CACHE BOOL "" FORCE)
    set(BUILD_TJS_TEST_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TJS_RESTRICTED_CORE ON CACHE BOOL "" FORCE)
    set(BUILD_TJS_BENCHMARK_SQLITE OFF CACHE BOOL "" FORCE)
    # A1: forward the profiling option into the quickjs xoption. The
    # overlay key/manifest do not depend on this cache value — the
    # compiled code does, so production builds keep it OFF.
    set(CONFIG_OPCODE_PROFILE ${CAPSID_ENABLE_OPCODE_PROFILE}
        CACHE BOOL "" FORCE)
    add_subdirectory("${CAPSID_TXIKI_OVERLAY}" "${CMAKE_CURRENT_BINARY_DIR}/txiki-build" EXCLUDE_FROM_ALL)
    if(WIN32)
        # Windows has no system iconv; the vendored win-iconv (public
        # domain, MultiByteToWideChar-backed) provides the same API the
        # restricted core's TextDecoder path uses (see vendor/win-iconv).
        if(NOT TARGET Iconv::Iconv)
            add_library(capsid_winiconv STATIC
                "${CMAKE_CURRENT_SOURCE_DIR}/vendor/win-iconv/win_iconv.c")
            target_include_directories(capsid_winiconv PUBLIC
                "${CMAKE_CURRENT_SOURCE_DIR}/vendor/win-iconv")
            add_library(Iconv::Iconv ALIAS capsid_winiconv)
        endif()
    else()
        find_package(Iconv REQUIRED)
    endif()
    target_sources(tjs PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/txiki_restricted_core.c"
    )
    target_link_libraries(tjs PUBLIC Iconv::Iconv)
    # The vendored txiki build compiles with its own -Werror. glibc marks
    # write() as warn_unused_result, and the vendor's fatal-path writes are
    # intentionally fire-and-forget (abort() follows immediately). Suppress
    # only this warning class for the vendored library; project code keeps
    # strict warnings. Vendor fixes should still go through patches/txiki
    # when the code itself is wrong. (MSVC warning categories do not map
    # onto the glibc warn_unused_result class; Windows toolchains warn
    # differently, matching txiki's own Unix-only -Werror stance.)
    if(NOT MSVC)
        target_compile_options(tjs PRIVATE -Wno-unused-result)
    endif()
    if(MSVC)
        # qjsc.c uses POSIX getopt (optarg/optind) with no portability
        # layer; the vendored shim (vendor/win32-shims) provides it for
        # the tjsc tool only, force-included into the translation unit.
        target_sources(tjsc PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/win32-shims/getopt.c")
        target_compile_options(tjsc PRIVATE
            "/FI${CMAKE_CURRENT_SOURCE_DIR}/vendor/win32-shims/getopt.h")
    endif()

    if(CAPSID_ESBUILD_EXECUTABLE)
        set(CAPSID_ESBUILD "${CAPSID_ESBUILD_EXECUTABLE}")
    elseif(WIN32)
        # npm's .bin/esbuild entry is a shell wrapper on Windows; call the
        # platform binary directly.
        set(CAPSID_ESBUILD
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js/node_modules/@esbuild/win32-x64/esbuild.exe")
    else()
        set(CAPSID_ESBUILD
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js/node_modules/.bin/esbuild")
    endif()
    if(NOT EXISTS "${CAPSID_ESBUILD}")
        message(FATAL_ERROR
            "esbuild is missing; run npm install --ignore-scripts --prefix vendor/txiki.js")
    endif()

    file(GLOB_RECURSE CAPSID_BOOTSTRAP_DEPS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/js/*.js"
        "${CAPSID_TXIKI_OVERLAY}/src/js/polyfills/*.js"
    )
    set(CAPSID_BOOTSTRAP_JS "${CAPSID_GENERATED_DIR}/bootstrap.js")
    set(CAPSID_BOOTSTRAP_C "${CAPSID_GENERATED_DIR}/bootstrap.c")

    add_custom_command(
        OUTPUT "${CAPSID_BOOTSTRAP_JS}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CAPSID_GENERATED_DIR}"
        COMMAND "${CAPSID_ESBUILD}"
            "${CMAKE_CURRENT_SOURCE_DIR}/js/bootstrap.js"
            --bundle
            --minify
            --keep-names
            --target=esnext
            --platform=neutral
            --format=esm
            --external:tjs:*
            "--alias:txiki-polyfills=${CAPSID_TXIKI_OVERLAY}/src/js/polyfills"
            "--alias:urlpattern-polyfill=${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js/node_modules/urlpattern-polyfill/index.js"
            "--outfile=${CAPSID_BOOTSTRAP_JS}"
        DEPENDS ${CAPSID_BOOTSTRAP_DEPS}
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${CAPSID_BOOTSTRAP_C}"
        COMMAND $<TARGET_FILE:tjsc>
            -m -s
            -o "${CAPSID_BOOTSTRAP_C}"
            -n "tjs:internal/capsid-bootstrap"
            -p capsid__
            "${CAPSID_BOOTSTRAP_JS}"
        DEPENDS tjsc "${CAPSID_BOOTSTRAP_JS}"
        VERBATIM
    )
    add_executable(capsid-worker
        src/binding_rpc.cc
        src/capability_policy.cc
        src/egress_policy.cc
        src/ipc_validation.cc
        src/protocol.cc
        src/response_headers.cc
        src/sandbox.cc
        src/worker_main.cc
        src/worker_runtime.cc
        "${CAPSID_BOOTSTRAP_C}"
    )
    if(CAPSID_ENABLE_LTO AND CAPSID_IPO_SUPPORTED)
        set_property(TARGET capsid-worker PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    target_include_directories(capsid-worker PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CAPSID_GENERATED_DIR}"
        "${CAPSID_TXIKI_OVERLAY}/src"
    )
    target_link_libraries(capsid-worker PRIVATE tjs capsid_sanitizers)
    target_compile_definitions(
        capsid-worker PRIVATE
        "CAPSID_RUNTIME_VERSION=\"${PROJECT_VERSION}\""
    )
    if(CAPSID_ENABLE_OPCODE_PROFILE)
        # A1: gate the teardown-time profile dump hook in worker_runtime.cc.
        # The quickjs CONFIG_OPCODE_PROFILE counters are enabled via the
        # forwarded cache value above; this define enables the capsid-side
        # observer that calls JS_DumpOpcodeProfile before each
        # TJS_FreeRuntime.
        target_compile_definitions(capsid-worker PRIVATE
            CAPSID_OPCODE_PROFILE=1)
    endif()
    if(CAPSID_GENERATE_LINK_MAP)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
           CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_link_options(
                capsid-worker PRIVATE
                "LINKER:-Map,${CAPSID_WORKER_LINK_MAP}")
        else()
            message(FATAL_ERROR
                "CAPSID_GENERATE_LINK_MAP currently supports GNU/Clang on Linux")
        endif()
    endif()
    set_target_properties(capsid-worker PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON
        OUTPUT_NAME capsid-worker
    )
    if(MSVC)
        # worker_runtime.cc uses std::optional (MSVC's C++14 mode has no
        # <optional>; GNU/Clang already compile this TU as >= C++17 because
        # their default standard exceeds the requested 11). Windows only —
        # the POSIX build surface stays unchanged.
        set_property(TARGET capsid-worker PROPERTY CXX_STANDARD 17)
    endif()
    if(CAPSID_STRICT_WARNINGS)
        if(MSVC)
            target_compile_options(capsid-worker PRIVATE /W4 /WX)
        else()
            target_compile_options(capsid-worker PRIVATE
                -Wall -Wextra -Wpedantic -Werror
                "$<$<COMPILE_LANGUAGE:CXX>:-Wno-c99-extensions>"
            )
        endif()
    endif()
    # First-party bytecode compiler (M1D-1): compiles module bytecode with
    # the same QuickJS the worker links (the txiki overlay's tjs), emits the
    # canonical attestation and the frozen binary signing message. It never
    # sees a private key; signing is the offline pipeline's job.
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
    if(NOT TARGET capsid_jansson)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/vendor/jansson")
    endif()
    add_executable(capsid-bytecode-compile
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/capsid-bytecode-compile.cc")
    target_include_directories(capsid-bytecode-compile PRIVATE
        "${CAPSID_GENERATED_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CAPSID_TXIKI_OVERLAY}/deps/quickjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/jansson/src")
    target_link_libraries(capsid-bytecode-compile PRIVATE
        tjs
        Iconv::Iconv
        capsid_jansson
        OpenSSL::Crypto
        capsid_sanitizers)
    set_target_properties(capsid-bytecode-compile PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    if(CAPSID_STRICT_WARNINGS)
        if(MSVC)
            target_compile_options(capsid-bytecode-compile PRIVATE /W4 /WX)
        else()
            target_compile_options(capsid-bytecode-compile PRIVATE
                -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()
    # Bytecode AOT rewriter (docs/bytecode-aot-rewriter.md): pure
    # capsid-side post-serialization rewrite of the .qjsb buffer. This is
    # product source consumed by the compiler, tests, benchmarks, and fuzzers;
    # it never changes the vendored VM or bytecode compatibility identity.
    add_library(capsid_bytecode_rewriter STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bytecode_rewriter/bytecode_rewriter.cc"
        # I0 bring-up CFG + I1 full-stack SSA (tier-3 plan): analyze-only
        # until the gates pass on the corpus; the production pipeline
        # never invokes them, and unmodeled functions stay byte-for-byte
        # BC26.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bytecode_rewriter/ir/cfg.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bytecode_rewriter/ir/effects.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bytecode_rewriter/ir/ssa.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bytecode_rewriter/ir/region.cc")
    target_include_directories(capsid_bytecode_rewriter
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
        PRIVATE
        "${CAPSID_TXIKI_OVERLAY}/deps/quickjs")
    set_target_properties(capsid_bytecode_rewriter PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    if(CAPSID_STRICT_WARNINGS)
        if(MSVC)
            target_compile_options(capsid_bytecode_rewriter PRIVATE /W4 /WX)
        else()
            target_compile_options(capsid_bytecode_rewriter PRIVATE
                -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()
    target_link_libraries(capsid-bytecode-compile PRIVATE
        capsid_bytecode_rewriter)
    add_dependencies(capsid_runtime capsid-worker)
endif()
