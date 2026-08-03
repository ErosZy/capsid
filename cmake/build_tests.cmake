if(BUILD_TESTING)
    if(CAPSID_BUILD_HOST)
        find_package(OpenSSL 3.5 REQUIRED COMPONENTS Crypto)

        add_executable(test-host-config tests/test_host_config.cc)
        target_include_directories(test-host-config PRIVATE src)
        target_link_libraries(test-host-config PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-config PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(test-host-config PRIVATE /W4 /WX)
            else()
                target_compile_options(test-host-config PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(NAME host_config COMMAND test-host-config)

        add_executable(
            test-host-bytecode-attestation
            tests/test_host_bytecode_attestation.cc)
        target_include_directories(
            test-host-bytecode-attestation PRIVATE src)
        target_link_libraries(test-host-bytecode-attestation PRIVATE
            capsid_host_core
            OpenSSL::Crypto
            capsid_sanitizers)
        set_target_properties(test-host-bytecode-attestation PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-bytecode-attestation PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-bytecode-attestation PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(
            NAME host_bytecode_attestation
            COMMAND test-host-bytecode-attestation)

        if(CAPSID_BUILD_WORKER)
            # M1D frozen RED: compiler → offline sign (test key) → host
            # verifier → trusted worker load → identical results to the
            # source-loaded worker; compatibility IDs and determinism; a
            # tamper matrix that must fail closed.
            add_executable(
                test-runtime-bytecode-compiler-round-trip
                tests/test_runtime_bytecode_compiler_round_trip.cc)
            target_include_directories(
                test-runtime-bytecode-compiler-round-trip PRIVATE
                src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(
                test-runtime-bytecode-compiler-round-trip PRIVATE
                capsid_runtime
                capsid_host_core
                capsid_jansson
                OpenSSL::Crypto
                capsid_sanitizers)
            set_target_properties(
                test-runtime-bytecode-compiler-round-trip PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(
                        test-runtime-bytecode-compiler-round-trip PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        test-runtime-bytecode-compiler-round-trip PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            add_test(
                NAME runtime_bytecode_compiler_round_trip
                COMMAND test-runtime-bytecode-compiler-round-trip
                    $<TARGET_FILE:capsid-bytecode-compile>
                    $<TARGET_FILE:capsid-worker>
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ipc-sync-response.js")
            set_tests_properties(
                runtime_bytecode_compiler_round_trip PROPERTIES TIMEOUT 60)
        endif()

        add_executable(
            test-host-artifact-safe-read
            tests/test_host_artifact_safe_read.cc)
        target_include_directories(
            test-host-artifact-safe-read PRIVATE include src)
        target_link_libraries(test-host-artifact-safe-read PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-artifact-safe-read PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-artifact-safe-read PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-artifact-safe-read PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(
            NAME host_artifact_safe_read
            COMMAND test-host-artifact-safe-read)

        add_executable(
            test-host-secret-snapshot
            tests/test_host_secret_snapshot.cc)
        target_include_directories(
            test-host-secret-snapshot PRIVATE include src)
        target_link_libraries(test-host-secret-snapshot PRIVATE
            capsid_host_core
            OpenSSL::Crypto
            capsid_sanitizers)
        set_target_properties(test-host-secret-snapshot PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-secret-snapshot PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-secret-snapshot PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(
            NAME host_secret_snapshot
            COMMAND test-host-secret-snapshot)

        add_executable(
            test-host-active-state
            tests/test_host_active_state.cc)
        target_include_directories(
            test-host-active-state PRIVATE src)
        target_link_libraries(test-host-active-state PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-active-state PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-active-state PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-active-state PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(
            NAME host_active_state
            COMMAND test-host-active-state)

        add_executable(
            test-host-request-normalization
            tests/test_host_request_normalization.cc)
        target_include_directories(
            test-host-request-normalization PRIVATE src)
        target_link_libraries(test-host-request-normalization PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-request-normalization PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-request-normalization PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-request-normalization PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(
            NAME host_request_normalization
            COMMAND test-host-request-normalization)

        add_executable(
            test-host-service-lifecycle
            tests/test_host_service_lifecycle.cc)
        target_include_directories(
            test-host-service-lifecycle PRIVATE src)
        target_link_libraries(test-host-service-lifecycle PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-service-lifecycle PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-service-lifecycle PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-service-lifecycle PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(
            NAME host_service_lifecycle
            COMMAND test-host-service-lifecycle)

        add_executable(
            test-host-worker-recovery
            tests/test_host_worker_recovery.cc)
        target_include_directories(
            test-host-worker-recovery PRIVATE src)
        target_link_libraries(test-host-worker-recovery PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-worker-recovery PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-worker-recovery PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-worker-recovery PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(
            NAME host_worker_recovery
            COMMAND test-host-worker-recovery)

        if(CAPSID_BUILD_WORKER)
            find_program(CAPSID_HOST_TEST_NODE NAMES node REQUIRED)
            add_test(
                NAME host_single_worker_integration
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_single_worker.mjs"
                    --host "$<TARGET_FILE:capsid-host>"
                    --worker "$<TARGET_FILE:capsid-worker>"
                    --bundle
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/host-single-worker.js")
            set_tests_properties(
                host_single_worker_integration
                PROPERTIES
                    LABELS "host;integration;m1"
                    TIMEOUT 30)

            # Metrics-on variant: the same integration run with
            # CAPSID_HOST_IPC_METRICS=1, which arms the per-pump metrics
            # emission path. The acceptance A/B evidence is generated with
            # metrics armed, so the TSan gate must cover this path too — a
            # metrics-off-only TSan pass does not prove the evidence path
            # race-free (worker thread increments metrics_, the io thread
            # reads and resets them; see the frozen RED audit).
            add_test(
                NAME host_single_worker_integration_metrics
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_single_worker.mjs"
                    --host "$<TARGET_FILE:capsid-host>"
                    --worker "$<TARGET_FILE:capsid-worker>"
                    --bundle
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/host-single-worker.js")
            set_tests_properties(
                host_single_worker_integration_metrics
                PROPERTIES
                    LABELS "host;integration;m1"
                    TIMEOUT 30
                    ENVIRONMENT "CAPSID_HOST_IPC_METRICS=1")

            # M1B: the A/B benchmark runner is validated against fake
            # components before any real process is attached (design review
            # §15.7 M1-perf). Skipped (77) where perf is not usable.
            add_test(
                NAME host_single_worker_ab_emits_complete_evidence
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_single_worker_ab_evidence.mjs"
                    --runner "${CMAKE_CURRENT_SOURCE_DIR}/bench/run-ab.sh"
                    --fake-dir "${CMAKE_CURRENT_SOURCE_DIR}/bench/fake"
                    --bundle
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/host-single-worker.js"
                    --build-dir "${CMAKE_BINARY_DIR}")
            set_tests_properties(
                host_single_worker_ab_emits_complete_evidence
                PROPERTIES
                    LABELS "host;benchmark;m1"
                    TIMEOUT 600
                    SKIP_RETURN_CODE 77)
        endif()

        add_executable(test-build-identity tests/test_build_identity.cc)
        file(READ
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/txiki-upgrade-baseline.json"
            CAPSID_TEST_BUILD_IDENTITY_JSON)
        string(JSON CAPSID_TEST_EXPECTED_QUICKJS_COMMIT
            ERROR_VARIABLE CAPSID_TEST_BUILD_IDENTITY_JSON_ERROR
            GET "${CAPSID_TEST_BUILD_IDENTITY_JSON}" quickjs commit)
        if(NOT CAPSID_TEST_BUILD_IDENTITY_JSON_ERROR STREQUAL "NOTFOUND" OR
           NOT CAPSID_TEST_EXPECTED_QUICKJS_COMMIT MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR
                "test build-identity manifest has no valid quickjs.commit")
        endif()
        string(LENGTH "${CAPSID_TEST_EXPECTED_QUICKJS_COMMIT}"
            CAPSID_TEST_EXPECTED_QUICKJS_COMMIT_LENGTH)
        if(NOT CAPSID_TEST_EXPECTED_QUICKJS_COMMIT_LENGTH EQUAL 40)
            message(FATAL_ERROR
                "test build-identity quickjs.commit must be 40 lowercase hex")
        endif()
        target_compile_definitions(test-build-identity PRIVATE
            "CAPSID_TEST_EXPECTED_QUICKJS_COMMIT=\"${CAPSID_TEST_EXPECTED_QUICKJS_COMMIT}\"")
        target_link_libraries(test-build-identity PRIVATE
            capsid_runtime
            OpenSSL::Crypto
            capsid_sanitizers)
        set_target_properties(test-build-identity PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(test-build-identity PRIVATE /W4 /WX)
            else()
                target_compile_options(test-build-identity PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(
            NAME runtime_build_identity
            COMMAND test-build-identity)

        if(CAPSID_BUILD_WORKER)
            if(NOT TARGET capsid-bytecode-compile)
                message(FATAL_ERROR
                    "M0.2 requires the first-party capsid-bytecode-compile "
                    "target when CAPSID_BUILD_WORKER=ON")
            endif()
            add_test(
                NAME runtime_worker_compiler_identity_matches
                COMMAND test-build-identity
                    "$<TARGET_FILE:capsid-worker>"
                    "$<TARGET_FILE:capsid-bytecode-compile>")
        endif()

        if(CAPSID_ENABLE_ASAN OR CAPSID_ENABLE_UBSAN)
            add_test(
                NAME host_sanitizer_instrumentation
                COMMAND "${CMAKE_COMMAND}"
                    "-DCAPSID_COMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json"
                    "-DCAPSID_EXPECT_ASAN=${CAPSID_ENABLE_ASAN}"
                    "-DCAPSID_EXPECT_UBSAN=${CAPSID_ENABLE_UBSAN}"
                    -P
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestHostSanitizerInstrumentation.cmake"
            )
            set_tests_properties(
                host_sanitizer_instrumentation
                PROPERTIES LABELS "host;security;sanitizer")
        endif()
    endif()

    add_executable(test-protocol tests/test_protocol.cc src/protocol.cc)
    target_include_directories(test-protocol PRIVATE src)
    target_link_libraries(test-protocol PRIVATE capsid_sanitizers)
    add_test(NAME protocol COMMAND test-protocol)

    add_executable(
        test-cpu-topology
        tests/test_cpu_topology.cc
        src/cpu_topology.cc)
    target_include_directories(test-cpu-topology PRIVATE src)
    target_link_libraries(test-cpu-topology PRIVATE capsid_sanitizers)
    add_test(NAME cpu_topology COMMAND test-cpu-topology)

    add_executable(
        test-ipc-validation
        tests/test_ipc_validation.cc
        src/capability_policy.cc
        src/egress_policy.cc
        src/ipc_validation.cc
        src/protocol.cc
        src/response_headers.cc
    )
    target_include_directories(test-ipc-validation PRIVATE
        include src "${CAPSID_GENERATED_DIR}")
    target_link_libraries(test-ipc-validation PRIVATE capsid_sanitizers)
    add_test(NAME ipc_validation COMMAND test-ipc-validation)

    add_executable(
        test-egress-policy
        tests/test_egress_policy.cc
        src/egress_policy.cc
    )
    target_include_directories(test-egress-policy PRIVATE include src)
    target_link_libraries(test-egress-policy PRIVATE capsid_sanitizers)
    add_test(NAME egress_policy COMMAND test-egress-policy)

    add_executable(
        test-capability-policy
        tests/test_capability_policy.cc
        src/capability_policy.cc
        src/egress_policy.cc
    )
    target_include_directories(test-capability-policy PRIVATE
        include src "${CAPSID_GENERATED_DIR}")
    target_compile_definitions(test-capability-policy PRIVATE
        CAPSID_CAPABILITY_MANIFEST_PATH="${CAPSID_CAPABILITY_MANIFEST}")
    target_link_libraries(test-capability-policy PRIVATE capsid_sanitizers)
    add_test(NAME capability_policy COMMAND test-capability-policy)

    add_executable(test-public-header tests/test_public_header.c)
    target_link_libraries(test-public-header PRIVATE capsid_runtime)
    add_test(NAME public_header COMMAND test-public-header)

    add_executable(
        test-abi-v7-compat tests/test_abi_v7_compat.c)
    target_include_directories(
        test-abi-v7-compat PRIVATE tests/compat)
    target_link_libraries(
        test-abi-v7-compat PRIVATE capsid_runtime)
    add_test(
        NAME abi_v7_current_header_current_library
        COMMAND test-abi-v7-compat)

    add_executable(test-cpp-header tests/test_cpp_header.cc)
    target_link_libraries(test-cpp-header PRIVATE capsid_runtime)
    add_test(NAME cpp_header COMMAND test-cpp-header)
    if(CAPSID_BUILD_WORKER)
        add_test(
            NAME capsid_product_artifact_names
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_WORKER=$<TARGET_FILE:capsid-worker>"
                "-DCAPSID_RUNTIME_ARCHIVE=$<TARGET_FILE:capsid_runtime>"
                -P
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestCapsidProductNames.cmake"
        )
    endif()

    add_executable(
        test-wpt-report
        tests/test_wpt_report.cc
        tests/wpt_report.cc)
    target_include_directories(test-wpt-report PRIVATE tests)
    target_link_libraries(test-wpt-report PRIVATE capsid_sanitizers)
    add_test(NAME wpt_report_nonzero_gate COMMAND test-wpt-report)

    if(CAPSID_BUILD_WORKER)
        add_executable(test-cpu-affinity tests/test_cpu_affinity.cc)
        target_link_libraries(test-cpu-affinity PRIVATE capsid_runtime)
        add_test(
            NAME worker_cpu_affinity
            COMMAND test-cpu-affinity "$<TARGET_FILE:capsid-worker>")
        set_tests_properties(worker_cpu_affinity PROPERTIES
            TIMEOUT 15
            SKIP_RETURN_CODE 77)
    endif()

    add_executable(
        test-audit
        tests/test_audit.cc
    )
    target_include_directories(test-audit PRIVATE src)
    target_link_libraries(test-audit PRIVATE capsid_runtime)
    add_test(NAME audit COMMAND test-audit)

    if(CAPSID_BUILD_FUZZERS)
        file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus"
            DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/fuzz")

        function(capsid_add_fuzzer TARGET_NAME SOURCE_FILE CORPUS_NAME)
            add_executable("${TARGET_NAME}" "${SOURCE_FILE}" ${ARGN})
            target_include_directories("${TARGET_NAME}" PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_compile_options("${TARGET_NAME}" PRIVATE
                -fsanitize=fuzzer,address,undefined
                -fno-omit-frame-pointer
                -fno-sanitize-recover=all
            )
            target_link_options("${TARGET_NAME}" PRIVATE
                -fsanitize=fuzzer,address,undefined
            )
            if(CAPSID_STRICT_WARNINGS)
                target_compile_options("${TARGET_NAME}" PRIVATE
                    -Wall -Wextra -Wpedantic -Werror
                )
            endif()
            set_target_properties("${TARGET_NAME}" PROPERTIES
                CXX_STANDARD 11
                CXX_STANDARD_REQUIRED ON
            )
            add_test(
                NAME "${TARGET_NAME}_smoke"
                COMMAND "${TARGET_NAME}"
                    "${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus/${CORPUS_NAME}"
                    -runs=10000
                    -max_len=65536
                    -timeout=2
                    -rss_limit_mb=512
            )
            set_tests_properties("${TARGET_NAME}_smoke" PROPERTIES
                LABELS "fuzz"
                TIMEOUT 60
            )
        endfunction()

        capsid_add_fuzzer(
            fuzz-protocol-parser
            tests/fuzz/fuzz_protocol_parser.cc
            protocol
            src/protocol.cc
        )
        capsid_add_fuzzer(
            fuzz-response-headers
            tests/fuzz/fuzz_response_headers.cc
            response_headers
            src/protocol.cc
            src/response_headers.cc
        )
        capsid_add_fuzzer(
            fuzz-worker-protocol
            tests/fuzz/fuzz_worker_protocol.cc
            worker_protocol
            src/egress_policy.cc
            src/capability_policy.cc
            src/ipc_validation.cc
            src/protocol.cc
        )
        capsid_add_fuzzer(
            fuzz-audit-decode
            tests/fuzz/fuzz_audit_decode.cc
            audit
            src/audit.cc
            src/protocol.cc
        )
    endif()

    if(CAPSID_BUILD_WORKER)
        set(CAPSID_GLOBAL_SURFACE_FIXTURE
            "${CAPSID_GENERATED_DIR}/test-global-surface.js")
        add_custom_command(
            OUTPUT "${CAPSID_GLOBAL_SURFACE_FIXTURE}"
            COMMAND "${CAPSID_ESBUILD}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/global-surface.js"
                --bundle
                --target=esnext
                --platform=neutral
                --format=esm
                "--outfile=${CAPSID_GLOBAL_SURFACE_FIXTURE}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/global-surface.js"
                "${CMAKE_CURRENT_SOURCE_DIR}/js/profile-manifest.js"
            VERBATIM
        )
        add_custom_target(test-global-surface-fixture
            DEPENDS "${CAPSID_GLOBAL_SURFACE_FIXTURE}")

        set(CAPSID_WASM_FIXTURE "${CAPSID_GENERATED_DIR}/test-wasm-minimal.js")
        add_custom_command(
            OUTPUT "${CAPSID_WASM_FIXTURE}"
            COMMAND "${CAPSID_ESBUILD}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm-minimal.js"
                --bundle
                --target=esnext
                --platform=neutral
                --format=esm
                "--outfile=${CAPSID_WASM_FIXTURE}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm-minimal.js"
            VERBATIM
        )
        add_custom_target(test-wasm-fixture
            DEPENDS "${CAPSID_WASM_FIXTURE}")

        set(CAPSID_WASM_EDGE_TEST_IDS
            shared_memory
            shared_global
            shared_table
            exported_funcref_table
            exported_global_reimport
            exported_memory_reimport
        )
        set(CAPSID_WASM_EDGE_FIXTURE_TARGETS "")
        foreach(CAPSID_WASM_EDGE_TEST_ID IN LISTS CAPSID_WASM_EDGE_TEST_IDS)
            string(REPLACE "_" "-" CAPSID_WASM_EDGE_FILE_ID
                "${CAPSID_WASM_EDGE_TEST_ID}")
            set(CAPSID_WASM_EDGE_SOURCE
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm-${CAPSID_WASM_EDGE_FILE_ID}.js")
            set(CAPSID_WASM_EDGE_FIXTURE
                "${CAPSID_GENERATED_DIR}/test-wasm-${CAPSID_WASM_EDGE_FILE_ID}.js")
            add_custom_command(
                OUTPUT "${CAPSID_WASM_EDGE_FIXTURE}"
                COMMAND "${CAPSID_ESBUILD}"
                    "${CAPSID_WASM_EDGE_SOURCE}"
                    --bundle
                    --target=esnext
                    --platform=neutral
                    --format=esm
                    "--outfile=${CAPSID_WASM_EDGE_FIXTURE}"
                DEPENDS
                    "${CAPSID_WASM_EDGE_SOURCE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm-edge-cases.js"
                VERBATIM
            )
            set(CAPSID_WASM_EDGE_FIXTURE_TARGET
                "test-wasm-${CAPSID_WASM_EDGE_FILE_ID}-fixture")
            add_custom_target("${CAPSID_WASM_EDGE_FIXTURE_TARGET}"
                DEPENDS "${CAPSID_WASM_EDGE_FIXTURE}")
            list(APPEND CAPSID_WASM_EDGE_FIXTURE_TARGETS
                "${CAPSID_WASM_EDGE_FIXTURE_TARGET}")
            set("CAPSID_WASM_EDGE_FIXTURE_${CAPSID_WASM_EDGE_TEST_ID}"
                "${CAPSID_WASM_EDGE_FIXTURE}")
        endforeach()

        set(CAPSID_FETCH_FIXTURE "${CAPSID_GENERATED_DIR}/test-fetch-local.js")
        add_custom_command(
            OUTPUT "${CAPSID_FETCH_FIXTURE}"
            COMMAND "${CAPSID_ESBUILD}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fetch-local.js"
                --bundle
                --target=esnext
                --platform=neutral
                --format=esm
                "--outfile=${CAPSID_FETCH_FIXTURE}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fetch-local.js"
            VERBATIM
        )
        add_custom_target(test-fetch-fixture
            DEPENDS "${CAPSID_FETCH_FIXTURE}")

        set(CAPSID_DIRECT_FETCH_MATRIX_FIXTURE
            "${CAPSID_GENERATED_DIR}/test-direct-fetch-matrix.js")
        add_custom_command(
            OUTPUT "${CAPSID_DIRECT_FETCH_MATRIX_FIXTURE}"
            COMMAND "${CAPSID_ESBUILD}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/direct-fetch-matrix.js"
                --bundle
                --target=esnext
                --platform=neutral
                --format=esm
                "--outfile=${CAPSID_DIRECT_FETCH_MATRIX_FIXTURE}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/direct-fetch-matrix.js"
            VERBATIM
        )
        add_custom_target(test-direct-fetch-matrix-fixture
            DEPENDS "${CAPSID_DIRECT_FETCH_MATRIX_FIXTURE}")

        set(CAPSID_DIRECT_FETCH_TLS_FIXTURE
            "${CAPSID_GENERATED_DIR}/test-direct-fetch-tls.js")
        add_custom_command(
            OUTPUT "${CAPSID_DIRECT_FETCH_TLS_FIXTURE}"
            COMMAND "${CAPSID_ESBUILD}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/direct-fetch-tls.js"
                --bundle
                --target=esnext
                --platform=neutral
                --format=esm
                "--outfile=${CAPSID_DIRECT_FETCH_TLS_FIXTURE}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/direct-fetch-tls.js"
            VERBATIM
        )
        add_custom_target(test-direct-fetch-tls-fixture
            DEPENDS "${CAPSID_DIRECT_FETCH_TLS_FIXTURE}")

        set(CAPSID_P0_FIXTURE "${CAPSID_GENERATED_DIR}/test-p0-contract.js")
        add_custom_command(
            OUTPUT "${CAPSID_P0_FIXTURE}"
            COMMAND "${CAPSID_ESBUILD}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/p0-contract.js"
                --bundle
                --target=esnext
                --platform=neutral
                --format=esm
                "--outfile=${CAPSID_P0_FIXTURE}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/p0-contract.js"
            VERBATIM
        )
        add_custom_target(test-p0-fixture DEPENDS "${CAPSID_P0_FIXTURE}")

        set(CAPSID_P1_PLATFORM_FIXTURE
            "${CAPSID_GENERATED_DIR}/test-p1-platform-contract.js")
        add_custom_command(
            OUTPUT "${CAPSID_P1_PLATFORM_FIXTURE}"
            COMMAND "${CAPSID_ESBUILD}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/p1-platform-contract.js"
                --bundle
                --target=esnext
                --platform=neutral
                --format=esm
                "--outfile=${CAPSID_P1_PLATFORM_FIXTURE}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/p1-platform-contract.js"
            VERBATIM
        )
        add_custom_target(test-p1-platform-fixture
            DEPENDS "${CAPSID_P1_PLATFORM_FIXTURE}")

        set(CAPSID_HONO_REFERENCE_ROOT
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/hono-reference")
        set(CAPSID_HONO_VERSION "4.12.32")
        if(NOT EXISTS
           "${CAPSID_HONO_REFERENCE_ROOT}/node_modules/hono/package.json")
            message(FATAL_ERROR
                "Hono ${CAPSID_HONO_VERSION} is missing; run "
                "npm ci --ignore-scripts --prefix examples/hono-reference")
        endif()
        file(READ
            "${CAPSID_HONO_REFERENCE_ROOT}/node_modules/hono/package.json"
            CAPSID_HONO_INSTALLED_PACKAGE)
        string(FIND
            "${CAPSID_HONO_INSTALLED_PACKAGE}"
            "\"version\": \"${CAPSID_HONO_VERSION}\""
            CAPSID_HONO_VERSION_OFFSET)
        if(CAPSID_HONO_VERSION_OFFSET EQUAL -1)
            message(FATAL_ERROR
                "examples/hono-reference must contain exactly "
                "Hono ${CAPSID_HONO_VERSION}")
        endif()
        find_program(CAPSID_NODE_EXECUTABLE NAMES node REQUIRED)
        add_test(
            NAME testing_validity_workflow_audit
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/audit-testing-validity-workflow.mjs"
                "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        set_tests_properties(
            testing_validity_workflow_audit
            PROPERTIES TIMEOUT 20 LABELS "security;ci;audit"
        )
        add_test(
            NAME current_documentation_audit
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/audit-current-docs.mjs"
                "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        set_tests_properties(
            current_documentation_audit
            PROPERTIES TIMEOUT 20 LABELS "documentation;audit"
        )
        file(GLOB_RECURSE CAPSID_HONO_BUNDLE_DEPS CONFIGURE_DEPENDS
            "${CAPSID_HONO_REFERENCE_ROOT}/src/*.js"
            "${CAPSID_HONO_REFERENCE_ROOT}/node_modules/hono/dist/*.js"
            "${CAPSID_HONO_REFERENCE_ROOT}/package.json"
            "${CAPSID_HONO_REFERENCE_ROOT}/package-lock.json"
        )

        function(capsid_add_hono_fixture name entry)
            set(output
                "${CAPSID_GENERATED_DIR}/test-hono-${name}.js")
            set(metafile "${output}.meta.json")
            add_custom_command(
                OUTPUT "${output}" "${metafile}"
                COMMAND "${CAPSID_ESBUILD}"
                    "${CAPSID_HONO_REFERENCE_ROOT}/src/${entry}"
                    --bundle
                    --minify
                    --keep-names
                    --tree-shaking=true
                    --target=esnext
                    --platform=neutral
                    --format=esm
                    "--metafile=${metafile}"
                    "--outfile=${output}"
                COMMAND "${CAPSID_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hono/audit-bundle.mjs"
                    "${output}"
                    "${metafile}"
                DEPENDS
                    ${CAPSID_HONO_BUNDLE_DEPS}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hono/audit-bundle.mjs"
                VERBATIM
            )
            add_custom_target("test-hono-${name}-fixture"
                DEPENDS "${output}" "${metafile}")
            set("CAPSID_HONO_${name}_FIXTURE"
                "${output}" PARENT_SCOPE)
        endfunction()

        capsid_add_hono_fixture(DEFAULT entry-default.js)
        capsid_add_hono_fixture(OBJECT entry-object.js)
        capsid_add_hono_fixture(NAMED entry-named.js)

        set(CAPSID_ITTY_ROUTER_REFERENCE_ROOT
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/itty-router-reference")
        set(CAPSID_ITTY_ROUTER_VERSION "5.0.24")
        if(NOT EXISTS
           "${CAPSID_ITTY_ROUTER_REFERENCE_ROOT}/node_modules/itty-router/package.json")
            message(FATAL_ERROR
                "itty-router ${CAPSID_ITTY_ROUTER_VERSION} is missing; run "
                "npm ci --ignore-scripts --prefix "
                "examples/itty-router-reference")
        endif()
        file(READ
            "${CAPSID_ITTY_ROUTER_REFERENCE_ROOT}/node_modules/itty-router/package.json"
            CAPSID_ITTY_ROUTER_INSTALLED_PACKAGE)
        string(FIND
            "${CAPSID_ITTY_ROUTER_INSTALLED_PACKAGE}"
            "\"version\": \"${CAPSID_ITTY_ROUTER_VERSION}\""
            CAPSID_ITTY_ROUTER_VERSION_OFFSET)
        if(CAPSID_ITTY_ROUTER_VERSION_OFFSET EQUAL -1)
            message(FATAL_ERROR
                "examples/itty-router-reference must contain exactly "
                "itty-router ${CAPSID_ITTY_ROUTER_VERSION}")
        endif()
        file(GLOB_RECURSE CAPSID_ITTY_ROUTER_BUNDLE_DEPS CONFIGURE_DEPENDS
            "${CAPSID_ITTY_ROUTER_REFERENCE_ROOT}/src/*.js"
            "${CAPSID_ITTY_ROUTER_REFERENCE_ROOT}/node_modules/itty-router/*.mjs"
            "${CAPSID_ITTY_ROUTER_REFERENCE_ROOT}/package.json"
            "${CAPSID_ITTY_ROUTER_REFERENCE_ROOT}/package-lock.json"
        )

        function(capsid_add_itty_router_fixture name entry)
            set(output
                "${CAPSID_GENERATED_DIR}/test-itty-router-${name}.js")
            set(metafile "${output}.meta.json")
            add_custom_command(
                OUTPUT "${output}" "${metafile}"
                COMMAND "${CAPSID_NODE_EXECUTABLE}"
                    "${CAPSID_ITTY_ROUTER_REFERENCE_ROOT}/build.mjs"
                    --esbuild "${CAPSID_ESBUILD}"
                    --entry
                        "${CAPSID_ITTY_ROUTER_REFERENCE_ROOT}/src/${entry}"
                    --outfile "${output}"
                    --metafile "${metafile}"
                COMMAND "${CAPSID_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/itty-router/audit-bundle.mjs"
                    "${output}"
                    "${metafile}"
                DEPENDS
                    ${CAPSID_ITTY_ROUTER_BUNDLE_DEPS}
                    "${CAPSID_ITTY_ROUTER_REFERENCE_ROOT}/build.mjs"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/itty-router/audit-bundle.mjs"
                VERBATIM
            )
            add_custom_target("test-itty-router-${name}-fixture"
                DEPENDS "${output}" "${metafile}")
            set("CAPSID_ITTY_ROUTER_${name}_FIXTURE"
                "${output}" PARENT_SCOPE)
        endfunction()

        capsid_add_itty_router_fixture(AUTOROUTER entry-autorouter.js)
        capsid_add_itty_router_fixture(ROUTER entry-router.js)
        capsid_add_itty_router_fixture(ITTY entry-itty-router.js)

        set(CAPSID_H3_V2_REFERENCE_ROOT
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/h3-v2-reference")
        set(CAPSID_H3_V2_VERSION "2.0.1-rc.26")
        if(NOT EXISTS
           "${CAPSID_H3_V2_REFERENCE_ROOT}/node_modules/h3/package.json")
            message(FATAL_ERROR
                "H3 ${CAPSID_H3_V2_VERSION} is missing; run "
                "npm ci --ignore-scripts --prefix "
                "examples/h3-v2-reference")
        endif()
        file(READ
            "${CAPSID_H3_V2_REFERENCE_ROOT}/package.json"
            CAPSID_H3_V2_REFERENCE_PACKAGE)
        file(READ
            "${CAPSID_H3_V2_REFERENCE_ROOT}/package-lock.json"
            CAPSID_H3_V2_REFERENCE_LOCK)
        file(READ
            "${CAPSID_H3_V2_REFERENCE_ROOT}/node_modules/h3/package.json"
            CAPSID_H3_V2_INSTALLED_PACKAGE)
        string(FIND
            "${CAPSID_H3_V2_REFERENCE_PACKAGE}"
            "\"h3\": \"${CAPSID_H3_V2_VERSION}\""
            CAPSID_H3_V2_PACKAGE_VERSION_OFFSET)
        string(FIND
            "${CAPSID_H3_V2_REFERENCE_LOCK}"
            "\"h3\": \"${CAPSID_H3_V2_VERSION}\""
            CAPSID_H3_V2_LOCK_VERSION_OFFSET)
        string(FIND
            "${CAPSID_H3_V2_INSTALLED_PACKAGE}"
            "\"version\": \"${CAPSID_H3_V2_VERSION}\""
            CAPSID_H3_V2_INSTALLED_VERSION_OFFSET)
        if(CAPSID_H3_V2_PACKAGE_VERSION_OFFSET EQUAL -1 OR
           CAPSID_H3_V2_LOCK_VERSION_OFFSET EQUAL -1 OR
           CAPSID_H3_V2_INSTALLED_VERSION_OFFSET EQUAL -1)
            message(FATAL_ERROR
                "examples/h3-v2-reference package, lockfile and install "
                "must all pin exactly H3 ${CAPSID_H3_V2_VERSION}")
        endif()
        file(GLOB_RECURSE CAPSID_H3_V2_BUNDLE_DEPS CONFIGURE_DEPENDS
            "${CAPSID_H3_V2_REFERENCE_ROOT}/src/*.js"
            "${CAPSID_H3_V2_REFERENCE_ROOT}/node_modules/h3/dist/*.mjs"
            "${CAPSID_H3_V2_REFERENCE_ROOT}/node_modules/rou3/dist/*.mjs"
            "${CAPSID_H3_V2_REFERENCE_ROOT}/node_modules/srvx/dist/*.mjs"
            "${CAPSID_H3_V2_REFERENCE_ROOT}/package.json"
            "${CAPSID_H3_V2_REFERENCE_ROOT}/package-lock.json"
        )

        function(capsid_add_h3_v2_fixture name entry)
            set(output
                "${CAPSID_GENERATED_DIR}/test-h3-v2-${name}.js")
            set(metafile "${output}.meta.json")
            add_custom_command(
                OUTPUT "${output}" "${metafile}"
                COMMAND "${CAPSID_NODE_EXECUTABLE}"
                    "${CAPSID_H3_V2_REFERENCE_ROOT}/build.mjs"
                    --esbuild "${CAPSID_ESBUILD}"
                    --entry
                        "${CAPSID_H3_V2_REFERENCE_ROOT}/src/${entry}"
                    --outfile "${output}"
                    --metafile "${metafile}"
                COMMAND "${CAPSID_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/h3-v2/audit-bundle.mjs"
                    "${output}"
                    "${metafile}"
                DEPENDS
                    ${CAPSID_H3_V2_BUNDLE_DEPS}
                    "${CAPSID_H3_V2_REFERENCE_ROOT}/build.mjs"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/h3-v2/audit-bundle.mjs"
                VERBATIM
            )
            add_custom_target("test-h3-v2-${name}-fixture"
                DEPENDS "${output}" "${metafile}")
            set("CAPSID_H3_V2_${name}_FIXTURE"
                "${output}" PARENT_SCOPE)
        endfunction()

        capsid_add_h3_v2_fixture(DEFAULT_APP entry-default-app.js)
        capsid_add_h3_v2_fixture(DEFAULT_OBJECT entry-default-object.js)
        capsid_add_h3_v2_fixture(NAMED entry-named.js)
        capsid_add_h3_v2_fixture(WRAPPER entry-wrapper.js)
        capsid_add_h3_v2_fixture(HANDLER entry-handler.js)
        capsid_add_h3_v2_fixture(MALFORMED entry-malformed.js)
        capsid_add_h3_v2_fixture(DEBUG entry-debug.js)

        if(CAPSID_WPT_ROOT)
            file(READ
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/manifest.json"
                CAPSID_WPT_MANIFEST_JSON)
            string(JSON CAPSID_WPT_REVISION ERROR_VARIABLE CAPSID_WPT_JSON_ERROR
                GET "${CAPSID_WPT_MANIFEST_JSON}" commit)
            if(CAPSID_WPT_JSON_ERROR)
                message(FATAL_ERROR
                    "tests/wpt/manifest.json has no pinned commit: "
                    "${CAPSID_WPT_JSON_ERROR}")
            endif()
            execute_process(
                COMMAND git -C "${CAPSID_WPT_ROOT}" rev-parse HEAD
                OUTPUT_VARIABLE CAPSID_WPT_ACTUAL_REVISION
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE CAPSID_WPT_REVISION_RESULT
            )
            if(NOT CAPSID_WPT_REVISION_RESULT EQUAL 0 OR
               NOT CAPSID_WPT_ACTUAL_REVISION STREQUAL CAPSID_WPT_REVISION)
                message(FATAL_ERROR
                    "CAPSID_WPT_ROOT must be WPT commit ${CAPSID_WPT_REVISION}; "
                    "got '${CAPSID_WPT_ACTUAL_REVISION}'")
            endif()

            # Expected failures are structured conformance metadata rather than
            # CMake string literals. The standalone metadata audit validates
            # deviation IDs, duplicates, and path membership even when no WPT
            # checkout is configured.
            string(JSON CAPSID_WPT_EXPECTED_FAILURE_COUNT
                ERROR_VARIABLE CAPSID_WPT_JSON_ERROR
                LENGTH "${CAPSID_WPT_MANIFEST_JSON}" expectedFailures)
            if(CAPSID_WPT_JSON_ERROR OR
               CAPSID_WPT_EXPECTED_FAILURE_COUNT LESS 1)
                message(FATAL_ERROR
                    "tests/wpt/manifest.json must declare expectedFailures")
            endif()
            math(EXPR CAPSID_WPT_EXPECTED_FAILURE_LAST
                "${CAPSID_WPT_EXPECTED_FAILURE_COUNT} - 1")
            foreach(CAPSID_EXPECTED_INDEX
                    RANGE 0 ${CAPSID_WPT_EXPECTED_FAILURE_LAST})
                string(JSON CAPSID_EXPECTED_PATH GET
                    "${CAPSID_WPT_MANIFEST_JSON}"
                    expectedFailures ${CAPSID_EXPECTED_INDEX} path)
                string(JSON CAPSID_EXPECTED_SUBTEST GET
                    "${CAPSID_WPT_MANIFEST_JSON}"
                    expectedFailures ${CAPSID_EXPECTED_INDEX} subtest)
                string(JSON CAPSID_EXPECTED_DEVIATION GET
                    "${CAPSID_WPT_MANIFEST_JSON}"
                    expectedFailures ${CAPSID_EXPECTED_INDEX} deviation)
                string(REPLACE "\\" "\\\\" CAPSID_EXPECTED_SUBTEST_JS
                    "${CAPSID_EXPECTED_SUBTEST}")
                string(REPLACE "'" "\\'" CAPSID_EXPECTED_SUBTEST_JS
                    "${CAPSID_EXPECTED_SUBTEST_JS}")
                string(REPLACE "\\" "\\\\" CAPSID_EXPECTED_DEVIATION_JS
                    "${CAPSID_EXPECTED_DEVIATION}")
                string(REPLACE "'" "\\'" CAPSID_EXPECTED_DEVIATION_JS
                    "${CAPSID_EXPECTED_DEVIATION_JS}")
                string(MAKE_C_IDENTIFIER
                    "${CAPSID_EXPECTED_PATH}" CAPSID_EXPECTED_PATH_ID)
                set(CAPSID_EXPECTED_VARIABLE
                    "CAPSID_WPT_EXPECTED_FAILURES_${CAPSID_EXPECTED_PATH_ID}")
                set(CAPSID_EXPECTED_OBJECT
                    "{ name: '${CAPSID_EXPECTED_SUBTEST_JS}', deviation: '${CAPSID_EXPECTED_DEVIATION_JS}' }")
                if(DEFINED ${CAPSID_EXPECTED_VARIABLE})
                    set("${CAPSID_EXPECTED_VARIABLE}"
                        "${${CAPSID_EXPECTED_VARIABLE}}, ${CAPSID_EXPECTED_OBJECT}")
                else()
                    set("${CAPSID_EXPECTED_VARIABLE}"
                        "${CAPSID_EXPECTED_OBJECT}")
                endif()
            endforeach()

            set(CAPSID_WPT_BATCH1
                dom/events/EventTarget-constructible.any.js
                dom/events/EventTarget-add-remove-listener.any.js
                dom/events/AddEventListenerOptions-once.any.js
                html/webappapis/timers/clearinterval-from-callback.any.js
                html/webappapis/timers/negative-setinterval.any.js
                html/webappapis/microtask-queuing/queue-microtask.any.js
                html/webappapis/scripting/reporterror.any.js
                html/webappapis/scripting/processing-model-2/unhandled-promise-rejections/promise-rejection-event-constructor.html
                html/webappapis/scripting/processing-model-2/unhandled-promise-rejections/support/promise-rejection-events.js
                encoding/textdecoder-fatal.any.js
                encoding/api-basics.any.js
                encoding/api-invalid-label.any.js
                encoding/api-replacement-encodings.any.js
                encoding/api-surrogates-utf8.any.js
                encoding/encodeInto.any.js
                encoding/single-byte-decoder.any.js
                encoding/textdecoder-arguments.any.js
                encoding/textdecoder-byte-order-marks.any.js
                encoding/textdecoder-copy.any.js
                encoding/textdecoder-fatal-single-byte.any.js
                encoding/textdecoder-fatal-streaming.any.js
                encoding/textdecoder-ignorebom.any.js
                encoding/textdecoder-labels.any.js
                encoding/textdecoder-streaming.any.js
                encoding/textdecoder-utf16-surrogates.any.js
                encoding/textencoder-constructor-non-utf.any.js
                encoding/textencoder-utf16-surrogates.any.js
                encoding/legacy-mb-schinese/gbk/gbk-decoder.any.js
                encoding/legacy-mb-schinese/gb18030/gb18030-decoder.any.js
                encoding/iso-2022-jp-decoder.any.js
                encoding/legacy-mb-tchinese/big5/big5-decode.html
                encoding/legacy-mb-japanese/euc-jp/eucjp-decode.html
                encoding/legacy-mb-japanese/shift_jis/sjis-decode.html
                encoding/legacy-mb-korean/euc-kr/euckr-decode.html
                encoding/textdecoder-eof.any.js
                encoding/textdecoder-mistakes.any.js
                encoding/streams/backpressure.any.js
                encoding/streams/decode-attributes.any.js
                encoding/streams/decode-bad-chunks.any.js
                encoding/streams/decode-ignore-bom.any.js
                encoding/streams/decode-incomplete-input.any.js
                encoding/streams/decode-non-utf8.any.js
                encoding/streams/decode-split-character.any.js
                encoding/streams/decode-utf8.any.js
                encoding/streams/encode-bad-chunks.any.js
                encoding/streams/encode-utf8.any.js
                encoding/streams/readable-writable-properties.any.js
                encoding/idlharness.any.js
                url/url-constructor.any.js
                url/urlsearchparams-constructor.any.js
                urlpattern/urlpattern-constructor.any.js
                FileAPI/blob/Blob-constructor.any.js
                FileAPI/file/File-constructor.any.js
                streams/readable-streams/constructor.any.js
                streams/writable-streams/constructor.any.js
                streams/transform-streams/properties.any.js
                streams/queuing-strategies.any.js
                webmessaging/message-channels/basics.any.js
                webmessaging/message-channels/close.any.js
                webmessaging/message-channels/implied-start.any.js
                webmessaging/MessagePort_initial_disabled.any.js
                compression/compression-stream.any.js
                WebCryptoAPI/getRandomValues.any.js
                WebCryptoAPI/randomUUID.https.any.js
                fetch/api/headers/headers-basic.any.js
                fetch/api/headers/headers-errors.any.js
                fetch/api/response/response-init-001.any.js
                fetch/api/response/response-static-json.any.js
                fetch/api/request/request-init-002.any.js
                wasm/jsapi/constructor/compile.any.js
                wasm/jsapi/constructor/instantiate.any.js
                wasm/jsapi/constructor/instantiate-bad-imports.any.js
                wasm/jsapi/constructor/validate.any.js
                wasm/jsapi/memory/constructor.any.js
                wasm/jsapi/memory/grow.any.js
                wasm/jsapi/table/constructor.any.js
                wasm/jsapi/table/grow.any.js
                wasm/jsapi/global/constructor.any.js
                wasm/jsapi/global/value-get-set.any.js
                wasm/webapi/instantiateStreaming-bad-imports.any.js
                wasm/webapi/instantiateStreaming.any.js
                console/console-is-a-namespace.any.js
                hr-time/basic.any.js
                hr-time/monotonic-clock.any.js
            )
            set(CAPSID_WPT_REGISTERED_PATHS_FILE
                "${CAPSID_GENERATED_DIR}/wpt-registered-paths.txt")
            string(REPLACE ";" "\n" CAPSID_WPT_REGISTERED_PATHS_TEXT
                "${CAPSID_WPT_BATCH1}")
            file(GENERATE
                OUTPUT "${CAPSID_WPT_REGISTERED_PATHS_FILE}"
                CONTENT "${CAPSID_WPT_REGISTERED_PATHS_TEXT}\n")
            set(CAPSID_WPT_FIXTURE_TARGETS)
            set(CAPSID_WPT_TEST_IDS)
            foreach(CAPSID_WPT_PATH IN LISTS CAPSID_WPT_BATCH1)
                if(NOT EXISTS "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}")
                    message(FATAL_ERROR
                        "locked WPT path is missing: ${CAPSID_WPT_PATH}")
                endif()
                string(MAKE_C_IDENTIFIER "${CAPSID_WPT_PATH}" CAPSID_WPT_TEST_ID)
                set("CAPSID_WPT_SOURCE_URL_${CAPSID_WPT_TEST_ID}"
                    "https://wpt.local/${CAPSID_WPT_PATH}")
                set(CAPSID_WPT_SOURCE "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}")
                set(CAPSID_WPT_SOURCE_DEPS "${CAPSID_WPT_SOURCE}")
                if(CAPSID_WPT_PATH MATCHES
                   "^encoding/legacy-mb-(tchinese/big5/big5|japanese/euc-jp/eucjp|japanese/shift_jis/sjis|korean/euc-kr/euckr)-decode\\.html$")
                    set(CAPSID_WPT_SOURCE
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-combined.js")
                    set(CAPSID_WPT_INDEX_SOURCES)
                    if(CAPSID_WPT_PATH MATCHES "tchinese/big5")
                        list(APPEND CAPSID_WPT_INDEX_SOURCES
                            "${CAPSID_WPT_ROOT}/encoding/legacy-mb-tchinese/big5/big5_index.js")
                    elseif(CAPSID_WPT_PATH MATCHES "japanese/euc-jp")
                        list(APPEND CAPSID_WPT_INDEX_SOURCES
                            "${CAPSID_WPT_ROOT}/encoding/legacy-mb-japanese/euc-jp/jis0208_index.js"
                            "${CAPSID_WPT_ROOT}/encoding/legacy-mb-japanese/euc-jp/jis0212_index.js")
                    elseif(CAPSID_WPT_PATH MATCHES "japanese/shift_jis")
                        list(APPEND CAPSID_WPT_INDEX_SOURCES
                            "${CAPSID_WPT_ROOT}/encoding/legacy-mb-japanese/shift_jis/jis0208_index.js")
                    elseif(CAPSID_WPT_PATH MATCHES "korean/euc-kr")
                        list(APPEND CAPSID_WPT_INDEX_SOURCES
                            "${CAPSID_WPT_ROOT}/encoding/legacy-mb-korean/euc-kr/euckr_index.js")
                    endif()
                    add_custom_command(
                        OUTPUT "${CAPSID_WPT_SOURCE}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_SOURCE}"
                            ${CAPSID_WPT_INDEX_SOURCES}
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/multibyte-index-decoder.js"
                        DEPENDS
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            ${CAPSID_WPT_INDEX_SOURCES}
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/multibyte-index-decoder.js"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        VERBATIM
                    )
                    list(APPEND CAPSID_WPT_SOURCE_DEPS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                        ${CAPSID_WPT_INDEX_SOURCES}
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/multibyte-index-decoder.js")
                elseif(CAPSID_WPT_PATH MATCHES "\\.html$")
                    set(CAPSID_WPT_SOURCE
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-inline.js")
                    add_custom_command(
                        OUTPUT "${CAPSID_WPT_SOURCE}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/extract-inline-script.mjs"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                            "${CAPSID_WPT_SOURCE}"
                        DEPENDS
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/extract-inline-script.mjs"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        VERBATIM
                    )
                    list(APPEND CAPSID_WPT_SOURCE_DEPS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/extract-inline-script.mjs")
                elseif(CAPSID_WPT_PATH STREQUAL
                       "encoding/single-byte-decoder.any.js")
                    set(CAPSID_WPT_SOURCE
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-combined.js")
                    set(CAPSID_WPT_ENCODINGS
                        "${CAPSID_WPT_ROOT}/encoding/resources/encodings.js")
                    set(CAPSID_WPT_SINGLE_BYTE_INDEXES
                        "${CAPSID_WPT_ROOT}/encoding/resources/single-byte-decoder.js")
                    add_custom_command(
                        OUTPUT "${CAPSID_WPT_SOURCE}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_SOURCE}"
                            "${CAPSID_WPT_ENCODINGS}"
                            "${CAPSID_WPT_SINGLE_BYTE_INDEXES}"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        DEPENDS
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_ENCODINGS}"
                            "${CAPSID_WPT_SINGLE_BYTE_INDEXES}"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        VERBATIM
                    )
                    list(APPEND CAPSID_WPT_SOURCE_DEPS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                        "${CAPSID_WPT_ENCODINGS}"
                        "${CAPSID_WPT_SINGLE_BYTE_INDEXES}")
                elseif(CAPSID_WPT_PATH STREQUAL
                       "encoding/legacy-mb-schinese/gb18030/gb18030-decoder.any.js")
                    set(CAPSID_WPT_SOURCE
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-combined.js")
                    set(CAPSID_WPT_GB18030_RANGES
                        "${CAPSID_WPT_ROOT}/encoding/legacy-mb-schinese/gb18030/resources/ranges.js")
                    add_custom_command(
                        OUTPUT "${CAPSID_WPT_SOURCE}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_SOURCE}"
                            "${CAPSID_WPT_GB18030_RANGES}"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        DEPENDS
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_GB18030_RANGES}"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        VERBATIM
                    )
                    list(APPEND CAPSID_WPT_SOURCE_DEPS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                        "${CAPSID_WPT_GB18030_RANGES}")
                elseif(CAPSID_WPT_PATH MATCHES
                       "^encoding/(api-invalid-label|api-replacement-encodings|encodeInto|textdecoder-copy|textdecoder-labels|textdecoder-streaming|textencoder-constructor-non-utf)\\.any\\.js$")
                    set(CAPSID_WPT_SOURCE
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-combined.js")
                    set(CAPSID_WPT_EXTRA_SOURCES)
                    if(NOT CAPSID_WPT_PATH MATCHES
                       "^encoding/(encodeInto|textdecoder-copy)\\.any\\.js$")
                        list(APPEND CAPSID_WPT_EXTRA_SOURCES
                            "${CAPSID_WPT_ROOT}/encoding/resources/encodings.js")
                    endif()
                    if(CAPSID_WPT_PATH MATCHES
                       "^encoding/(encodeInto|textdecoder-copy|textdecoder-streaming)\\.any\\.js$")
                        list(APPEND CAPSID_WPT_EXTRA_SOURCES
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/sab.js")
                    endif()
                    add_custom_command(
                        OUTPUT "${CAPSID_WPT_SOURCE}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_SOURCE}"
                            ${CAPSID_WPT_EXTRA_SOURCES}
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        DEPENDS
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            ${CAPSID_WPT_EXTRA_SOURCES}
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        VERBATIM
                    )
                    list(APPEND CAPSID_WPT_SOURCE_DEPS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                        ${CAPSID_WPT_EXTRA_SOURCES})
                elseif(CAPSID_WPT_PATH MATCHES
                       "^encoding/streams/.*\\.any\\.js$")
                    set(CAPSID_WPT_SOURCE
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-combined.js")
                    set(CAPSID_WPT_STREAM_SOURCES
                        "${CAPSID_WPT_ROOT}/encoding/streams/resources/readable-stream-from-array.js"
                        "${CAPSID_WPT_ROOT}/encoding/streams/resources/readable-stream-to-array.js")
                    if(CAPSID_WPT_PATH STREQUAL
                       "encoding/streams/decode-utf8.any.js")
                        list(APPEND CAPSID_WPT_STREAM_SOURCES
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/sab.js")
                    endif()
                    add_custom_command(
                        OUTPUT "${CAPSID_WPT_SOURCE}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_SOURCE}"
                            ${CAPSID_WPT_STREAM_SOURCES}
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        DEPENDS
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            ${CAPSID_WPT_STREAM_SOURCES}
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        VERBATIM
                    )
                    list(APPEND CAPSID_WPT_SOURCE_DEPS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                        ${CAPSID_WPT_STREAM_SOURCES})
                elseif(CAPSID_WPT_PATH STREQUAL
                       "encoding/idlharness.any.js")
                    set(CAPSID_WPT_SOURCE
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-combined.js")
                    set(CAPSID_WPT_IDL_SOURCES
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-idl-sources.js")
                    set(CAPSID_WPT_WEBIDL2
                        "${CAPSID_WPT_ROOT}/resources/webidl2/lib/webidl2.js")
                    set(CAPSID_WPT_WEBIDL2_NORMALIZED
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-webidl2.js")
                    set(CAPSID_WPT_IDLHARNESS
                        "${CAPSID_WPT_ROOT}/resources/idlharness.js")
                    set(CAPSID_WPT_IDLHARNESS_NORMALIZED
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-idlharness.js")
                    add_custom_command(
                        OUTPUT
                            "${CAPSID_WPT_SOURCE}"
                            "${CAPSID_WPT_IDL_SOURCES}"
                            "${CAPSID_WPT_WEBIDL2_NORMALIZED}"
                            "${CAPSID_WPT_IDLHARNESS_NORMALIZED}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/generate-idl-sources.mjs"
                            "${CAPSID_WPT_ROOT}"
                            "${CAPSID_WPT_IDL_SOURCES}"
                            encoding streams
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/normalize-webidl2.mjs"
                            "${CAPSID_WPT_WEBIDL2}"
                            "${CAPSID_WPT_WEBIDL2_NORMALIZED}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/normalize-idlharness.mjs"
                            "${CAPSID_WPT_IDLHARNESS}"
                            "${CAPSID_WPT_IDLHARNESS_NORMALIZED}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_SOURCE}"
                            "${CAPSID_WPT_WEBIDL2_NORMALIZED}"
                            "${CAPSID_WPT_IDLHARNESS_NORMALIZED}"
                            "${CAPSID_WPT_IDL_SOURCES}"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        DEPENDS
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/generate-idl-sources.mjs"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/normalize-webidl2.mjs"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/normalize-idlharness.mjs"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_WEBIDL2}"
                            "${CAPSID_WPT_IDLHARNESS}"
                            "${CAPSID_WPT_ROOT}/interfaces/encoding.idl"
                            "${CAPSID_WPT_ROOT}/interfaces/streams.idl"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        VERBATIM
                    )
                    list(APPEND CAPSID_WPT_SOURCE_DEPS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/generate-idl-sources.mjs"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/normalize-webidl2.mjs"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/normalize-idlharness.mjs"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                        "${CAPSID_WPT_WEBIDL2}"
                        "${CAPSID_WPT_IDLHARNESS}"
                        "${CAPSID_WPT_ROOT}/interfaces/encoding.idl"
                        "${CAPSID_WPT_ROOT}/interfaces/streams.idl")
                elseif(CAPSID_WPT_PATH STREQUAL
                       "url/url-constructor.any.js")
                    set(CAPSID_WPT_SOURCE
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-combined.js")
                    set(CAPSID_WPT_RESOURCE_MAP
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-resources.js")
                    set(CAPSID_WPT_URL_RESOURCES
                        "url/resources/urltestdata.json"
                        "url/resources/urltestdata-javascript-only.json")
                    set(CAPSID_WPT_URL_RESOURCE_DEPS)
                    foreach(CAPSID_WPT_RESOURCE IN LISTS CAPSID_WPT_URL_RESOURCES)
                        list(APPEND CAPSID_WPT_URL_RESOURCE_DEPS
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_RESOURCE}")
                    endforeach()
                    add_custom_command(
                        OUTPUT
                            "${CAPSID_WPT_SOURCE}"
                            "${CAPSID_WPT_RESOURCE_MAP}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/generate-resource-map.mjs"
                            "${CAPSID_WPT_ROOT}"
                            "${CAPSID_WPT_RESOURCE_MAP}"
                            ${CAPSID_WPT_URL_RESOURCES}
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_SOURCE}"
                            "${CAPSID_WPT_ROOT}/common/subset-tests-by-key.js"
                            "${CAPSID_WPT_RESOURCE_MAP}"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        DEPENDS
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/generate-resource-map.mjs"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_ROOT}/common/subset-tests-by-key.js"
                            ${CAPSID_WPT_URL_RESOURCE_DEPS}
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        VERBATIM
                    )
                    list(APPEND CAPSID_WPT_SOURCE_DEPS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/generate-resource-map.mjs"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                        "${CAPSID_WPT_ROOT}/common/subset-tests-by-key.js"
                        ${CAPSID_WPT_URL_RESOURCE_DEPS})
                elseif(CAPSID_WPT_PATH STREQUAL
                       "FileAPI/blob/Blob-constructor.any.js")
                    set(CAPSID_WPT_SOURCE
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-combined.js")
                    set(CAPSID_WPT_BLOB_SUPPORT
                        "${CAPSID_WPT_ROOT}/FileAPI/support/Blob.js")
                    add_custom_command(
                        OUTPUT "${CAPSID_WPT_SOURCE}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_SOURCE}"
                            "${CAPSID_WPT_BLOB_SUPPORT}"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        DEPENDS
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_BLOB_SUPPORT}"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        VERBATIM
                    )
                    list(APPEND CAPSID_WPT_SOURCE_DEPS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                        "${CAPSID_WPT_BLOB_SUPPORT}")
                elseif(CAPSID_WPT_PATH STREQUAL
                       "compression/compression-stream.any.js")
                    set(CAPSID_WPT_SOURCE
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-combined.js")
                    set(CAPSID_WPT_RESOURCE_MAP
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-resources.js")
                    set(CAPSID_WPT_COMPRESSION_RESOURCES
                        "media/foo.vtt"
                        "media/test-av-384k-44100Hz-1ch-320x240-30fps-10kfr.webm")
                    set(CAPSID_WPT_COMPRESSION_RESOURCE_DEPS)
                    foreach(CAPSID_WPT_RESOURCE IN LISTS
                            CAPSID_WPT_COMPRESSION_RESOURCES)
                        list(APPEND CAPSID_WPT_COMPRESSION_RESOURCE_DEPS
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_RESOURCE}")
                    endforeach()
                    add_custom_command(
                        OUTPUT
                            "${CAPSID_WPT_SOURCE}"
                            "${CAPSID_WPT_RESOURCE_MAP}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/generate-resource-map.mjs"
                            "${CAPSID_WPT_ROOT}"
                            "${CAPSID_WPT_RESOURCE_MAP}"
                            ${CAPSID_WPT_COMPRESSION_RESOURCES}
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_SOURCE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/compression-decompress.js"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/compression-formats-profile.js"
                            "${CAPSID_WPT_RESOURCE_MAP}"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        DEPENDS
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/generate-resource-map.mjs"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/compression-decompress.js"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/compression-formats-profile.js"
                            ${CAPSID_WPT_COMPRESSION_RESOURCE_DEPS}
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        VERBATIM
                    )
                    list(APPEND CAPSID_WPT_SOURCE_DEPS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/generate-resource-map.mjs"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/compression-decompress.js"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/compression-formats-profile.js"
                        ${CAPSID_WPT_COMPRESSION_RESOURCE_DEPS})
                elseif(CAPSID_WPT_PATH MATCHES
                       "^wasm/(jsapi/(constructor/(compile|instantiate|instantiate-bad-imports|validate)|memory/(constructor|grow)|table/(constructor|grow)|global/(constructor|value-get-set))|webapi/instantiateStreaming(-bad-imports)?)\\.any\\.js$")
                    set(CAPSID_WPT_SOURCE
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-combined.js")
                    set(CAPSID_WPT_WASM_SUPPORT)
                    if(CAPSID_WPT_PATH MATCHES
                       "^wasm/jsapi/constructor/(compile|validate)\\.any\\.js$")
                        list(APPEND CAPSID_WPT_WASM_SUPPORT
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/wasm-module-builder.js")
                    elseif(CAPSID_WPT_PATH MATCHES
                           "^wasm/(jsapi/constructor/instantiate-bad-imports|webapi/instantiateStreaming-bad-imports)\\.any\\.js$")
                        list(APPEND CAPSID_WPT_WASM_SUPPORT
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/wasm-module-builder.js"
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/bad-imports.js")
                    elseif(CAPSID_WPT_PATH STREQUAL
                           "wasm/jsapi/constructor/instantiate.any.js")
                        list(APPEND CAPSID_WPT_WASM_SUPPORT
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/wasm-module-builder.js"
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/assertions.js"
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/instanceTestFactory.js")
                    elseif(CAPSID_WPT_PATH STREQUAL
                           "wasm/jsapi/memory/constructor.any.js")
                        list(APPEND CAPSID_WPT_WASM_SUPPORT
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/assertions.js"
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/memory/assertions.js")
                    elseif(CAPSID_WPT_PATH STREQUAL
                           "wasm/jsapi/table/constructor.any.js")
                        list(APPEND CAPSID_WPT_WASM_SUPPORT
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/wasm-module-builder.js"
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/assertions.js"
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/table/assertions.js")
                    elseif(CAPSID_WPT_PATH STREQUAL
                           "wasm/jsapi/table/grow.any.js")
                        list(APPEND CAPSID_WPT_WASM_SUPPORT
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/wasm-module-builder.js"
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/table/assertions.js")
                    elseif(CAPSID_WPT_PATH STREQUAL
                           "wasm/jsapi/global/constructor.any.js")
                        list(APPEND CAPSID_WPT_WASM_SUPPORT
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/assertions.js")
                    elseif(CAPSID_WPT_PATH STREQUAL
                           "wasm/webapi/instantiateStreaming.any.js")
                        list(APPEND CAPSID_WPT_WASM_SUPPORT
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/wasm-module-builder.js"
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/assertions.js"
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/instanceTestFactory.js")
                    else()
                        list(APPEND CAPSID_WPT_WASM_SUPPORT
                            "${CAPSID_WPT_ROOT}/wasm/jsapi/memory/assertions.js")
                    endif()
                    add_custom_command(
                        OUTPUT "${CAPSID_WPT_SOURCE}"
                        COMMAND node
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            "${CAPSID_WPT_SOURCE}"
                            ${CAPSID_WPT_WASM_SUPPORT}
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        DEPENDS
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                            ${CAPSID_WPT_WASM_SUPPORT}
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        VERBATIM
                    )
                    list(APPEND CAPSID_WPT_SOURCE_DEPS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/concat-sources.mjs"
                        ${CAPSID_WPT_WASM_SUPPORT})
                endif()
                set(CAPSID_WPT_ENV
                    "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-env.js")
                set(CAPSID_WPT_ENV_SOURCE
                    "Object.defineProperty(globalThis, 'location', { configurable: true, value: Object.freeze({ href: '${CAPSID_WPT_SOURCE_URL_${CAPSID_WPT_TEST_ID}}' }) });\n")
                if(CAPSID_WPT_PATH MATCHES "tchinese/big5")
                    string(APPEND CAPSID_WPT_ENV_SOURCE
                        "globalThis.__wptEncoding = 'big5';\n")
                elseif(CAPSID_WPT_PATH MATCHES "japanese/euc-jp")
                    string(APPEND CAPSID_WPT_ENV_SOURCE
                        "globalThis.__wptEncoding = 'euc-jp';\n")
                elseif(CAPSID_WPT_PATH MATCHES "japanese/shift_jis")
                    string(APPEND CAPSID_WPT_ENV_SOURCE
                        "globalThis.__wptEncoding = 'shift_jis';\n")
                elseif(CAPSID_WPT_PATH MATCHES "korean/euc-kr")
                    string(APPEND CAPSID_WPT_ENV_SOURCE
                        "globalThis.__wptEncoding = 'euc-kr';\n")
                endif()
                if(CAPSID_WPT_PATH STREQUAL
                   "encoding/idlharness.any.js")
                    string(APPEND CAPSID_WPT_ENV_SOURCE
                        "class DedicatedWorkerGlobalScope { static [Symbol.hasInstance](value) { return value === globalThis; } }\nObject.defineProperty(globalThis, 'DedicatedWorkerGlobalScope', { configurable: true, value: DedicatedWorkerGlobalScope });\n")
                endif()
                if(CAPSID_WPT_PATH STREQUAL
                   "encoding/single-byte-decoder.any.js")
                    set(CAPSID_WPT_ENV_SOURCE
                        "Object.defineProperty(globalThis, 'location', { configurable: true, value: Object.freeze({ href: '${CAPSID_WPT_SOURCE_URL_${CAPSID_WPT_TEST_ID}}?TextDecoder', search: '?TextDecoder' }) });\n")
                endif()
                if(CAPSID_WPT_PATH STREQUAL
                   "encoding/textdecoder-fatal-single-byte.any.js")
                    string(APPEND CAPSID_WPT_ENV_SOURCE
                        "Object.defineProperty(globalThis, 'subsetTest', { configurable: true, value(harness, callback, name) { return harness(callback, name); } });\n")
                endif()
                if(CAPSID_WPT_PATH STREQUAL
                   "encoding/api-invalid-label.any.js")
                    string(APPEND CAPSID_WPT_ENV_SOURCE
                        "Object.defineProperty(globalThis, 'subsetTest', { configurable: true, value(harness, callback, name) { return harness(callback, name); } });\n")
                endif()
                set(CAPSID_EXPECTED_VARIABLE
                    "CAPSID_WPT_EXPECTED_FAILURES_${CAPSID_WPT_TEST_ID}")
                if(DEFINED ${CAPSID_EXPECTED_VARIABLE})
                    string(APPEND CAPSID_WPT_ENV_SOURCE
                        "Object.defineProperty(globalThis, '__wptExpectedFailures', { configurable: true, value: Object.freeze([${${CAPSID_EXPECTED_VARIABLE}}]) });\n")
                endif()
                if(CAPSID_WPT_PATH STREQUAL
                   "html/webappapis/scripting/processing-model-2/unhandled-promise-rejections/support/promise-rejection-events.js")
                    string(APPEND CAPSID_WPT_ENV_SOURCE
                        "Object.defineProperty(globalThis, 'createImageBitmap', { configurable: true, value() { return Promise.reject(new DOMException('', 'InvalidStateError')); } });\n")
                endif()
                file(GENERATE
                    OUTPUT "${CAPSID_WPT_ENV}"
                    CONTENT "${CAPSID_WPT_ENV_SOURCE}")
                set(CAPSID_WPT_ENTRY
                    "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-entry.js")
                string(CONCAT CAPSID_WPT_ENTRY_SOURCE
                    "import '${CAPSID_WPT_ENV}';\n"
                    "import '${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/adapter.js';\n"
                    "import '${CAPSID_WPT_SOURCE}';\n"
                    "import app from '${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/app.js';\n"
                    "globalThis.__wptSeal();\n"
                    "export default app;\n")
                file(GENERATE
                    OUTPUT "${CAPSID_WPT_ENTRY}"
                    CONTENT "${CAPSID_WPT_ENTRY_SOURCE}")

                set(CAPSID_WPT_FIXTURE
                    "${CAPSID_GENERATED_DIR}/test-wpt-${CAPSID_WPT_TEST_ID}.js")
                add_custom_command(
                    OUTPUT "${CAPSID_WPT_FIXTURE}"
                    COMMAND "${CAPSID_ESBUILD}"
                        "${CAPSID_WPT_ENTRY}"
                        --bundle
                        --target=esnext
                        --platform=neutral
                        --format=esm
                        "--outfile=${CAPSID_WPT_FIXTURE}"
                    DEPENDS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/adapter.js"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/app.js"
                        "${CAPSID_WPT_ENV}"
                        ${CAPSID_WPT_SOURCE_DEPS}
                        "${CAPSID_WPT_SOURCE}"
                        "${CAPSID_WPT_ENTRY}"
                    VERBATIM
                )
                set(CAPSID_WPT_FIXTURE_TARGET
                    "test-wpt-${CAPSID_WPT_TEST_ID}-fixture")
                add_custom_target("${CAPSID_WPT_FIXTURE_TARGET}"
                    DEPENDS "${CAPSID_WPT_FIXTURE}")
                list(APPEND CAPSID_WPT_FIXTURE_TARGETS
                    "${CAPSID_WPT_FIXTURE_TARGET}")
                list(APPEND CAPSID_WPT_TEST_IDS "${CAPSID_WPT_TEST_ID}")
                set("CAPSID_WPT_FIXTURE_${CAPSID_WPT_TEST_ID}"
                    "${CAPSID_WPT_FIXTURE}")
            endforeach()
        endif()

        find_package(Threads REQUIRED)
        add_executable(
            test-worker-integration
            tests/test_worker_integration.cc
            tests/wpt_report.cc)
        target_include_directories(test-worker-integration PRIVATE tests)
        target_link_libraries(
            test-worker-integration
            PRIVATE capsid_runtime Threads::Threads
        )
        add_dependencies(
            test-worker-integration
            test-fetch-fixture
            test-global-surface-fixture
            test-p1-platform-fixture
            test-wasm-fixture
            ${CAPSID_WASM_EDGE_FIXTURE_TARGETS}
        )
        if(CAPSID_WPT_ROOT)
            add_dependencies(
                test-worker-integration
                ${CAPSID_WPT_FIXTURE_TARGETS}
            )
        endif()
        add_test(
            NAME worker_global_surface
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_GLOBAL_SURFACE_FIXTURE}"
        )
        set_tests_properties(worker_global_surface PROPERTIES TIMEOUT 20)
        add_executable(
            test-worker-end-after-response
            tests/test_worker_end_after_response.cc)
        target_include_directories(test-worker-end-after-response PRIVATE tests)
        target_link_libraries(
            test-worker-end-after-response
            PRIVATE capsid_runtime
        )
        add_test(
            NAME worker_end_after_response
            COMMAND test-worker-end-after-response
                $<TARGET_FILE:capsid-worker>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ipc-sync-response.js"
        )
        set_tests_properties(worker_end_after_response PROPERTIES TIMEOUT 20)
        add_test(
            NAME worker_bodyless_end_failure
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ipc-sync-response.js"
                bodyless-end-failure
        )
        set_tests_properties(worker_bodyless_end_failure PROPERTIES TIMEOUT 20)
        add_test(
            NAME worker_wasm_minimal
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_WASM_FIXTURE}"
                wasm
        )
        set_tests_properties(worker_wasm_minimal PROPERTIES TIMEOUT 20)
        foreach(CAPSID_WASM_EDGE_TEST_ID IN LISTS CAPSID_WASM_EDGE_TEST_IDS)
            set(CAPSID_WASM_EDGE_FIXTURE_VAR
                "CAPSID_WASM_EDGE_FIXTURE_${CAPSID_WASM_EDGE_TEST_ID}")
            add_test(
                NAME "worker_wasm_${CAPSID_WASM_EDGE_TEST_ID}"
                COMMAND test-worker-integration
                    $<TARGET_FILE:capsid-worker>
                    "${${CAPSID_WASM_EDGE_FIXTURE_VAR}}"
                    wasm
            )
            set_tests_properties(
                "worker_wasm_${CAPSID_WASM_EDGE_TEST_ID}"
                PROPERTIES TIMEOUT 20
            )
        endforeach()
        add_test(
            NAME wasm_resource_limit_constants
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_WASM_C_SOURCE=${CAPSID_TXIKI_OVERLAY}/src/wasm.c"
                "-DCAPSID_WASM_JS_SOURCE=${CAPSID_TXIKI_OVERLAY}/src/js/polyfills/wasm.js"
                "-DCAPSID_WASM_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm-minimal.js"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuditWasmLimits.cmake"
        )
        set_tests_properties(wasm_resource_limit_constants PROPERTIES TIMEOUT 30)
        add_test(
            NAME wasm_resource_limit_negative_controls
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_WASM_AUDIT_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuditWasmLimits.cmake"
                "-DCAPSID_WASM_C_SOURCE=${CAPSID_TXIKI_OVERLAY}/src/wasm.c"
                "-DCAPSID_WASM_JS_SOURCE=${CAPSID_TXIKI_OVERLAY}/src/js/polyfills/wasm.js"
                "-DCAPSID_WASM_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm-minimal.js"
                "-DCAPSID_TEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/wasm-limits-negative"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestWasmLimitsNegative.cmake"
        )
        set_tests_properties(
            wasm_resource_limit_negative_controls PROPERTIES TIMEOUT 30)
        add_test(
            NAME wpt_metadata_manifest
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/audit-metadata.mjs"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/manifest.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/docs/conformance-deviations.md"
        )
        add_test(
            NAME wpt_metadata_negative_controls
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/audit-metadata.test.mjs"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/manifest.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/docs/conformance-deviations.md"
        )
        set_tests_properties(
            wpt_metadata_manifest
            wpt_metadata_negative_controls
            PROPERTIES TIMEOUT 30 LABELS "conformance")
        add_test(
            NAME txiki_vendor_patch_integrity
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_TXIKI_VENDOR=${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js"
                "-DCAPSID_TXIKI_PATCH_DIR=${CMAKE_CURRENT_SOURCE_DIR}/patches/txiki"
                "-DCAPSID_TXIKI_EXPECTED_TAG=v26.6.0"
                "-DCAPSID_TXIKI_OVERLAY_STAMP=${CAPSID_OVERLAY_STAMP}"
                "-DCAPSID_TXIKI_PREPARE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareTxiki.cmake"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuditTxikiVendor.cmake"
        )
        set_tests_properties(
            txiki_vendor_patch_integrity PROPERTIES TIMEOUT 60
        )
        add_test(
            NAME txiki_overlay_audit_negative_controls
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_TXIKI_AUDIT_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuditTxikiVendor.cmake"
                "-DCAPSID_TXIKI_VENDOR=${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js"
                "-DCAPSID_TXIKI_PATCH_DIR=${CMAKE_CURRENT_SOURCE_DIR}/patches/txiki"
                "-DCAPSID_TXIKI_OVERLAY_STAMP=${CAPSID_OVERLAY_STAMP}"
                "-DCAPSID_TXIKI_PREPARE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareTxiki.cmake"
                "-DCAPSID_TEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/txiki-overlay-audit"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestTxikiOverlayAudit.cmake"
        )
        set_tests_properties(
            txiki_overlay_audit_negative_controls PROPERTIES TIMEOUT 120
        )
        add_test(
            NAME txiki_overlay_key_canonicalization
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_TXIKI_KEY_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/ComputeTxikiOverlayKey.cmake"
                "-DCAPSID_TXIKI_VENDOR=${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js"
                "-DCAPSID_TXIKI_PATCH_DIR=${CMAKE_CURRENT_SOURCE_DIR}/patches/txiki"
                "-DCAPSID_TXIKI_PREPARE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareTxiki.cmake"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestTxikiOverlayKey.cmake"
        )
        set_tests_properties(
            txiki_overlay_key_canonicalization PROPERTIES TIMEOUT 30
        )

        # CMake exposes the authoritative reconfiguration inputs through the
        # directory's CMAKE_CONFIGURE_DEPENDS property.  Persist a normalized
        # snapshot for the audit instead of depending on generator-specific
        # implementation files such as CMakeFiles/Makefile.cmake, which does
        # not exist when using Ninja.
        get_property(
            CAPSID_CONFIGURE_DEPENDENCIES
            DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            PROPERTY CMAKE_CONFIGURE_DEPENDS
        )
        set(CAPSID_CONFIGURE_DEPENDENCIES_NORMALIZED "")
        foreach(CAPSID_CONFIGURE_DEPENDENCY IN LISTS
                CAPSID_CONFIGURE_DEPENDENCIES)
            get_filename_component(
                CAPSID_CONFIGURE_DEPENDENCY_ABSOLUTE
                "${CAPSID_CONFIGURE_DEPENDENCY}"
                ABSOLUTE
                BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
            )
            file(TO_CMAKE_PATH
                "${CAPSID_CONFIGURE_DEPENDENCY_ABSOLUTE}"
                CAPSID_CONFIGURE_DEPENDENCY_NORMALIZED
            )
            list(APPEND CAPSID_CONFIGURE_DEPENDENCIES_NORMALIZED
                "${CAPSID_CONFIGURE_DEPENDENCY_NORMALIZED}")
        endforeach()
        list(REMOVE_DUPLICATES CAPSID_CONFIGURE_DEPENDENCIES_NORMALIZED)
        list(SORT CAPSID_CONFIGURE_DEPENDENCIES_NORMALIZED)
        list(JOIN CAPSID_CONFIGURE_DEPENDENCIES_NORMALIZED "\n"
            CAPSID_CONFIGURE_DEPENDENCIES_CONTENT)
        set(CAPSID_CONFIGURE_DEPENDENCIES_FILE
            "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/CapsidConfigureDepends.txt")
        file(WRITE "${CAPSID_CONFIGURE_DEPENDENCIES_FILE}"
            "${CAPSID_CONFIGURE_DEPENDENCIES_CONTENT}\n")

        add_test(
            NAME txiki_overlay_configure_dependencies
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_CMAKE_CONFIGURE_DEPENDS=${CAPSID_CONFIGURE_DEPENDENCIES_FILE}"
                "-DCAPSID_CMAKE_VERIFY_GLOBS=${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/VerifyGlobs.cmake"
                "-DCAPSID_TXIKI_VENDOR=${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js"
                "-DCAPSID_TXIKI_PATCH_DIR=${CMAKE_CURRENT_SOURCE_DIR}/patches/txiki"
                "-DCAPSID_TXIKI_PREPARE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareTxiki.cmake"
                "-DCAPSID_TXIKI_KEY_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/ComputeTxikiOverlayKey.cmake"
                "-DCAPSID_CAPABILITY_MANIFEST=${CAPSID_CAPABILITY_MANIFEST}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestTxikiOverlayConfigureDependencies.cmake"
        )
        set_tests_properties(
            txiki_overlay_configure_dependencies PROPERTIES TIMEOUT 30
        )
        add_test(
            NAME worker_fetch_direct_egress
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_FETCH_FIXTURE}"
                fetch
        )
        set_tests_properties(worker_fetch_direct_egress PROPERTIES TIMEOUT 20)
        add_test(
            NAME worker_p1_platform_contract
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_P1_PLATFORM_FIXTURE}"
                platform
        )
        set_tests_properties(worker_p1_platform_contract PROPERTIES TIMEOUT 20)
        if(CAPSID_WPT_ROOT)
            foreach(CAPSID_WPT_TEST_ID IN LISTS CAPSID_WPT_TEST_IDS)
                set(CAPSID_WPT_TEST_NAME "worker_wpt_${CAPSID_WPT_TEST_ID}")
                add_test(
                    NAME "${CAPSID_WPT_TEST_NAME}"
                    COMMAND test-worker-integration
                        $<TARGET_FILE:capsid-worker>
                        "${CAPSID_WPT_FIXTURE_${CAPSID_WPT_TEST_ID}}"
                        wpt
                        "${CAPSID_WPT_SOURCE_URL_${CAPSID_WPT_TEST_ID}}"
                )
                set_tests_properties(
                    "${CAPSID_WPT_TEST_NAME}"
                    PROPERTIES TIMEOUT 20
                )
            endforeach()

            # Guard against the executed batch silently drifting away from the
            # manifest that the conformance documentation cites as evidence.
            list(LENGTH CAPSID_WPT_TEST_IDS CAPSID_WPT_REGISTERED_COUNT)
            add_test(
                NAME wpt_coverage_manifest
                COMMAND "${CMAKE_COMMAND}"
                    "-DCAPSID_WPT_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/manifest.json"
                    "-DCAPSID_WPT_REGISTERED_COUNT=${CAPSID_WPT_REGISTERED_COUNT}"
                    "-DCAPSID_WPT_REGISTERED_PATHS_FILE=${CAPSID_WPT_REGISTERED_PATHS_FILE}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuditWptCoverage.cmake"
            )
            set_tests_properties(wpt_coverage_manifest PROPERTIES TIMEOUT 60)
            add_test(
                NAME wpt_coverage_audit_rejects_equal_count_substitution
                COMMAND "${CMAKE_COMMAND}"
                    "-DCAPSID_WPT_AUDIT_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuditWptCoverage.cmake"
                    "-DCAPSID_WPT_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/manifest.json"
                    "-DCAPSID_TEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/wpt-coverage-audit"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestWptCoverageAudit.cmake"
            )
            set_tests_properties(
                wpt_coverage_audit_rejects_equal_count_substitution
                PROPERTIES TIMEOUT 60
            )
            add_test(
                NAME wpt_coverage_audit_rejects_missing_paths_file
                COMMAND "${CMAKE_COMMAND}"
                    "-DCAPSID_WPT_AUDIT_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuditWptCoverage.cmake"
                    "-DCAPSID_WPT_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/manifest.json"
                    "-DCAPSID_TEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/wpt-missing-paths-file"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestWptCoverageMissingPathsFile.cmake"
            )
            set_tests_properties(
                wpt_coverage_audit_rejects_missing_paths_file
                PROPERTIES TIMEOUT 60
            )
        else()
            # WPT is the ONLY source of evidence for the ECMA-429 conformance
            # claims in docs/standards-matrix.md and docs/conformance-deviations.md.
            # If it silently does not run, `ctest` reports all-green while the
            # conformance surface is entirely untested. Register a test that fails
            # loudly so that absence of coverage can never be mistaken for a pass.
            add_test(
                NAME wpt_conformance_not_configured
                COMMAND "${CMAKE_COMMAND}" -E false
            )
            set_tests_properties(wpt_conformance_not_configured PROPERTIES
                TIMEOUT 10
                LABELS "conformance"
                SKIP_RETURN_CODE 77
            )
        endif()

        add_executable(test-module-denial tests/test_module_denial.cc)
        target_link_libraries(test-module-denial PRIVATE capsid_runtime)
        add_test(
            NAME worker_denies_public_native_module
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "unauthorized=capsid:fs"
        )
        add_test(
            NAME worker_denies_internal_native_module
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "tjs:internal/core"
        )

        # Exercise every concrete denied module category in the machine-readable
        # capability manifest against a real worker. This prevents the generic
        # restricted loader from regressing one module while the two historical
        # representative tests remain green.
        file(READ
            "${CAPSID_CAPABILITY_MANIFEST}"
            CAPSID_CAPABILITY_MANIFEST_JSON)
        add_test(
            NAME utility_module_manifest
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/audit-utility-module-manifest.mjs"
                "${CMAKE_CURRENT_SOURCE_DIR}"
                "${CAPSID_CAPABILITY_MANIFEST}"
        )
        set_tests_properties(
            utility_module_manifest
            PROPERTIES TIMEOUT 20 LABELS "capability;audit"
        )
        set(CAPSID_DENIED_MODULE_ARGUMENTS)
        string(JSON CAPSID_AVAILABLE_MODULE_COUNT
            LENGTH "${CAPSID_CAPABILITY_MANIFEST_JSON}"
            modules built_and_available)
        math(EXPR CAPSID_AVAILABLE_MODULE_LAST
            "${CAPSID_AVAILABLE_MODULE_COUNT} - 1")
        foreach(CAPSID_MODULE_INDEX RANGE 0 ${CAPSID_AVAILABLE_MODULE_LAST})
            string(JSON CAPSID_MODULE GET
                "${CAPSID_CAPABILITY_MANIFEST_JSON}"
                modules built_and_available ${CAPSID_MODULE_INDEX})
            list(APPEND CAPSID_DENIED_MODULE_ARGUMENTS
                "unauthorized=${CAPSID_MODULE}")
        endforeach()

        string(JSON CAPSID_UNAVAILABLE_MODULE_COUNT
            LENGTH "${CAPSID_CAPABILITY_MANIFEST_JSON}"
            modules known_but_not_built)
        math(EXPR CAPSID_UNAVAILABLE_MODULE_LAST
            "${CAPSID_UNAVAILABLE_MODULE_COUNT} - 1")
        foreach(CAPSID_MODULE_INDEX RANGE 0 ${CAPSID_UNAVAILABLE_MODULE_LAST})
            string(JSON CAPSID_MODULE GET
                "${CAPSID_CAPABILITY_MANIFEST_JSON}"
                modules known_but_not_built ${CAPSID_MODULE_INDEX})
            list(APPEND CAPSID_DENIED_MODULE_ARGUMENTS
                "unavailable=${CAPSID_MODULE}")
        endforeach()

        string(JSON CAPSID_FORBIDDEN_MODULE_COUNT
            LENGTH "${CAPSID_CAPABILITY_MANIFEST_JSON}"
            modules permanently_forbidden)
        math(EXPR CAPSID_FORBIDDEN_MODULE_LAST
            "${CAPSID_FORBIDDEN_MODULE_COUNT} - 1")
        foreach(CAPSID_MODULE_INDEX RANGE 0 ${CAPSID_FORBIDDEN_MODULE_LAST})
            string(JSON CAPSID_MODULE GET
                "${CAPSID_CAPABILITY_MANIFEST_JSON}"
                modules permanently_forbidden ${CAPSID_MODULE_INDEX})
            if(CAPSID_MODULE STREQUAL "data:")
                set(CAPSID_MODULE
                    "data:text/javascript,export default 1")
            elseif(CAPSID_MODULE STREQUAL "file:")
                set(CAPSID_MODULE "file:///tmp/capsid-denied.mjs")
            elseif(CAPSID_MODULE STREQUAL "http:")
                set(CAPSID_MODULE "http://denied.invalid/app.mjs")
            elseif(CAPSID_MODULE STREQUAL "https:")
                set(CAPSID_MODULE "https://denied.invalid/app.mjs")
            elseif(CAPSID_MODULE STREQUAL "node:")
                set(CAPSID_MODULE "node:fs")
            elseif(CAPSID_MODULE STREQUAL
                   "relative-or-absolute-path-import")
                set(CAPSID_MODULE "./capsid-denied.mjs")
            else()
                string(REPLACE "*" "probe" CAPSID_MODULE "${CAPSID_MODULE}")
            endif()
            list(APPEND CAPSID_DENIED_MODULE_ARGUMENTS
                "forbidden=${CAPSID_MODULE}")
        endforeach()
        add_test(
            NAME worker_denies_capability_manifest_modules
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                ${CAPSID_DENIED_MODULE_ARGUMENTS}
        )
        set_tests_properties(
            worker_denies_capability_manifest_modules
            PROPERTIES TIMEOUT 180 LABELS "capability;sandbox")

        add_test(
            NAME worker_hono_excludes_node_adapter
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "@hono/node-server"
        )
        add_test(
            NAME worker_hono_excludes_bun_adapter
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "hono/adapter/bun"
        )
        add_test(
            NAME worker_hono_excludes_deno_adapter
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "hono/adapter/deno"
        )
        add_test(
            NAME worker_hono_excludes_cloudflare_adapter
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "hono/adapter/cloudflare-workers"
        )
        add_test(
            NAME worker_hono_excludes_websocket_adapter
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "hono/adapter/bun/websocket"
        )
        add_test(
            NAME worker_hono_excludes_static_file_adapter
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "hono/adapter/bun/serve-static"
        )
        add_test(
            NAME worker_hono_excludes_node_builtin
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "node:http"
        )
        add_test(
            NAME worker_hono_excludes_file_module
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "file:///tmp/hono-adapter.js"
        )
        add_test(
            NAME worker_hono_excludes_remote_module
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "https://example.test/hono-adapter.js"
        )
        add_test(
            NAME worker_itty_router_excludes_node_http
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "node:http"
        )
        add_test(
            NAME worker_itty_router_excludes_node_fs
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "node:fs"
        )
        add_test(
            NAME worker_itty_router_excludes_node_adapter
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "itty-router/node"
        )
        add_test(
            NAME worker_itty_router_excludes_bun_adapter
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "itty-router/bun"
        )
        add_test(
            NAME worker_itty_router_excludes_websocket_adapter
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "itty-router/websocket"
        )
        add_test(
            NAME worker_itty_router_excludes_file_module
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "file:///tmp/itty-router-adapter.js"
        )
        add_test(
            NAME worker_itty_router_excludes_remote_module
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "https://example.test/itty-router-adapter.js"
        )
        add_test(
            NAME worker_h3_v2_excludes_node_entry
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "h3/node"
        )
        add_test(
            NAME worker_h3_v2_excludes_srvx_listener
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "srvx"
        )
        add_test(
            NAME worker_h3_v2_excludes_crossws_server
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "crossws/server"
        )
        add_test(
            NAME worker_h3_v2_excludes_node_http
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "node:http"
        )
        add_test(
            NAME worker_h3_v2_excludes_node_fs
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "node:fs"
        )
        add_test(
            NAME worker_h3_v2_excludes_external_module
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "unbundled-h3-dependency"
        )
        add_test(
            NAME worker_h3_v2_excludes_file_module
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "file:///tmp/h3-listener.js"
        )
        add_test(
            NAME worker_h3_v2_excludes_remote_module
            COMMAND test-module-denial
                $<TARGET_FILE:capsid-worker>
                "https://example.test/h3-listener.js"
        )
        set_tests_properties(
            worker_denies_public_native_module
            worker_denies_internal_native_module
            PROPERTIES TIMEOUT 20
        )
        set_tests_properties(
            worker_hono_excludes_node_adapter
            worker_hono_excludes_bun_adapter
            worker_hono_excludes_deno_adapter
            worker_hono_excludes_cloudflare_adapter
            worker_hono_excludes_websocket_adapter
            worker_hono_excludes_static_file_adapter
            worker_hono_excludes_node_builtin
            worker_hono_excludes_file_module
            worker_hono_excludes_remote_module
            PROPERTIES
                TIMEOUT 20
                LABELS "framework;hono;hono-excluded"
        )
        set_tests_properties(
            worker_itty_router_excludes_node_http
            worker_itty_router_excludes_node_fs
            worker_itty_router_excludes_node_adapter
            worker_itty_router_excludes_bun_adapter
            worker_itty_router_excludes_websocket_adapter
            worker_itty_router_excludes_file_module
            worker_itty_router_excludes_remote_module
            PROPERTIES
                TIMEOUT 20
                LABELS "framework;itty-router;itty-router-excluded"
        )
        set_tests_properties(
            worker_h3_v2_excludes_node_entry
            worker_h3_v2_excludes_srvx_listener
            worker_h3_v2_excludes_crossws_server
            worker_h3_v2_excludes_node_http
            worker_h3_v2_excludes_node_fs
            worker_h3_v2_excludes_external_module
            worker_h3_v2_excludes_file_module
            worker_h3_v2_excludes_remote_module
            PROPERTIES
                TIMEOUT 20
                LABELS "framework;h3-v2;h3-v2-excluded"
        )

        add_executable(
            test-permissions-integration
            tests/test_permissions_integration.cc
        )
        target_link_libraries(
            test-permissions-integration PRIVATE capsid_runtime
        )
        add_test(
            NAME worker_permissions_contract
            COMMAND test-permissions-integration
                $<TARGET_FILE:capsid-worker>
        )
        add_test(
            NAME worker_utility_modules_contract
            COMMAND test-permissions-integration
                $<TARGET_FILE:capsid-worker>
                --utility-modules
        )
        add_test(
            NAME worker_environment_module_contract
            COMMAND test-permissions-integration
                $<TARGET_FILE:capsid-worker>
                --environment
        )
        add_test(
            NAME worker_system_module_contract
            COMMAND test-permissions-integration
                $<TARGET_FILE:capsid-worker>
                --system
        )
        add_test(
            NAME worker_storage_module_contract
            COMMAND test-permissions-integration
                $<TARGET_FILE:capsid-worker>
                --storage
        )
        add_test(
            NAME worker_stdio_module_contract
            COMMAND test-permissions-integration
                $<TARGET_FILE:capsid-worker>
                --stdio
        )
        add_test(
            NAME worker_fs_module_contract
            COMMAND test-permissions-integration
                $<TARGET_FILE:capsid-worker>
                --fs
        )
        set_tests_properties(
            worker_permissions_contract
            worker_utility_modules_contract
            worker_environment_module_contract
            worker_system_module_contract
            worker_storage_module_contract
            worker_stdio_module_contract
            worker_fs_module_contract
            PROPERTIES TIMEOUT 45 LABELS "capability;sandbox"
        )
        add_test(
            NAME escape_capability_defaults
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_CACHE=${CMAKE_BINARY_DIR}/CMakeCache.txt"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuditEscapeCapabilityDefaults.cmake"
        )
        add_test(
            NAME escape_capability_configure_negative_controls
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DCAPSID_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/Testing/escape-capability-configure"
                "-DCAPSID_GENERATOR=${CMAKE_GENERATOR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestEscapeCapabilityConfigure.cmake"
        )
        set_tests_properties(
            escape_capability_defaults
            escape_capability_configure_negative_controls
            PROPERTIES TIMEOUT 90 LABELS "capability;sandbox;audit"
        )

        set(CAPSID_RESTRICTED_AUDIT_EXPECT_LTO OFF)
        if(CAPSID_ENABLE_LTO AND CAPSID_IPO_SUPPORTED)
            set(CAPSID_RESTRICTED_AUDIT_EXPECT_LTO ON)
        endif()
        add_test(
            NAME worker_binary_audit
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_WORKER=$<TARGET_FILE:capsid-worker>"
                "-DCAPSID_TJS_ARCHIVE=$<TARGET_FILE:tjs>"
                "-DCAPSID_NM=${CMAKE_NM}"
                "-DCAPSID_AR=${CMAKE_AR}"
                "-DCAPSID_EXPECT_LTO=${CAPSID_RESTRICTED_AUDIT_EXPECT_LTO}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuditRestrictedWorker.cmake"
        )
        set_tests_properties(worker_binary_audit PROPERTIES TIMEOUT 120)
        set(CAPSID_RESTRICTED_AUDIT_CAN_INJECT OFF)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_OBJCOPY)
            set(CAPSID_RESTRICTED_AUDIT_CAN_INJECT ON)
        endif()
        add_test(
            NAME worker_binary_audit_negative_controls
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_AUDIT_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuditRestrictedWorker.cmake"
                "-DCAPSID_WORKER=$<TARGET_FILE:capsid-worker>"
                "-DCAPSID_TJS_ARCHIVE=$<TARGET_FILE:tjs>"
                "-DCAPSID_NM=${CMAKE_NM}"
                "-DCAPSID_AR=${CMAKE_AR}"
                "-DCAPSID_STRIP=${CMAKE_STRIP}"
                "-DCAPSID_OBJCOPY=${CMAKE_OBJCOPY}"
                "-DCAPSID_ENABLE_BINARY_INJECTION=${CAPSID_RESTRICTED_AUDIT_CAN_INJECT}"
                "-DCAPSID_EXPECT_LTO=${CAPSID_RESTRICTED_AUDIT_EXPECT_LTO}"
                "-DCAPSID_TEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/restricted-worker-audit"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestRestrictedWorkerAuditNegative.cmake"
        )
        set_tests_properties(
            worker_binary_audit_negative_controls PROPERTIES TIMEOUT 120)

        add_executable(test-p0-integration tests/test_p0_integration.cc)
        target_link_libraries(test-p0-integration PRIVATE capsid_runtime)
        add_dependencies(test-p0-integration test-p0-fixture)
        add_test(
            NAME worker_p0_contract
            COMMAND test-p0-integration
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_P0_FIXTURE}"
        )
        set_tests_properties(worker_p0_contract PROPERTIES TIMEOUT 20)

        add_executable(
            test-hono-worker-driver
            tests/test_framework_worker_driver.cc
        )
        target_link_libraries(
            test-hono-worker-driver PRIVATE capsid_runtime
        )
        add_dependencies(
            test-hono-worker-driver
            test-hono-DEFAULT-fixture
            test-hono-OBJECT-fixture
            test-hono-NAMED-fixture
        )
        add_test(
            NAME worker_hono_compatibility
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/hono/differential.mjs"
                --driver $<TARGET_FILE:test-hono-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_HONO_DEFAULT_FIXTURE}"
        )
        add_test(
            NAME worker_hono_entry_object
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/hono/differential.mjs"
                --driver $<TARGET_FILE:test-hono-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_HONO_OBJECT_FIXTURE}"
                --smoke true
        )
        add_test(
            NAME worker_hono_entry_named
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/hono/differential.mjs"
                --driver $<TARGET_FILE:test-hono-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_HONO_NAMED_FIXTURE}"
                --smoke true
        )
        add_test(
            NAME worker_hono_lifecycle
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/hono/lifecycle.mjs"
                --driver $<TARGET_FILE:test-hono-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_HONO_DEFAULT_FIXTURE}"
        )
        set_tests_properties(
            worker_hono_compatibility
            PROPERTIES TIMEOUT 120 LABELS "framework;hono"
        )
        set_tests_properties(
            worker_hono_entry_object
            worker_hono_entry_named
            PROPERTIES TIMEOUT 30 LABELS "framework;hono"
        )
        set_tests_properties(
            worker_hono_lifecycle
            PROPERTIES TIMEOUT 45 LABELS "framework;hono;lifecycle"
        )

        add_executable(
            test-itty-router-worker-driver
            tests/test_framework_worker_driver.cc
        )
        target_link_libraries(
            test-itty-router-worker-driver PRIVATE capsid_runtime
        )
        add_dependencies(
            test-itty-router-worker-driver
            test-itty-router-AUTOROUTER-fixture
            test-itty-router-ROUTER-fixture
            test-itty-router-ITTY-fixture
        )
        add_test(
            NAME worker_itty_router_autorouter
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/itty-router/differential.mjs"
                --driver $<TARGET_FILE:test-itty-router-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_ITTY_ROUTER_AUTOROUTER_FIXTURE}"
                --variant autorouter
        )
        add_test(
            NAME worker_itty_router_router
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/itty-router/differential.mjs"
                --driver $<TARGET_FILE:test-itty-router-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_ITTY_ROUTER_ROUTER_FIXTURE}"
                --variant router
        )
        add_test(
            NAME worker_itty_router_manual
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/itty-router/differential.mjs"
                --driver $<TARGET_FILE:test-itty-router-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_ITTY_ROUTER_ITTY_FIXTURE}"
                --variant itty-router
        )
        add_test(
            NAME worker_itty_router_autorouter_lifecycle
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/itty-router/lifecycle.mjs"
                --driver $<TARGET_FILE:test-itty-router-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_ITTY_ROUTER_AUTOROUTER_FIXTURE}"
                --variant autorouter
        )
        add_test(
            NAME worker_itty_router_router_lifecycle
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/itty-router/lifecycle.mjs"
                --driver $<TARGET_FILE:test-itty-router-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_ITTY_ROUTER_ROUTER_FIXTURE}"
                --variant router
        )
        add_test(
            NAME worker_itty_router_manual_lifecycle
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/itty-router/lifecycle.mjs"
                --driver $<TARGET_FILE:test-itty-router-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_ITTY_ROUTER_ITTY_FIXTURE}"
                --variant itty-router
        )
        set_tests_properties(
            worker_itty_router_autorouter
            worker_itty_router_router
            worker_itty_router_manual
            PROPERTIES TIMEOUT 120 LABELS "framework;itty-router"
        )
        set_tests_properties(
            worker_itty_router_autorouter_lifecycle
            worker_itty_router_router_lifecycle
            worker_itty_router_manual_lifecycle
            PROPERTIES
                TIMEOUT 45
                LABELS "framework;itty-router;lifecycle"
        )

        add_executable(
            test-h3-v2-worker-driver
            tests/test_framework_worker_driver.cc
        )
        target_link_libraries(
            test-h3-v2-worker-driver PRIVATE capsid_runtime
        )
        add_dependencies(
            test-h3-v2-worker-driver
            test-h3-v2-DEFAULT_APP-fixture
            test-h3-v2-DEFAULT_OBJECT-fixture
            test-h3-v2-NAMED-fixture
            test-h3-v2-WRAPPER-fixture
            test-h3-v2-HANDLER-fixture
            test-h3-v2-MALFORMED-fixture
            test-h3-v2-DEBUG-fixture
        )
        add_test(
            NAME worker_h3_v2_compatibility
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/h3-v2/differential.mjs"
                --driver $<TARGET_FILE:test-h3-v2-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_H3_V2_DEFAULT_APP_FIXTURE}"
        )
        add_test(
            NAME worker_h3_v2_entry_default_app
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/h3-v2/entry.mjs"
                --driver $<TARGET_FILE:test-h3-v2-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_H3_V2_DEFAULT_APP_FIXTURE}"
        )
        add_test(
            NAME worker_h3_v2_entry_default_object
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/h3-v2/entry.mjs"
                --driver $<TARGET_FILE:test-h3-v2-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_H3_V2_DEFAULT_OBJECT_FIXTURE}"
        )
        add_test(
            NAME worker_h3_v2_entry_named
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/h3-v2/entry.mjs"
                --driver $<TARGET_FILE:test-h3-v2-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_H3_V2_NAMED_FIXTURE}"
        )
        add_test(
            NAME worker_h3_v2_entry_wrapper
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/h3-v2/entry.mjs"
                --driver $<TARGET_FILE:test-h3-v2-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_H3_V2_WRAPPER_FIXTURE}"
        )
        add_test(
            NAME worker_h3_v2_entry_handler
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/h3-v2/entry.mjs"
                --driver $<TARGET_FILE:test-h3-v2-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_H3_V2_HANDLER_FIXTURE}"
                --expected-body h3-handler-ok
                --path /handler
        )
        add_test(
            NAME worker_h3_v2_lifecycle
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/h3-v2/lifecycle.mjs"
                --driver $<TARGET_FILE:test-h3-v2-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_H3_V2_DEFAULT_APP_FIXTURE}"
        )
        add_test(
            NAME worker_h3_v2_modes
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/h3-v2/modes.mjs"
                --driver $<TARGET_FILE:test-h3-v2-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --default-bundle "${CAPSID_H3_V2_DEFAULT_APP_FIXTURE}"
                --malformed-bundle "${CAPSID_H3_V2_MALFORMED_FIXTURE}"
                --debug-bundle "${CAPSID_H3_V2_DEBUG_FIXTURE}"
        )
        add_test(
            NAME worker_h3_v2_permissions
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/h3-v2/permissions.mjs"
                --driver $<TARGET_FILE:test-h3-v2-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_H3_V2_DEFAULT_APP_FIXTURE}"
        )
        set_tests_properties(
            worker_h3_v2_compatibility
            PROPERTIES
                TIMEOUT 180
                LABELS "framework;h3-v2;differential"
        )
        set_tests_properties(
            worker_h3_v2_entry_default_app
            worker_h3_v2_entry_default_object
            worker_h3_v2_entry_named
            worker_h3_v2_entry_wrapper
            worker_h3_v2_entry_handler
            PROPERTIES TIMEOUT 30 LABELS "framework;h3-v2;entry"
        )
        set_tests_properties(
            worker_h3_v2_lifecycle
            PROPERTIES TIMEOUT 60 LABELS "framework;h3-v2;lifecycle"
        )
        set_tests_properties(
            worker_h3_v2_modes
            PROPERTIES TIMEOUT 45 LABELS "framework;h3-v2;modes"
        )
        set_tests_properties(
            worker_h3_v2_permissions
            PROPERTIES TIMEOUT 45 LABELS "framework;h3-v2;permissions"
        )

        add_executable(test-p0-boundaries tests/test_p0_boundaries.cc)
        target_link_libraries(test-p0-boundaries PRIVATE capsid_runtime)
        add_dependencies(test-p0-boundaries test-p0-fixture)
        add_test(
            NAME worker_p0_boundaries
            COMMAND test-p0-boundaries
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_P0_FIXTURE}"
        )
        set_tests_properties(worker_p0_boundaries PROPERTIES TIMEOUT 20)

        add_executable(
            test-sandbox
            tests/test_sandbox.cc
            src/sandbox.cc
        )
        target_include_directories(test-sandbox PRIVATE include src)
        target_link_libraries(test-sandbox PRIVATE capsid_sanitizers)
        add_test(NAME worker_sandbox_enforcement COMMAND test-sandbox)
        set_tests_properties(worker_sandbox_enforcement PROPERTIES TIMEOUT 10)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_executable(
                test-sandbox-fd-hygiene
                tests/test_sandbox_fd_hygiene.cc
            )
            target_link_libraries(
                test-sandbox-fd-hygiene PRIVATE capsid_runtime
            )
            add_test(
                NAME worker_sandbox_fd_hygiene
                COMMAND test-sandbox-fd-hygiene
                    $<TARGET_FILE:capsid-worker>
            )
            set_tests_properties(
                worker_sandbox_fd_hygiene
                PROPERTIES
                    TIMEOUT 10
                    LABELS "sandbox"
            )

            add_test(
                NAME worker_sandbox_namespaces
                COMMAND test-sandbox --namespaces
            )
            set_tests_properties(
                worker_sandbox_namespaces
                PROPERTIES
                    TIMEOUT 10
                    LABELS "sandbox"
                    SKIP_RETURN_CODE 77
            )

            add_executable(
                test-sandbox-cgroup
                tests/test_sandbox_cgroup.cc
            )
            target_link_libraries(
                test-sandbox-cgroup PRIVATE capsid_runtime
            )
            add_dependencies(test-sandbox-cgroup test-p0-fixture)
            add_test(
                NAME worker_sandbox_cgroup_v2
                COMMAND test-sandbox-cgroup
                    $<TARGET_FILE:capsid-worker>
                    "${CAPSID_P0_FIXTURE}"
            )
            set_tests_properties(
                worker_sandbox_cgroup_v2
                PROPERTIES
                    TIMEOUT 15
                    LABELS "sandbox"
                    SKIP_RETURN_CODE 77
            )

            add_executable(
                test-sandbox-network-namespace
                tests/test_sandbox_network_namespace.cc
            )
            target_link_libraries(
                test-sandbox-network-namespace
                PRIVATE capsid_runtime
            )
            add_dependencies(
                test-sandbox-network-namespace
                test-p0-fixture
            )
            add_test(
                NAME worker_sandbox_network_namespace
                COMMAND test-sandbox-network-namespace
                    $<TARGET_FILE:capsid-worker>
                    "${CAPSID_P0_FIXTURE}"
            )
            set_tests_properties(
                worker_sandbox_network_namespace
                PROPERTIES
                    TIMEOUT 15
                    LABELS "sandbox"
                    SKIP_RETURN_CODE 77
            )
        endif()

        add_executable(
            test-worker-protocol-violation
            tests/test_worker_protocol_violation.cc
            src/protocol.cc
        )
        target_include_directories(
            test-worker-protocol-violation
            PRIVATE include src)
        target_link_libraries(
            test-worker-protocol-violation
            PRIVATE capsid_sanitizers)
        add_dependencies(test-worker-protocol-violation test-p0-fixture)
        add_test(
            NAME worker_rejects_request_credit_violation
            COMMAND test-worker-protocol-violation
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_P0_FIXTURE}"
        )
        set_tests_properties(
            worker_rejects_request_credit_violation
            PROPERTIES TIMEOUT 10
        )

        add_executable(test-fetch-cancel tests/test_fetch_cancel.cc)
        target_link_libraries(
            test-fetch-cancel
            PRIVATE capsid_runtime Threads::Threads
        )
        add_dependencies(test-fetch-cancel test-p0-fixture)
        add_test(
            NAME worker_fetch_cancel_lifecycle
            COMMAND test-fetch-cancel
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_P0_FIXTURE}"
        )
        set_tests_properties(worker_fetch_cancel_lifecycle PROPERTIES TIMEOUT 15)

        add_executable(
            test-direct-fetch-matrix
            tests/test_direct_fetch_matrix.cc
        )
        target_link_libraries(
            test-direct-fetch-matrix
            PRIVATE capsid_runtime Threads::Threads
        )
        add_dependencies(
            test-direct-fetch-matrix
            test-direct-fetch-matrix-fixture
        )
        add_test(
            NAME worker_direct_fetch_http_matrix
            COMMAND test-direct-fetch-matrix
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_DIRECT_FETCH_MATRIX_FIXTURE}"
        )
        set_tests_properties(
            worker_direct_fetch_http_matrix
            PROPERTIES TIMEOUT 40
        )
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(
                NAME worker_strict_sandbox_direct_fetch
                COMMAND test-direct-fetch-matrix
                    $<TARGET_FILE:capsid-worker>
                    "${CAPSID_DIRECT_FETCH_MATRIX_FIXTURE}"
                    --strict
            )
            set_tests_properties(
                worker_strict_sandbox_direct_fetch
                PROPERTIES TIMEOUT 40 LABELS "sandbox"
            )
        endif()

        set(CAPSID_MBEDTLS_TEST_DATA
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js/deps/mbedtls/framework/data_files")
        add_executable(
            test-direct-fetch-tls
            tests/test_direct_fetch_tls.cc
        )
        target_link_libraries(
            test-direct-fetch-tls
            PRIVATE
                capsid_runtime
                Threads::Threads
                mbedtls
                mbedx509
                mbedcrypto
        )
        add_dependencies(
            test-direct-fetch-tls
            test-direct-fetch-tls-fixture
        )
        add_test(
            NAME worker_direct_fetch_https_ca
            COMMAND test-direct-fetch-tls
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_DIRECT_FETCH_TLS_FIXTURE}"
                "${CAPSID_MBEDTLS_TEST_DATA}/server2-sha256.crt"
                "${CAPSID_MBEDTLS_TEST_DATA}/server2.key"
                "${CAPSID_MBEDTLS_TEST_DATA}/test-ca-sha256.crt"
        )
        set_tests_properties(
            worker_direct_fetch_https_ca
            PROPERTIES TIMEOUT 30
        )
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(
                NAME worker_strict_sandbox_https_ca
                COMMAND test-direct-fetch-tls
                    $<TARGET_FILE:capsid-worker>
                    "${CAPSID_DIRECT_FETCH_TLS_FIXTURE}"
                    "${CAPSID_MBEDTLS_TEST_DATA}/server2-sha256.crt"
                    "${CAPSID_MBEDTLS_TEST_DATA}/server2.key"
                    "${CAPSID_MBEDTLS_TEST_DATA}/test-ca-sha256.crt"
                    --strict
            )
            set_tests_properties(
                worker_strict_sandbox_https_ca
                PROPERTIES TIMEOUT 30 LABELS "sandbox"
            )
        endif()

        add_executable(test-stubborn-worker tests/stubborn_worker.cc)
        add_executable(test-worker-lifecycle tests/test_worker_lifecycle.cc)
        target_link_libraries(test-worker-lifecycle PRIVATE capsid_runtime)
        add_test(
            NAME worker_bounded_destroy
            COMMAND test-worker-lifecycle $<TARGET_FILE:test-stubborn-worker>
        )
        set_tests_properties(worker_bounded_destroy PROPERTIES TIMEOUT 5)
    endif()
endif()
