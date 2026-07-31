if(CAPSID_BUILD_BENCHMARKS)
    if(NOT CAPSID_BUILD_WORKER)
        message(FATAL_ERROR
            "CAPSID_BUILD_BENCHMARKS requires CAPSID_BUILD_WORKER=ON")
    endif()

    find_program(CAPSID_BENCHMARK_NODE NAMES node REQUIRED)
    find_program(CAPSID_BENCHMARK_GO NAMES go REQUIRED)
    find_program(CAPSID_BENCHMARK_PYTHON3 NAMES python3 REQUIRED)
    find_program(CAPSID_BENCHMARK_DENO NAMES deno)

    set(CAPSID_BENCHMARK_APPS
        "${CMAKE_CURRENT_SOURCE_DIR}/bench/apps")
    set(CAPSID_BENCHMARK_APP_OUTPUT
        "${CMAKE_CURRENT_BINARY_DIR}/bench/apps")
    set(CAPSID_BENCHMARK_BIN
        "${CMAKE_CURRENT_BINARY_DIR}/bench/bin")
    set(CAPSID_BENCHMARK_METADATA
        "${CMAKE_CURRENT_BINARY_DIR}/bench/build-metadata.json")

    set(CAPSID_BENCHMARK_PACKAGE_JSON
        "${CAPSID_BENCHMARK_APPS}/node_modules/hono/package.json")
    if(NOT EXISTS "${CAPSID_BENCHMARK_APPS}/package-lock.json")
        message(FATAL_ERROR
            "benchmark package lock is missing: bench/apps/package-lock.json")
    endif()
    if(NOT EXISTS "${CAPSID_BENCHMARK_PACKAGE_JSON}")
        message(FATAL_ERROR
            "hono@4.12.32 is missing; run "
            "npm ci --ignore-scripts --prefix bench/apps")
    endif()
    file(READ
        "${CAPSID_BENCHMARK_PACKAGE_JSON}"
        CAPSID_BENCHMARK_PACKAGE_CONTENT)
    string(REGEX MATCH
        "\"version\"[ \t\r\n]*:[ \t\r\n]*\"4\\.12\\.32\""
        CAPSID_BENCHMARK_VERSION_MATCH
        "${CAPSID_BENCHMARK_PACKAGE_CONTENT}")
    if(NOT CAPSID_BENCHMARK_VERSION_MATCH)
        message(FATAL_ERROR
            "bench/apps must contain exactly hono@4.12.32")
    endif()

    file(GLOB_RECURSE CAPSID_BENCHMARK_APP_DEPS CONFIGURE_DEPENDS
        "${CAPSID_BENCHMARK_APPS}/hono/src/*.ts"
        "${CAPSID_BENCHMARK_APPS}/shared/*.ts"
        "${CAPSID_BENCHMARK_APPS}/package.json"
        "${CAPSID_BENCHMARK_APPS}/package-lock.json"
        "${CAPSID_BENCHMARK_APPS}/node_modules/hono/package.json"
    )
    list(APPEND CAPSID_BENCHMARK_APP_DEPS
        "${CAPSID_BENCHMARK_APPS}/build-lib.mjs"
        "${CAPSID_BENCHMARK_APPS}/audit-bundle.mjs"
        "${CAPSID_BENCHMARK_APPS}/contract-test.mjs")

    set(CAPSID_BENCHMARK_BUNDLE
        "${CAPSID_BENCHMARK_APP_OUTPUT}/hono-core.mjs")
    set(CAPSID_BENCHMARK_METAFILE
        "${CAPSID_BENCHMARK_BUNDLE}.meta.json")
    add_custom_command(
        OUTPUT
            "${CAPSID_BENCHMARK_BUNDLE}"
            "${CAPSID_BENCHMARK_METAFILE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CAPSID_BENCHMARK_APP_OUTPUT}"
        COMMAND "${CAPSID_BENCHMARK_NODE}"
            "${CAPSID_BENCHMARK_APPS}/hono/build.mjs"
            --outfile "${CAPSID_BENCHMARK_BUNDLE}"
            --metafile "${CAPSID_BENCHMARK_METAFILE}"
            --esbuild "${CAPSID_ESBUILD}"
        COMMAND "${CAPSID_BENCHMARK_NODE}"
            "${CAPSID_BENCHMARK_APPS}/audit-bundle.mjs"
            "${CAPSID_BENCHMARK_BUNDLE}"
            "${CAPSID_BENCHMARK_METAFILE}"
        DEPENDS ${CAPSID_BENCHMARK_APP_DEPS}
        VERBATIM
    )
    add_custom_target(capsid-bench-app
        DEPENDS
            "${CAPSID_BENCHMARK_BUNDLE}"
            "${CAPSID_BENCHMARK_METAFILE}")

    if(CAPSID_BUILD_SQLITE_BENCHMARK)
        set(CAPSID_SQLITE_BENCHMARK_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/bench/sqlite")
        set(CAPSID_SQLITE_BENCHMARK_DATABASE
            "${CAPSID_SQLITE_BENCHMARK_DIR}/sqlite.db")
        set(CAPSID_SQLITE_BENCHMARK_MANIFEST
            "${CAPSID_SQLITE_BENCHMARK_DIR}/manifest.json")
        set(CAPSID_SQLITE_BENCHMARK_BUNDLE
            "${CAPSID_BENCHMARK_APP_OUTPUT}/hono-sqlite.mjs")
        set(CAPSID_SQLITE_BENCHMARK_METAFILE
            "${CAPSID_SQLITE_BENCHMARK_BUNDLE}.meta.json")
        add_custom_command(
            OUTPUT
                "${CAPSID_SQLITE_BENCHMARK_DATABASE}"
                "${CAPSID_SQLITE_BENCHMARK_MANIFEST}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${CAPSID_SQLITE_BENCHMARK_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E rm -f
                "${CAPSID_SQLITE_BENCHMARK_DATABASE}"
                "${CAPSID_SQLITE_BENCHMARK_MANIFEST}"
            COMMAND "${CAPSID_BENCHMARK_PYTHON3}"
                "${CMAKE_CURRENT_SOURCE_DIR}/bench/sqlite-compare/generate_db.py"
                --output "${CAPSID_SQLITE_BENCHMARK_DATABASE}"
                --manifest "${CAPSID_SQLITE_BENCHMARK_MANIFEST}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/bench/sqlite-compare/generate_db.py"
            VERBATIM
        )
        add_custom_command(
            OUTPUT
                "${CAPSID_SQLITE_BENCHMARK_BUNDLE}"
                "${CAPSID_SQLITE_BENCHMARK_METAFILE}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${CAPSID_BENCHMARK_APP_OUTPUT}"
            COMMAND "${CAPSID_BENCHMARK_NODE}"
                "${CAPSID_BENCHMARK_APPS}/hono-sqlite/build.mjs"
                --outfile "${CAPSID_SQLITE_BENCHMARK_BUNDLE}"
                --metafile "${CAPSID_SQLITE_BENCHMARK_METAFILE}"
                --esbuild "${CAPSID_ESBUILD}"
            COMMAND "${CAPSID_BENCHMARK_NODE}"
                "${CAPSID_BENCHMARK_APPS}/audit-sqlite-bundle.mjs"
                "${CAPSID_SQLITE_BENCHMARK_BUNDLE}"
                "${CAPSID_SQLITE_BENCHMARK_METAFILE}"
            DEPENDS
                ${CAPSID_BENCHMARK_APP_DEPS}
                "${CAPSID_BENCHMARK_APPS}/hono-sqlite/build.mjs"
                "${CAPSID_BENCHMARK_APPS}/hono-sqlite/src/app.ts"
                "${CAPSID_BENCHMARK_APPS}/audit-sqlite-bundle.mjs"
            VERBATIM
        )
        add_custom_target(capsid-sqlite-bench-assets
            DEPENDS
                "${CAPSID_SQLITE_BENCHMARK_DATABASE}"
                "${CAPSID_SQLITE_BENCHMARK_MANIFEST}"
                "${CAPSID_SQLITE_BENCHMARK_BUNDLE}"
                "${CAPSID_SQLITE_BENCHMARK_METAFILE}")
    endif()

    file(GLOB_RECURSE CAPSID_BENCHMARK_GO_DEPS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/bench/cmd/*.go"
        "${CMAKE_CURRENT_SOURCE_DIR}/bench/cmd/*.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/bench/loadgen/*.go"
        "${CMAKE_CURRENT_SOURCE_DIR}/bench/loadgen/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/bench/loadgen/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/bench/go.mod"
    )
    set(CAPSID_BENCHMARK_GO_CACHE
        "${CMAKE_CURRENT_BINARY_DIR}/bench/go-cache")
    set(CAPSID_BENCHMARK_GO_MODULE_CACHE
        "${CMAKE_CURRENT_BINARY_DIR}/bench/go-mod-cache")
    set(CAPSID_BENCHMARK_LOADGEN
        "${CAPSID_BENCHMARK_BIN}/capsid-loadgen")
    set(CAPSID_BENCHMARK_HTTP_GW
        "${CAPSID_BENCHMARK_BIN}/capsid-http-gw")
    set(CAPSID_BENCHMARK_IPC_PROFILE
        "${CAPSID_BENCHMARK_BIN}/capsid-ipc-profile")
    set(CAPSID_BENCHMARK_STARTUP
        "${CAPSID_BENCHMARK_BIN}/capsid-startup-bench")
    set(CAPSID_BENCHMARK_RESOURCE_BASELINE
        "${CAPSID_BENCHMARK_BIN}/capsid-resource-baseline")
    add_executable(
        capsid-bytecode-compile
        "${CMAKE_CURRENT_SOURCE_DIR}/bench/cmd/capsid-bytecode-compile/main.c")
    target_link_libraries(capsid-bytecode-compile PRIVATE qjs)
    set_target_properties(capsid-bytecode-compile PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CAPSID_BENCHMARK_BIN}")
    set(CAPSID_BENCHMARK_CGO_LDFLAGS
        "$<TARGET_FILE:capsid_runtime>")
    if(CAPSID_ENABLE_ASAN)
        string(APPEND CAPSID_BENCHMARK_CGO_LDFLAGS
            " -fsanitize=address")
    endif()
    if(CAPSID_ENABLE_UBSAN)
        string(APPEND CAPSID_BENCHMARK_CGO_LDFLAGS
            " -fsanitize=undefined")
    endif()
    add_custom_command(
        OUTPUT
            "${CAPSID_BENCHMARK_LOADGEN}"
            "${CAPSID_BENCHMARK_HTTP_GW}"
            "${CAPSID_BENCHMARK_IPC_PROFILE}"
            "${CAPSID_BENCHMARK_STARTUP}"
            "${CAPSID_BENCHMARK_RESOURCE_BASELINE}"
        # Go's action cache does not reliably key an external static archive's
        # contents. This command already reruns when capsid_runtime changes;
        # drop only its private build cache so the tools cannot retain an old
        # libcapsid_runtime.a link.
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${CAPSID_BENCHMARK_GO_CACHE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CAPSID_BENCHMARK_BIN}"
            "${CAPSID_BENCHMARK_GO_CACHE}"
            "${CAPSID_BENCHMARK_GO_MODULE_CACHE}"
        COMMAND "${CMAKE_COMMAND}" -E env
            "GOCACHE=${CAPSID_BENCHMARK_GO_CACHE}"
            "GOMODCACHE=${CAPSID_BENCHMARK_GO_MODULE_CACHE}"
            "CGO_ENABLED=1"
            "CGO_LDFLAGS=${CAPSID_BENCHMARK_CGO_LDFLAGS}"
            "${CAPSID_BENCHMARK_GO}" build -trimpath -buildvcs=false
            -o "${CAPSID_BENCHMARK_LOADGEN}"
            ./loadgen/cmd/capsid-loadgen
        COMMAND "${CMAKE_COMMAND}" -E env
            "GOCACHE=${CAPSID_BENCHMARK_GO_CACHE}"
            "GOMODCACHE=${CAPSID_BENCHMARK_GO_MODULE_CACHE}"
            "CGO_ENABLED=1"
            "CGO_LDFLAGS=${CAPSID_BENCHMARK_CGO_LDFLAGS}"
            "${CAPSID_BENCHMARK_GO}" build -trimpath -buildvcs=false
            -o "${CAPSID_BENCHMARK_HTTP_GW}"
            ./cmd/capsid-http-gw
        COMMAND "${CMAKE_COMMAND}" -E env
            "GOCACHE=${CAPSID_BENCHMARK_GO_CACHE}"
            "GOMODCACHE=${CAPSID_BENCHMARK_GO_MODULE_CACHE}"
            "CGO_ENABLED=1"
            "CGO_LDFLAGS=${CAPSID_BENCHMARK_CGO_LDFLAGS}"
            "${CAPSID_BENCHMARK_GO}" build -trimpath -buildvcs=false
            -o "${CAPSID_BENCHMARK_IPC_PROFILE}"
            ./cmd/capsid-ipc-profile
        COMMAND "${CMAKE_COMMAND}" -E env
            "GOCACHE=${CAPSID_BENCHMARK_GO_CACHE}"
            "GOMODCACHE=${CAPSID_BENCHMARK_GO_MODULE_CACHE}"
            "CGO_ENABLED=1"
            "CGO_LDFLAGS=${CAPSID_BENCHMARK_CGO_LDFLAGS}"
            "${CAPSID_BENCHMARK_GO}" build -trimpath -buildvcs=false
            -o "${CAPSID_BENCHMARK_STARTUP}"
            ./cmd/capsid-startup-bench
        COMMAND "${CMAKE_COMMAND}" -E env
            "GOCACHE=${CAPSID_BENCHMARK_GO_CACHE}"
            "GOMODCACHE=${CAPSID_BENCHMARK_GO_MODULE_CACHE}"
            "CGO_ENABLED=1"
            "CGO_LDFLAGS=${CAPSID_BENCHMARK_CGO_LDFLAGS}"
            "${CAPSID_BENCHMARK_GO}" build -trimpath -buildvcs=false
            -o "${CAPSID_BENCHMARK_RESOURCE_BASELINE}"
            ./cmd/capsid-resource-baseline
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bench"
        DEPENDS
            ${CAPSID_BENCHMARK_GO_DEPS}
            capsid_runtime
            capsid-worker
        VERBATIM
    )
    add_custom_target(capsid-bench-tools
        DEPENDS
            "${CAPSID_BENCHMARK_LOADGEN}"
            "${CAPSID_BENCHMARK_HTTP_GW}"
            "${CAPSID_BENCHMARK_IPC_PROFILE}"
            "${CAPSID_BENCHMARK_STARTUP}"
            "${CAPSID_BENCHMARK_RESOURCE_BASELINE}"
            capsid-bytecode-compile)

    string(TOUPPER "${CMAKE_BUILD_TYPE}" CAPSID_BENCHMARK_BUILD_TYPE_UPPER)
    set(CAPSID_BENCHMARK_BUILD_FLAGS
        "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${CAPSID_BENCHMARK_BUILD_TYPE_UPPER}}")
    string(STRIP
        "${CAPSID_BENCHMARK_BUILD_FLAGS}"
        CAPSID_BENCHMARK_BUILD_FLAGS)
    string(REPLACE "\\" "\\\\"
        CAPSID_BENCHMARK_BUILD_FLAGS
        "${CAPSID_BENCHMARK_BUILD_FLAGS}")
    string(REPLACE "\"" "\\\""
        CAPSID_BENCHMARK_BUILD_FLAGS
        "${CAPSID_BENCHMARK_BUILD_FLAGS}")
    if(CAPSID_USE_MIMALLOC)
        set(CAPSID_BENCHMARK_ALLOCATOR "mimalloc")
    else()
        set(CAPSID_BENCHMARK_ALLOCATOR "system")
    endif()
    if(CAPSID_ENABLE_LTO AND CAPSID_IPO_SUPPORTED)
        set(CAPSID_BENCHMARK_LTO_JSON true)
    else()
        set(CAPSID_BENCHMARK_LTO_JSON false)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR
       CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        set(CAPSID_BENCHMARK_ASSERTIONS_JSON false)
    else()
        set(CAPSID_BENCHMARK_ASSERTIONS_JSON true)
    endif()
    set(CAPSID_BENCHMARK_SANITIZER_VALUES)
    if(CAPSID_ENABLE_ASAN)
        list(APPEND CAPSID_BENCHMARK_SANITIZER_VALUES "\"ASan\"")
    endif()
    if(CAPSID_ENABLE_UBSAN)
        list(APPEND CAPSID_BENCHMARK_SANITIZER_VALUES "\"UBSan\"")
    endif()
    string(JOIN ", " CAPSID_BENCHMARK_SANITIZER_LIST
        ${CAPSID_BENCHMARK_SANITIZER_VALUES})
    set(CAPSID_BENCHMARK_SANITIZERS_JSON
        "[${CAPSID_BENCHMARK_SANITIZER_LIST}]")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/bench/build-metadata.json.in"
        "${CAPSID_BENCHMARK_METADATA}"
        @ONLY
    )

    add_custom_target(capsid-bench-suite ALL
        DEPENDS capsid-bench-app capsid-bench-tools)
    if(CAPSID_BUILD_SQLITE_BENCHMARK)
        add_dependencies(
            capsid-bench-suite capsid-sqlite-bench-assets)
    endif()

    if(BUILD_TESTING)
        add_test(
            NAME benchmark_external_compare_runner_unit
            COMMAND "${CAPSID_BENCHMARK_PYTHON3}" -m unittest
                bench/external-compare/test_run.py
        )
        add_test(
            NAME benchmark_sqlite_database_unit
            COMMAND "${CAPSID_BENCHMARK_PYTHON3}" -m unittest
                bench/sqlite-compare/test_generate_db.py
        )
        add_test(
            NAME benchmark_artifact_baseline_unit
            COMMAND "${CAPSID_BENCHMARK_PYTHON3}" -m unittest
                bench/artifact-baseline/test_run.py
        )
        add_test(
            NAME benchmark_vue_ssr_fair_runner_unit
            COMMAND "${CAPSID_BENCHMARK_PYTHON3}" -m unittest
                bench/test_vue_ssr_fair.py
        )
        set_tests_properties(
            benchmark_sqlite_database_unit
            PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                LABELS "benchmark"
                TIMEOUT 30
        )
        set_tests_properties(
            benchmark_external_compare_runner_unit
            benchmark_artifact_baseline_unit
            benchmark_vue_ssr_fair_runner_unit
            PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                LABELS "benchmark"
                TIMEOUT 30
        )

        add_test(
            NAME benchmark_app_contract
            COMMAND "${CAPSID_BENCHMARK_NODE}"
                --experimental-global-webcrypto
                "${CAPSID_BENCHMARK_APPS}/contract-test.mjs"
                "${CAPSID_BENCHMARK_APP_OUTPUT}"
        )
        set_tests_properties(benchmark_app_contract PROPERTIES
            LABELS "benchmark"
            TIMEOUT 30)

        add_test(
            NAME benchmark_startup_bytecode_smoke
            COMMAND "${CAPSID_BENCHMARK_STARTUP}"
                --worker "$<TARGET_FILE:capsid-worker>"
                --bytecode-compiler
                    "$<TARGET_FILE:capsid-bytecode-compile>"
                --build-metadata "${CAPSID_BENCHMARK_METADATA}"
                --deno off
                --sizes 1024
                --warmups 0
                --samples 1
                --output
                    "${CMAKE_CURRENT_BINARY_DIR}/Testing/startup-bytecode-smoke.json"
        )
        set_tests_properties(benchmark_startup_bytecode_smoke PROPERTIES
            LABELS "benchmark"
            TIMEOUT 30)

        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(
                NAME benchmark_resource_baseline_smoke
                COMMAND "${CAPSID_BENCHMARK_RESOURCE_BASELINE}"
                    --worker "$<TARGET_FILE:capsid-worker>"
                    --bundle "${CAPSID_BENCHMARK_BUNDLE}"
                    --build-metadata "${CAPSID_BENCHMARK_METADATA}"
                    --samples 1
                    --warm-requests 1
                    --output
                        "${CMAKE_CURRENT_BINARY_DIR}/Testing/resource-baseline-smoke.json"
            )
            set_tests_properties(
                benchmark_resource_baseline_smoke PROPERTIES
                LABELS "benchmark"
                TIMEOUT 30)
        endif()

        add_test(
            NAME benchmark_go_suite
            COMMAND "${CMAKE_COMMAND}" -E env
                "GOCACHE=${CAPSID_BENCHMARK_GO_CACHE}"
                "GOMODCACHE=${CAPSID_BENCHMARK_GO_MODULE_CACHE}"
                "CGO_ENABLED=1"
                "CGO_LDFLAGS=${CAPSID_BENCHMARK_CGO_LDFLAGS}"
                "CAPSID_TEST_WORKER=$<TARGET_FILE:capsid-worker>"
                "CAPSID_TEST_BUNDLE=${CAPSID_BENCHMARK_BUNDLE}"
                "${CAPSID_BENCHMARK_GO}" test ./...
        )
        set_tests_properties(benchmark_go_suite PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bench"
            LABELS "benchmark"
            TIMEOUT 90)

        if(CAPSID_BENCHMARK_DENO)
            add_test(
                NAME benchmark_startup_deno_smoke
                COMMAND "${CAPSID_BENCHMARK_STARTUP}"
                    --worker "$<TARGET_FILE:capsid-worker>"
                    --bytecode-compiler
                        "$<TARGET_FILE:capsid-bytecode-compile>"
                    --build-metadata "${CAPSID_BENCHMARK_METADATA}"
                    --deno "${CAPSID_BENCHMARK_DENO}"
                    --sizes 1024
                    --warmups 0
                    --samples 1
                    --output
                        "${CMAKE_CURRENT_BINARY_DIR}/Testing/startup-deno-smoke.json"
            )
            set_tests_properties(benchmark_startup_deno_smoke PROPERTIES
                LABELS "benchmark"
                TIMEOUT 60)
        endif()

        add_test(
            NAME benchmark_real_worker_smoke
            COMMAND "${CMAKE_COMMAND}" -E env
                "GOCACHE=${CAPSID_BENCHMARK_GO_CACHE}"
                "GOMODCACHE=${CAPSID_BENCHMARK_GO_MODULE_CACHE}"
                "CGO_ENABLED=1"
                "CGO_LDFLAGS=${CAPSID_BENCHMARK_CGO_LDFLAGS}"
                "CAPSID_TEST_WORKER=$<TARGET_FILE:capsid-worker>"
                "CAPSID_TEST_BUNDLE=${CAPSID_BENCHMARK_BUNDLE}"
                "${CAPSID_BENCHMARK_GO}" test ./loadgen/api
                    -run "^TestExternalComparisonL1AndL2$"
                    -count=1
        )
        set_tests_properties(benchmark_real_worker_smoke PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bench"
            LABELS "benchmark"
            TIMEOUT 120)
        if(CAPSID_ENABLE_ASAN)
            # Keep ASan's shadow mapping out of the production RLIMIT_AS only
            # for this sanitizer test process. Release sandbox tests still
            # exercise the real resource limit.
            set_tests_properties(
                benchmark_go_suite
                benchmark_real_worker_smoke
                PROPERTIES
                ENVIRONMENT
                    "CAPSID_TEST_DISABLE_PROCESS_MEMORY_LIMIT=1")
        endif()
    endif()
endif()

install(TARGETS capsid_runtime ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
if(TARGET capsid-worker)
    install(
        TARGETS capsid-worker
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
endif()
install(
    DIRECTORY include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    PATTERN ".DS_Store" EXCLUDE
)
install(
    FILES docs/capability-manifest.json
    DESTINATION "${CMAKE_INSTALL_DATADIR}/capsid-runtime"
)
