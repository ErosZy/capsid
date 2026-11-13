if(BUILD_TESTING)
    # The frozen FetchRPC vectors are also exercised in Host/worker-off
    # builds (including the standalone fuzz job). They need the actual
    # vendored JSON target, even when neither product target creates it.
    if(NOT TARGET capsid_jansson)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/vendor/jansson")
        target_link_libraries(capsid_jansson PRIVATE capsid_sanitizers)
    endif()

    add_executable(test-bytecode-publish tests/test_bytecode_publish.cc)
    target_include_directories(test-bytecode-publish PRIVATE tools)
    target_link_libraries(test-bytecode-publish PRIVATE capsid_sanitizers)
    set_target_properties(test-bytecode-publish PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(CAPSID_STRICT_WARNINGS)
        if(MSVC)
            target_compile_options(test-bytecode-publish PRIVATE /W4 /WX)
        else()
            target_compile_options(test-bytecode-publish PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()
    add_test(NAME bytecode_publish_operations COMMAND test-bytecode-publish)
    set_tests_properties(bytecode_publish_operations PROPERTIES
        LABELS "compiler;bytecode;filesystem" TIMEOUT 10)
    if(TARGET capsid-bytecode-compile)
        find_program(CAPSID_PYTHON3_EXECUTABLE NAMES python3 python REQUIRED)
        add_test(NAME bytecode_output_publication
            COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bytecode_output_publication.py"
                --compiler "$<TARGET_FILE:capsid-bytecode-compile>")
        set_tests_properties(bytecode_output_publication PROPERTIES
            LABELS "compiler;bytecode;filesystem" TIMEOUT 60)
        add_test(NAME txiki_upgrade_report_contract_controls
            COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_txiki_upgrade_report.py")
        set_tests_properties(txiki_upgrade_report_contract_controls PROPERTIES
            LABELS "build;provenance;negative-controls" TIMEOUT 30)
    endif()
    if(NOT CAPSID_PYTHON3_EXECUTABLE)
        find_program(CAPSID_PYTHON3_EXECUTABLE NAMES python3 python REQUIRED)
    endif()
    add_test(NAME asan_memory_acceptance_controls
        COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_asan_memory_acceptance.py")
    set_tests_properties(asan_memory_acceptance_controls PROPERTIES
        TIMEOUT 30 LABELS "build;sanitizer;negative-controls")
    # Portable measurement rules are tested on both native platforms. The
    # actual C-ABI/PSS probe and its real-process tests require Linux /proc.
    add_executable(test-benchmark-cold-measurement
        tests/test_benchmark_cold_measurement.cc)
    target_include_directories(test-benchmark-cold-measurement PRIVATE bench)
    target_link_libraries(test-benchmark-cold-measurement PRIVATE capsid_sanitizers)
    set_target_properties(test-benchmark-cold-measurement PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(CAPSID_STRICT_WARNINGS)
        if(MSVC)
            target_compile_options(test-benchmark-cold-measurement PRIVATE /W4 /WX)
        else()
            target_compile_options(test-benchmark-cold-measurement PRIVATE
                -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()
    add_test(NAME benchmark_cold_measurement COMMAND test-benchmark-cold-measurement)
    set_tests_properties(benchmark_cold_measurement PROPERTIES
        LABELS "benchmark;measurement" TIMEOUT 10)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CAPSID_BUILD_WORKER)
        add_executable(test-cold-worker-descendant tests/helpers/cold_worker_descendant.cc)
        set_target_properties(test-cold-worker-descendant PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            target_compile_options(test-cold-worker-descendant PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_executable(benchmark-cold-start bench/cold-start.cc)
        target_link_libraries(benchmark-cold-start PRIVATE capsid_runtime)
        set_target_properties(benchmark-cold-start PROPERTIES
            OUTPUT_NAME cold-start CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            target_compile_options(benchmark-cold-start PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()
    # Test sources include "win32_compat.h" and other src/-relative
    # internals; expose the directory scope once instead of duplicating
    # the include dir on every test target. (build_tests.cmake is
    # included from the top-level scope, so this applies to the test
    # targets created below only.)
    include_directories("${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(WIN32)
        # Many test targets compile runtime sources (egress_policy.cc ...)
        # directly instead of linking capsid_runtime; each of them needs
        # the Winsock import library for inet_pton/ntohs/htons.
        link_libraries(ws2_32)
    endif()
    if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
        add_test(NAME msvc_header_dependencies
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_PREFIX_MODULE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/msvc_dependency_prefix.cmake"
                "-DCAPSID_PREFIX_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/msvc-header-dependency-tests"
                "-DCAPSID_PREFIX_C_COMPILER=${CMAKE_C_COMPILER}"
                "-DCAPSID_PREFIX_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DCAPSID_PREFIX_NINJA=${CMAKE_MAKE_PROGRAM}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestMsvcDependencyPrefix.cmake")
        set_tests_properties(msvc_header_dependencies PROPERTIES TIMEOUT 60 LABELS "build;windows")
        find_program(CAPSID_PYTHON3_EXECUTABLE NAMES python3 python REQUIRED)
        add_test(NAME msvc_header_dependency_encodings
            COMMAND "${CAPSID_PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_msvc_header_dependency_encodings.py"
                --cmake "${CMAKE_COMMAND}"
                --prefix-module "${CMAKE_CURRENT_SOURCE_DIR}/cmake/msvc_dependency_prefix.cmake"
                --fixture-script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestMsvcDependencyPrefix.cmake"
                --test-root "${CMAKE_CURRENT_BINARY_DIR}/mhd"
                --c-compiler "${CMAKE_C_COMPILER}"
                --cxx-compiler "${CMAKE_CXX_COMPILER}"
                --ninja "${CMAKE_MAKE_PROGRAM}")
        set_tests_properties(msvc_header_dependency_encodings PROPERTIES
            TIMEOUT 150 LABELS "build;windows;contract")
    endif()
    if(TARGET capsid_host_core)
        add_executable(test-host-parent-stop-monitor tests/test_host_parent_stop_monitor.cc)
        target_include_directories(test-host-parent-stop-monitor PRIVATE src)
        target_link_libraries(test-host-parent-stop-monitor PRIVATE capsid_host_core capsid_sanitizers)
        set_target_properties(test-host-parent-stop-monitor PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(test-host-parent-stop-monitor PRIVATE /W4 /WX)
            else()
                target_compile_options(test-host-parent-stop-monitor PRIVATE -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(NAME host_parent_stop_monitor COMMAND test-host-parent-stop-monitor)
        set_tests_properties(host_parent_stop_monitor PROPERTIES TIMEOUT 10 LABELS "host;lifecycle;descriptor")
    endif()

    if(CAPSID_BUILD_HOST)
        find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

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

        if(UNIX)
            add_test(
                NAME release_install_script
                COMMAND bash
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install_script.sh"
                    "${CMAKE_CURRENT_SOURCE_DIR}/install.sh")
            set_tests_properties(release_install_script PROPERTIES
                LABELS "release;install"
                TIMEOUT 30)
        endif()

        # host_config_model.cc is part of the managed coordinator and not
        # built on Windows (docs/windows.md); the model test SKIPs by
        # absence there.
        if(NOT WIN32)
        add_executable(
            test-host-config-model
            tests/test_host_config_model.cc)
        target_include_directories(
            test-host-config-model PRIVATE include src)
        target_link_libraries(test-host-config-model PRIVATE
            capsid_host_core
            Boost::system
            capsid_sanitizers)
        set_target_properties(test-host-config-model PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-config-model PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-config-model PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(NAME host_config_model COMMAND test-host-config-model)
        endif()

        # Binding v1 (docs/binding-technical-design.md §7.1): one target
        if(Boost_FOUND)
            add_executable(test-host-client-disconnect tests/test_host_client_disconnect.cc)
            target_link_libraries(test-host-client-disconnect PRIVATE Boost::system capsid_sanitizers)
            set_target_properties(test-host-client-disconnect PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
            add_test(NAME host_client_disconnect_probe COMMAND test-host-client-disconnect)
            set_tests_properties(host_client_disconnect_probe PROPERTIES TIMEOUT 10 LABELS "host;http;lifecycle")
        endif()

        # Binding v1 (docs/binding-technical-design.md §7.1): one target
        # per contract layer so a schema edit never rebuilds the scanner.
        function(capsid_add_binding_test name source)
            add_executable(${name} ${source})
            target_include_directories(${name} PRIVATE include src)
            target_link_libraries(${name} PRIVATE
                capsid_host_core
                capsid_sanitizers)
            set_target_properties(${name} PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(${name} PRIVATE /W4 /WX)
                else()
                    target_compile_options(${name} PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
        endfunction()

        capsid_add_binding_test(
            test-host-binding-config
            tests/test_host_binding_config.cc)
        add_test(NAME host_binding_config COMMAND test-host-binding-config)

        capsid_add_binding_test(
            test-host-binding-manifest
            tests/test_host_binding_manifest.cc)
        add_test(NAME host_binding_manifest COMMAND test-host-binding-manifest)

        # The v1 scanner intentionally uses the POSIX fd-relative
        # openat/fstatat boundary. Windows fails --bindings-root closed until
        # its reparse-point/ACL equivalent exists, so the POSIX fixture is
        # not compiled there.
        if(NOT WIN32)
            capsid_add_binding_test(
                test-host-binding-registry
                tests/test_host_binding_registry.cc)
            add_test(NAME host_binding_registry COMMAND test-host-binding-registry)
        else()
            capsid_add_binding_test(
                test-host-binding-registry-win
                tests/test_host_binding_registry_win.cc)
            add_test(
                NAME host_binding_registry_win
                COMMAND test-host-binding-registry-win)
        endif()

        capsid_add_binding_test(
            test-host-binding-compile
            tests/test_host_binding_compile.cc)
        add_test(NAME host_binding_compile COMMAND test-host-binding-compile)

        add_executable(
            test-host-trusted-key-store
            tests/test_host_trusted_key_store.cc)
        target_include_directories(
            test-host-trusted-key-store PRIVATE include src)
        target_link_libraries(test-host-trusted-key-store PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-trusted-key-store PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-trusted-key-store PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-trusted-key-store PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(NAME host_trusted_key_store
            COMMAND test-host-trusted-key-store)

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
            # Explicit dependencies: a fresh build of this test target alone
            # must produce the compiler and worker binaries it executes
            # (never rely on binaries left in the tree by earlier builds).
            add_dependencies(
                test-runtime-bytecode-compiler-round-trip
                capsid-bytecode-compile
                capsid-worker)
        endif()

        # The safe-read fixture builder uses dirfd-relative POSIX
        # primitives (openat/mkdirat/symlinkat/mkfifoat) and unix-domain
        # socket nodes throughout; the Windows open_beneath path is
        # exercised via the runtime compile-time contract and documented
        # in docs/windows.md (SKIP by absence, never FAIL).
        if(NOT WIN32)
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
        endif()

        # Secret files belong to the managed coordinator (POSIX-only,
        # docs/windows.md); the driver's openat/mkfifoat fixtures skip by
        # absence on Windows.
        if(NOT WIN32)
        add_executable(
            test-host-secret-file-provider
            tests/test_host_secret_file_provider.cc)
        target_include_directories(
            test-host-secret-file-provider PRIVATE include src)
        target_link_libraries(test-host-secret-file-provider PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-secret-file-provider PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-secret-file-provider PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-secret-file-provider PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(
            NAME host_secret_file_provider
            COMMAND test-host-secret-file-provider)
        endif()

        if(UNIX)
            # M1D Unix Admin API frozen RED suite. This is deliberately
            # independent of the single-worker benchmark fixture: the
            # managed service will own the listener and coordinator adapter.
            add_executable(
                test-host-admin-api
                tests/test_host_admin_api.cc)
            target_include_directories(
                test-host-admin-api PRIVATE include src)
            target_link_libraries(test-host-admin-api PRIVATE
                capsid_host_core
                capsid_sanitizers)
            set_target_properties(test-host-admin-api PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(
                        test-host-admin-api PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        test-host-admin-api PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            foreach(CAPSID_ADMIN_API_TEST_ID
                    host_admin_peer_credentials
                    host_admin_socket_mode
                    host_admin_global_authorization
                    host_admin_four_endpoints
                    host_admin_request_limits
                    host_admin_strict_requests_and_redaction
                    host_admin_identifier_grammar
                    host_admin_bodyless_routes_and_safe_status
                    host_admin_backend_exception_redacted
                    host_admin_backend_output_validation
                    host_admin_submission_status
                    host_admin_socket_group
                    host_admin_stale_socket_recovery
                    host_admin_socket_path_fail_closed)
                add_test(
                    NAME "${CAPSID_ADMIN_API_TEST_ID}"
                    COMMAND test-host-admin-api
                        "${CAPSID_ADMIN_API_TEST_ID}")
                set_tests_properties(
                    "${CAPSID_ADMIN_API_TEST_ID}" PROPERTIES TIMEOUT 10)
            endforeach()
        endif()

        if(UNIX AND Boost_FOUND)
            # The second Admin batch crosses the real Unix-stream/HTTP
            # boundary. The production implementation must use the Host's
            # Boost.Beast HTTP/1 authority; the public header remains free of
            # Boost types.
            find_package(Threads REQUIRED)
            add_executable(
                test-host-admin-http
                tests/test_host_admin_http.cc)
            target_include_directories(
                test-host-admin-http PRIVATE include src)
            target_link_libraries(test-host-admin-http PRIVATE
                capsid_host_core
                Boost::system
                Threads::Threads
                capsid_sanitizers)
            set_target_properties(test-host-admin-http PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                target_compile_options(
                    test-host-admin-http PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
            foreach(CAPSID_ADMIN_HTTP_TEST_ID
                    host_admin_http_authorized_round_trip
                    host_admin_http_peer_rejected_before_read
                    host_admin_http_framing_limits
                    host_admin_http_smuggling_rejected
                    host_admin_http_slow_header_timeout
                    host_admin_http_slow_drip_deadlines
                    host_admin_http_accepted_fd_remains_caller_owned
                    host_admin_http_closed_peer_does_not_raise_sigpipe)
                add_test(
                    NAME "${CAPSID_ADMIN_HTTP_TEST_ID}"
                    COMMAND test-host-admin-http
                        "${CAPSID_ADMIN_HTTP_TEST_ID}")
                set_tests_properties(
                    "${CAPSID_ADMIN_HTTP_TEST_ID}" PROPERTIES TIMEOUT 10)
            endforeach()

            # Poll-timeout saturation boundary: every Host poll call site
            # clamps timeouts wider than int to INT_MAX so a large-but-valid
            # deadline can never wrap negative and block forever (unit test,
            # no sockets or sleeps).
            add_executable(
                test-host-poll-limits
                tests/test_host_poll_limits.cc)
            target_include_directories(
                test-host-poll-limits PRIVATE include src)
            target_link_libraries(test-host-poll-limits PRIVATE
                capsid_host_core
                Boost::system
                Threads::Threads
                capsid_sanitizers)
            set_target_properties(test-host-poll-limits PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                target_compile_options(
                    test-host-poll-limits PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
            add_test(
                NAME host_poll_limits_saturation
                COMMAND test-host-poll-limits)
            set_tests_properties(
                host_poll_limits_saturation PROPERTIES TIMEOUT 10)

            # Credit aggregation threshold clamp: a threshold at or above
            # the response window could never be reached by a long-lived
            # stream (pending credit is bounded by the window), so the
            # effective threshold must clamp to window/4 (unit test, no
            # sockets or sleeps).
            add_executable(
                test-host-credit-limits
                tests/test_host_credit_limits.cc)
            target_include_directories(
                test-host-credit-limits PRIVATE include src)
            target_link_libraries(test-host-credit-limits PRIVATE
                capsid_host_core
                Boost::system
                Threads::Threads
                capsid_sanitizers)
            set_target_properties(test-host-credit-limits PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                target_compile_options(
                    test-host-credit-limits PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
            add_test(
                NAME host_credit_limits_clamp
                COMMAND test-host-credit-limits)
            set_tests_properties(
                host_credit_limits_clamp PROPERTIES TIMEOUT 10)

            # The one-connection transport above is intentionally not a
            # daemon lifecycle. This suite freezes the owning long-lived
            # service: repeated accepts, bounded stop from idle/slow-client
            # states, and inode-safe pathname cleanup.
            add_executable(
                test-host-admin-service
                tests/test_host_admin_service.cc)
            target_include_directories(
                test-host-admin-service PRIVATE include src)
            target_link_libraries(test-host-admin-service PRIVATE
                capsid_host_core
                Boost::system
                Threads::Threads
                capsid_sanitizers)
            set_target_properties(test-host-admin-service PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                target_compile_options(
                    test-host-admin-service PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
            foreach(CAPSID_ADMIN_SERVICE_TEST_ID
                    host_admin_service_multiple_connections
                    host_admin_service_idle_stop
                    host_admin_service_slow_client_stop
                    host_admin_service_preserves_replaced_path
                    host_admin_service_stop_burst_is_nonblocking
                    host_admin_service_double_start_rejected
                    host_admin_service_stop_before_start_rejected
                    host_admin_service_start_stop_race)
                add_test(
                    NAME "${CAPSID_ADMIN_SERVICE_TEST_ID}"
                    COMMAND test-host-admin-service
                        "${CAPSID_ADMIN_SERVICE_TEST_ID}")
                set_tests_properties(
                    "${CAPSID_ADMIN_SERVICE_TEST_ID}" PROPERTIES TIMEOUT 10)
            endforeach()
        endif()

        if(CAPSID_BUILD_WORKER AND NOT WIN32)
            # M1D Admin/coordinator bridge: the real managed adapter and its
            # bounded asynchronous submission wrapper are separate from the
            # HTTP transport so operation progress remains independently
            # testable. (The managed coordinator is POSIX-only;
            # docs/windows.md.)
            find_package(Threads REQUIRED)
            add_executable(
                test-host-managed-admin-backend
                tests/test_host_managed_admin_backend.cc)
            target_include_directories(
                test-host-managed-admin-backend PRIVATE include src)
            target_link_libraries(test-host-managed-admin-backend PRIVATE
                capsid_runtime
                capsid_host_core
                capsid_jansson
                OpenSSL::Crypto
                Threads::Threads
                capsid_sanitizers)
            set_target_properties(test-host-managed-admin-backend PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                target_compile_options(
                    test-host-managed-admin-backend PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
            foreach(CAPSID_MANAGED_ADMIN_TEST_ID
                    host_admin_async_deploy_progress
                    host_admin_async_failure_and_capacity
                    host_managed_admin_routes_real_coordinator
                    host_admin_managed_status_dispatch_round_trip
                    host_admin_async_rejects_submission_after_stop)
                add_test(
                    NAME "${CAPSID_MANAGED_ADMIN_TEST_ID}"
                    COMMAND test-host-managed-admin-backend
                        "${CAPSID_MANAGED_ADMIN_TEST_ID}")
                set_tests_properties(
                    "${CAPSID_MANAGED_ADMIN_TEST_ID}" PROPERTIES TIMEOUT 10)
            endforeach()

            # The managed executable suite (deploys, crash replacement,
            # quarantine, health probe) exercises the Linux worker spawn
            # and /proc pid scans; it is not registered on macOS — the
            # POSIX host matrix covers the pure host units there.
            if(UNIX AND NOT APPLE AND Boost_FOUND AND TARGET capsid-host)
                # The production process closure: unlike the M1A benchmark
                # fixture, managed mode consumes host.json, owns the Admin
                # service and warmed worker, and shuts both down on SIGTERM.
                add_executable(
                    test-host-managed-executable
                    tests/test_host_managed_executable.cc)
                target_link_libraries(test-host-managed-executable PRIVATE
                    capsid_jansson
                    capsid_sanitizers)
                set_target_properties(test-host-managed-executable PROPERTIES
                    CXX_STANDARD 20
                    CXX_STANDARD_REQUIRED ON
                    CXX_EXTENSIONS OFF)
                if(CAPSID_STRICT_WARNINGS)
                    target_compile_options(
                        test-host-managed-executable PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
                add_dependencies(test-host-managed-executable
                    capsid-host
                    capsid-worker)
                foreach(CAPSID_MANAGED_EXECUTABLE_TEST_ID
                        host_managed_executable_deploy_and_shutdown
                        host_managed_executable_stops_during_deploy
                        host_managed_executable_recovers_on_restart
                        host_managed_executable_rejects_host_config_fifo
                        host_managed_executable_rejects_host_config_symlink
                        host_managed_executable_rejects_embedded_nul_path
                        host_managed_executable_rejects_ambiguous_secret_template
                        host_managed_executable_rejects_unsafe_admin_mode
                        host_managed_executable_rejects_negative_workers_total
                        host_managed_executable_rejects_oversized_workers_total
                        host_managed_executable_rejects_negative_max_inflight
                        host_managed_executable_rejects_zero_or_negative_pool_bounds
                        host_managed_executable_active_state_directory
                        host_managed_executable_active_state_oversized
                        host_managed_executable_active_state_foreign_owner
                        host_managed_executable_enforces_global_worker_capacity
                        host_managed_executable_enforces_queue_maximums
                        host_managed_executable_binding_deploy
                        host_managed_executable_enforces_streaming_maximums
                        host_managed_executable_enforces_write_timeout_maximum
                        host_managed_executable_redeploys_with_capacity_one
                        host_managed_executable_recovery_consumes_capacity
                        host_managed_executable_secret_canary_no_leak
                        host_managed_executable_structured_logs_json
                        host_managed_executable_metrics_endpoint
                        host_managed_executable_crash_mid_deploy_keeps_old
                        host_managed_executable_crash_staging_remnants
                        host_managed_executable_crash_orphan_generation
                        host_managed_executable_crash_quarantined_not_resurrected
                        host_managed_executable_crash_replaced
                        host_managed_executable_deploy_overlaps_quarantine
                        host_managed_executable_crash_loop_quarantines
                        host_managed_executable_quarantine_releases_capacity
                        host_managed_executable_quarantine_cleared_by_deploy
                        host_managed_executable_boot_recovery_bounded
                        host_managed_executable_crash_loop_does_not_starve_other_app
                        host_managed_executable_active_health_recycles_unhealthy
                        host_managed_executable_active_health_healthy_stays
                        host_managed_http_e2e_multi_app
                        host_managed_http_restart_recovers_route
                        host_managed_http_rejects_multiple_listeners)
                    add_test(
                        NAME "${CAPSID_MANAGED_EXECUTABLE_TEST_ID}"
                        COMMAND test-host-managed-executable
                            "${CAPSID_MANAGED_EXECUTABLE_TEST_ID}"
                            $<TARGET_FILE:capsid-host>
                            $<TARGET_FILE:capsid-worker>)
                    set_tests_properties(
                        "${CAPSID_MANAGED_EXECUTABLE_TEST_ID}"
                        PROPERTIES TIMEOUT 40)
                endforeach()
                set_tests_properties(
                    host_managed_executable_active_state_foreign_owner
                    # A normal user cannot chown the persisted fixture. The
                    # mandatory root sandbox matrix must rerun this case;
                    # ordinary prerequisite skips alone never prove it passed.
                    PROPERTIES SKIP_RETURN_CODE 77 LABELS "host;managed;sandbox")
                # The item-5a crash matrix drives repeated real worker
                # replacements (SIGKILL + backoff + spawn/READY) and, in
                # the boot test, a full restart; give the cycles headroom
                # under ASan-instrumented workers. The item-6 health
                # probe recycles an unhealthy worker through the same
                # replacement chain (2 probes per recycle × 3 recycles +
                # backoff + spawn before quarantine).
                set_tests_properties(
                    host_managed_executable_crash_replaced
                    host_managed_executable_crash_loop_quarantines
                    host_managed_executable_quarantine_cleared_by_deploy
                    host_managed_executable_boot_recovery_bounded
                    host_managed_executable_crash_loop_does_not_starve_other_app
                    host_managed_executable_active_health_recycles_unhealthy
                    host_managed_executable_active_health_healthy_stays
                    PROPERTIES TIMEOUT 60)
            endif()

            # M1D managed host frozen suite: one binary, one mode per test.
            # Every mode spawns a real worker, and worker spawn requires the
            # Linux-only strict sandbox (src/sandbox.cc); on other platforms
            # the suite cannot run, so it is registered on Linux only
            # (spec §9.6-10: macOS runs portable Host units only, and
            # §9.6-14: unsupported platforms SKIP, never FAIL).
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_executable(
                test-host-managed
                tests/test_host_managed.cc)
            target_include_directories(
                test-host-managed PRIVATE include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(test-host-managed PRIVATE
                capsid_runtime
                capsid_host_core
                capsid_jansson
                OpenSSL::Crypto
                capsid_sanitizers)
            if(Boost_FOUND)
                # ActivationPlan owns the legacy StaticPoolServer facade.
                # Its Debug destructor pulls the single-worker transport
                # from the static host core even for generation-only tests.
                target_sources(test-host-managed PRIVATE src/host/single_worker_server.cc)
                target_link_libraries(test-host-managed PRIVATE Boost::system)
                if(CAPSID_STRICT_WARNINGS AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                    # Same Boost.Beast false-positive allowance as capsid-host.
                    target_compile_options(test-host-managed PRIVATE -Wno-maybe-uninitialized)
                endif()
            endif()
            set_target_properties(test-host-managed PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(
                        test-host-managed PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        test-host-managed PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            add_dependencies(test-host-managed
                capsid-bytecode-compile
                capsid-worker)
            foreach(CAPSID_MANAGED_TEST_ID
                    host_managed_deploy_integration
                    host_managed_queue_config_deploy
                    host_managed_sse_permit_config_deploy
                    host_managed_write_timeout_config_deploy
                    host_managed_fixed_pool_deploy_and_recover
                    host_managed_generation_factory_replacement
                    host_managed_admin_generation_replacement_exit_status
                    host_managed_admin_fixed_generation_replacement_exit_status
                    host_managed_request_cancel_exit_status
                    host_managed_fixed_request_cancel_exit_status
                    host_managed_health_recycle_quarantine_exit_status
                    host_managed_quarantine_stale_generation
                    host_managed_quarantine_same_generation_reactivation
                    host_managed_quarantine_aborted_generation
                    host_managed_quarantine_prepared_generation_commits
                    host_managed_quarantine_retire_stale
                    host_managed_quarantine_persist_failure
                    host_managed_quarantine_pending_shutdown
                    host_managed_quarantine_expired_notifications
                    host_managed_quarantine_stale_handles
                    host_managed_prepare_failure_capacity
                    host_managed_misconfigured_activation_capacity
                    host_managed_fixed_pool_warm_failure_is_atomic
                    host_managed_admin_fixed_pool_handoff
                    host_managed_admin_worker_lifecycle
                    host_managed_admin_legacy_unclaimed_pool
                    host_managed_trusted_bytecode
                    host_managed_compatibility_fallback
                    host_managed_fallback_identity_retains_attestation
                    host_managed_fallback_records_reason
                    host_managed_bytecode_key_rotation_deploys
                    host_managed_revoked_key_deploy_rejected
                    host_managed_revoked_key_recovery_fail_closed
                    host_managed_restart_identity_stable
                    host_managed_secret_snapshot
                    host_managed_secret_snapshot_limit_precedes_staging
                    host_managed_secret_app_symlink_rejected
                    host_managed_secret_value_not_persisted
                    host_managed_secret_rotation_generates_new_pool
                    host_managed_recovery_accepts_large_valid_env_metadata
                    host_managed_deploy_fail_closed
                    host_managed_deploy_persist_failure_aborts
                    host_managed_retire_and_recovery
                    host_managed_retire_is_idempotent
                    host_managed_recovery_warms_worker
                    host_managed_version_immutability
                    host_managed_version_mapping_canonical_path
                    host_managed_resource_config_affects_identity
                    host_managed_applies_worker_memory_limit
                    host_managed_rejects_request_limit_over_host
                    host_managed_host_policy_affects_identity
                    host_managed_fetch_policy_warms_worker
                    host_managed_multiple_rules_stable
                    host_managed_multiple_env_entries_stable
                    host_managed_rejects_unknown_config
                    host_managed_literal_env_without_secret_root
                    host_managed_runtime_identity_fail_closed
                    host_managed_state_root_symlink_rejected
                    host_managed_staging_symlink_rejected
                    host_managed_generations_symlink_rejected
                    host_managed_complete_symlink_rejected
                    host_managed_concurrent_operation_ids
                    host_managed_recovery_uses_committed_generation
                    host_managed_recovery_cleans_stale_temp
                    host_managed_recovery_rejects_generation_tamper
                    host_managed_recovery_rejects_snapshot_fifo_promptly
                    host_managed_recovery_rejects_optional_snapshot_fifo
                    host_managed_recovery_rejects_oversized_snapshot
                    host_managed_recovery_revalidates_host_policy
                    host_managed_recovery_never_reuses_stale_secret
                    host_managed_recovery_rejects_trusted_bundle_swap
                    host_managed_recovery_rejects_trusted_attestation_drift
                    host_managed_recovery_rejects_source_name_tamper
                    host_managed_recovery_rejects_source_name_separator_tamper
                    host_managed_recovery_rejects_source_name_segment_tamper
                    host_managed_idempotent_rejects_corrupt_generation
                    host_managed_shared_generation_cleans_staging
                    host_managed_shared_generation_redeploys
                    host_managed_shared_generation_recovers
                    host_managed_storage_namespace_reaches_worker
                    host_managed_stdio_stream_reaches_worker
                    host_managed_rejects_storage_namespace_over_host
                    host_managed_rejects_stdio_stream_over_host
                    host_managed_resource_fields_affect_identity
                    host_managed_resource_limits_reach_worker
                    host_managed_process_address_space_reaches_release_runtime
                    host_managed_process_address_space_skipped_under_tsan
                    host_managed_failed_deploy_cleans_staging
                    host_managed_staging_mode_rejected
                    host_managed_generations_mode_rejected
                    host_managed_rejects_corrupt_version_mapping
                    host_managed_failed_operation_status)
                add_test(
                    NAME "${CAPSID_MANAGED_TEST_ID}"
                    COMMAND test-host-managed
                        "${CAPSID_MANAGED_TEST_ID}"
                        $<TARGET_FILE:capsid-worker>
                        $<TARGET_FILE:capsid-bytecode-compile>
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ipc-sync-response.js")
                set_tests_properties(
                    "${CAPSID_MANAGED_TEST_ID}" PROPERTIES TIMEOUT 120)
            endforeach()
            if(CAPSID_ENABLE_ASAN OR CAPSID_ENABLE_TSAN)
                set_tests_properties(host_managed_recovery_rejects_oversized_snapshot
                    PROPERTIES SKIP_RETURN_CODE 77)
            endif()
            endif()  # CMAKE_SYSTEM_NAME STREQUAL "Linux" — worker-spawn suite
        endif()

        add_executable(
            test-host-structured-log
            tests/test_host_structured_log.cc)
        target_include_directories(
            test-host-structured-log PRIVATE include src)
        target_link_libraries(test-host-structured-log PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-structured-log PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-structured-log PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-structured-log PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        foreach(CAPSID_STRUCTURED_LOG_TEST_ID
                structured_log_emits_single_line_json
                structured_log_app_lane_drops_and_counts
                structured_log_control_lane_never_drops
                structured_log_control_precedes_app_backlog
                structured_log_app_lane_is_fifo
                structured_log_flush_idle_writer
                structured_log_flush_after_final_event
                structured_log_concurrent_flush_joins_writer)
            add_test(
                NAME "${CAPSID_STRUCTURED_LOG_TEST_ID}"
                COMMAND test-host-structured-log
                    "${CAPSID_STRUCTURED_LOG_TEST_ID}")
            set_tests_properties(
                "${CAPSID_STRUCTURED_LOG_TEST_ID}" PROPERTIES TIMEOUT 30)
        endforeach()

        add_executable(
            test-host-policy-compiler
            tests/test_host_policy_compiler.cc)
        target_include_directories(
            test-host-policy-compiler PRIVATE include src)
        target_link_libraries(test-host-policy-compiler PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-policy-compiler PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-policy-compiler PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-policy-compiler PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(
            NAME host_policy_compiler
            COMMAND test-host-policy-compiler)

        add_executable(
            test-host-static-pool
            tests/test_host_static_pool.cc)
        target_include_directories(
            test-host-static-pool PRIVATE include src)
        target_link_libraries(test-host-static-pool PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-static-pool PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-static-pool PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-static-pool PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(
            NAME host_static_pool_activates_only_when_all_ready
            COMMAND test-host-static-pool all-ready)
        add_test(
            NAME host_static_pool_preserves_owner_shard
            COMMAND test-host-static-pool owner-shard)

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
            test-host-worker-capacity-ledger
            tests/test_worker_capacity_ledger.cc)
        target_include_directories(
            test-host-worker-capacity-ledger PRIVATE src)
        target_link_libraries(test-host-worker-capacity-ledger PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-worker-capacity-ledger PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-worker-capacity-ledger PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-worker-capacity-ledger PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        foreach(CAPSID_LEDGER_TEST_ID
                ledger_fresh_budget
                ledger_replace_surge_gate
                ledger_retire_drain_release
                ledger_no_surge_refuses
                ledger_retired_headroom
                ledger_overflow_limits)
            add_test(
                NAME "host_worker_capacity_ledger_${CAPSID_LEDGER_TEST_ID}"
                COMMAND test-host-worker-capacity-ledger
                    "${CAPSID_LEDGER_TEST_ID}")
            set_tests_properties(
                "host_worker_capacity_ledger_${CAPSID_LEDGER_TEST_ID}"
                PROPERTIES TIMEOUT 10)
        endforeach()

        add_executable(test-host-worker-capacity-lease tests/test_worker_capacity_lease.cc)
        target_include_directories(test-host-worker-capacity-lease PRIVATE src)
        target_link_libraries(test-host-worker-capacity-lease PRIVATE capsid_host_core capsid_sanitizers)
        set_target_properties(test-host-worker-capacity-lease PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(test-host-worker-capacity-lease PRIVATE /W4 /WX)
            else()
                target_compile_options(test-host-worker-capacity-lease PRIVATE -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        foreach(CAPSID_LEASE_CASE IN ITEMS lifecycle reaped-before-activation aborted-warm commit-reap-race)
            string(REPLACE "-" "_" CAPSID_LEASE_ID "${CAPSID_LEASE_CASE}")
            add_test(NAME "host_worker_capacity_lease_${CAPSID_LEASE_ID}"
                COMMAND test-host-worker-capacity-lease "${CAPSID_LEASE_CASE}")
            set_tests_properties("host_worker_capacity_lease_${CAPSID_LEASE_ID}" PROPERTIES TIMEOUT 20)
        endforeach()

        # The operation registry lives in the managed coordinator, which
        # is not built on Windows (docs/windows.md); SKIP by absence.
        if(NOT WIN32)
        add_executable(
            test-host-managed-registry
            tests/test_managed_registry.cc)
        target_include_directories(
            test-host-managed-registry PRIVATE src)
        target_link_libraries(test-host-managed-registry PRIVATE
            capsid_host_core
            capsid_sanitizers)
        set_target_properties(test-host-managed-registry PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-managed-registry PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-managed-registry PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        foreach(CAPSID_REGISTRY_TEST_ID
                registry_bounded
                registry_status_roundtrip
                slots_bounded_reclaimed
                slots_serialize_same_app
                slots_pinned_survive)
            add_test(
                NAME "host_managed_registry_${CAPSID_REGISTRY_TEST_ID}"
                COMMAND test-host-managed-registry
                    "${CAPSID_REGISTRY_TEST_ID}")
            set_tests_properties(
                "host_managed_registry_${CAPSID_REGISTRY_TEST_ID}"
                PROPERTIES TIMEOUT 30)
        endforeach()
        endif()

        # WP-09 §13.6 soak platform: the memory-wave client is a real
        # binary so the CI gates build it and the smoke test proves the
        # cancel/reclaim convergence protocol against the real worker.
        add_executable(
            soak-memory-waves
            soak/soak_memory_waves.cc)
        target_include_directories(soak-memory-waves PRIVATE src)
        target_link_libraries(soak-memory-waves PRIVATE capsid_runtime)
        set_target_properties(soak-memory-waves PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(soak-memory-waves PRIVATE /W4 /WX)
            else()
                target_compile_options(soak-memory-waves PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        if(TARGET capsid-worker)
            add_test(
                NAME soak_memory_waves_smoke
                COMMAND soak-memory-waves
                    $<TARGET_FILE:capsid-worker>
                    "${CMAKE_CURRENT_SOURCE_DIR}/soak/fixtures/soak-app.js"
                    4
                    50)
            set_tests_properties(soak_memory_waves_smoke PROPERTIES TIMEOUT 120)
        endif()

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

        if(TARGET capsid-host AND TARGET capsid-worker)
            add_executable(
                test-host-single-worker-lifecycle
                tests/test_host_single_worker_lifecycle.cc
                src/host/single_worker_server.cc)
            target_include_directories(
                test-host-single-worker-lifecycle PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(
                test-host-single-worker-lifecycle PRIVATE
                capsid_runtime
                capsid_host_core
                Boost::system
                capsid_sanitizers)
            set_target_properties(
                test-host-single-worker-lifecycle PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(
                        test-host-single-worker-lifecycle PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        test-host-single-worker-lifecycle PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                        target_compile_options(
                            test-host-single-worker-lifecycle PRIVATE
                            -Wno-maybe-uninitialized)
                    endif()
                endif()
            endif()
            add_test(
                NAME host_single_worker_controllable_lifecycle
                COMMAND test-host-single-worker-lifecycle
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_single_worker_controllable_lifecycle PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)

            # v0.1.3 local capsid.json permissions (--capsid-json): env
            # grants reach the worker, absent defaults stay deny-all, and
            # worker/request/healthCheck sections, env valueFrom and an
            # explicitly requested missing file all fail startup closed.
            add_executable(
                test-host-local-capsid-policy
                tests/test_host_local_capsid_policy.cc
                src/host/single_worker_server.cc)
            target_include_directories(
                test-host-local-capsid-policy PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(
                test-host-local-capsid-policy PRIVATE
                capsid_runtime
                capsid_host_core
                Boost::system
                capsid_sanitizers)
            set_target_properties(
                test-host-local-capsid-policy PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(
                        test-host-local-capsid-policy PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        test-host-local-capsid-policy PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                        target_compile_options(
                            test-host-local-capsid-policy PRIVATE
                            -Wno-maybe-uninitialized)
                    endif()
                endif()
            endif()
            add_test(
                NAME host_local_capsid_policy
                COMMAND test-host-local-capsid-policy
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_local_capsid_policy PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)

            capsid_add_binding_test(
                test-worker-binding-ready tests/test_worker_binding_ready.cc)
            target_link_libraries(test-worker-binding-ready PRIVATE capsid_runtime)
            add_test(NAME worker_binding_ready_zero
                COMMAND test-worker-binding-ready $<TARGET_FILE:capsid-worker> zero)
            set_tests_properties(worker_binding_ready_zero PROPERTIES
                TIMEOUT 40 LABELS "host;worker;binding;integration")
            # Windows Job limits bound committed memory and support ASan.
            # POSIX RLIMIT_AS cannot accommodate sanitizer shadow mappings;
            # Darwin rejects nonzero process-memory limits in the public API.
            if(WIN32 OR (NOT APPLE AND NOT CAPSID_ENABLE_ASAN AND NOT CAPSID_ENABLE_TSAN))
                add_test(NAME worker_binding_ready_finite
                    COMMAND test-worker-binding-ready $<TARGET_FILE:capsid-worker> finite)
                set_tests_properties(worker_binding_ready_finite PROPERTIES
                    TIMEOUT 40 LABELS "host;worker;binding;integration")
            endif()

            # Batch B begins RED before the production source exists. The
            # CONFIGURE_DEPENDS glob adds it to this isolated test target as
            # soon as the implementation is created, without requiring the
            # implementation agent to edit the frozen test registration.
            file(GLOB CAPSID_STATIC_POOL_SERVER_TEST_SOURCE
                CONFIGURE_DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/src/host/static_pool_server.cc")
            add_executable(
                test-host-static-pool-server
                tests/test_host_static_pool_server.cc
                src/host/single_worker_server.cc
                ${CAPSID_STATIC_POOL_SERVER_TEST_SOURCE})
            target_include_directories(
                test-host-static-pool-server PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(
                test-host-static-pool-server PRIVATE
                capsid_runtime
                capsid_host_core
                Boost::system
                capsid_sanitizers)
            set_target_properties(
                test-host-static-pool-server PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(
                        test-host-static-pool-server PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        test-host-static-pool-server PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                        target_compile_options(
                            test-host-static-pool-server PRIVATE
                            -Wno-maybe-uninitialized)
                    endif()
                endif()
            endif()
            # Multi-shard scenarios run on every platform through the pool's
            # shared acceptor (see docs/platform-support.md).
            add_test(
                NAME host_static_pool_server_shared_port_lifecycle
                COMMAND test-host-static-pool-server lifecycle
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_static_pool_server_shared_port_lifecycle PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)
            foreach(CAPSID_WS_POOL_WORKERS IN ITEMS 1 4)
                add_test(
                    NAME host_static_pool_websocket_io_${CAPSID_WS_POOL_WORKERS}
                    COMMAND test-host-static-pool-server
                        websocket-io-${CAPSID_WS_POOL_WORKERS}
                        $<TARGET_FILE:capsid-worker>)
                set_tests_properties(
                    host_static_pool_websocket_io_${CAPSID_WS_POOL_WORKERS} PROPERTIES
                    LABELS "host;integration;websocket;platform"
                    TIMEOUT 45)
            endforeach()
            add_test(
                NAME host_static_pool_server_binding_local_policy
                COMMAND test-host-static-pool-server binding-local-policy
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_static_pool_server_binding_local_policy PROPERTIES
                LABELS "host;integration;binding"
                TIMEOUT 30)
            add_test(
                NAME host_static_pool_server_drain_inflight_completes
                COMMAND test-host-static-pool-server drain-inflight-completes
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_static_pool_server_drain_deadline_forces
                COMMAND test-host-static-pool-server drain-deadline-forces
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_static_pool_server_drain_idle_exits
                COMMAND test-host-static-pool-server drain-idle-exits
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_static_pool_server_drain_inflight_completes
                host_static_pool_server_drain_deadline_forces
                host_static_pool_server_drain_idle_exits PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)
            add_test(
                NAME host_static_pool_server_atomic_start_failure
                COMMAND test-host-static-pool-server atomic-failure
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_static_pool_server_rejects_oversized_pool
                COMMAND test-host-static-pool-server oversized-pool
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_static_pool_server_stop_before_start
                COMMAND test-host-static-pool-server stop-before-start
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_static_pool_server_start_stop_race
                COMMAND test-host-static-pool-server start-stop-race
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_static_pool_server_atomic_start_failure
                host_static_pool_server_rejects_oversized_pool
                host_static_pool_server_stop_before_start
                host_static_pool_server_start_stop_race PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)

            # M2 E-1 admission control (§10.3): five-level gate chain in its
            # v1 fixed-pool form, frozen RED/GREEN in test_host_admission.cc.
            add_executable(
                test-host-admission
                tests/test_host_admission.cc
                src/host/single_worker_server.cc
                ${CAPSID_STATIC_POOL_SERVER_TEST_SOURCE})
            target_include_directories(
                test-host-admission PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(
                test-host-admission PRIVATE
                capsid_runtime
                capsid_host_core
                Boost::system
                capsid_sanitizers)
            set_target_properties(
                test-host-admission PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(
                        test-host-admission PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        test-host-admission PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                        target_compile_options(
                            test-host-admission PRIVATE
                            -Wno-maybe-uninitialized)
                    endif()
                endif()
            endif()
            add_test(
                NAME host_admission_inflight_full_rejects
                COMMAND test-host-admission inflight-full-rejects
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_admission_queue_full_rejects
                COMMAND test-host-admission queue-full-rejects
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_admission_queue_timeout_returns_504
                COMMAND test-host-admission queue-timeout-504
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_admission_keepalive_streamed
                COMMAND test-host-admission keepalive-streamed
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_admission_keepalive_streamed_drain
                COMMAND test-host-admission keepalive-streamed-drain
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(host_admission_keepalive_streamed
                host_admission_keepalive_streamed_drain PROPERTIES TIMEOUT 20)
            foreach(mode IN ITEMS idle-drain keepalive-streamed-drain-deadline
                                  keepalive-streamed-drain-no-signals)
                string(REPLACE "-" "_" case_suffix "${mode}")
                add_test(NAME host_admission_${case_suffix}
                    COMMAND test-host-admission ${mode} $<TARGET_FILE:capsid-worker>)
                set_tests_properties(host_admission_${case_suffix} PROPERTIES TIMEOUT 8)
            endforeach()
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(
                NAME host_admission_worker_death_returns_503
                COMMAND test-host-admission worker-death-503
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_admission_worker_death_returns_503 PROPERTIES
                TIMEOUT 30 LABELS "host;integration;m2")
            endif()
            # pool-forwards runs a multi-shard pool: SO_REUSEPORT on
            # Linux/macOS, pool-owned shared acceptor on Windows.
            add_test(
                NAME host_admission_pool_forwards_options
                COMMAND test-host-admission pool-forwards-admission
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_admission_pool_forwards_options PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)
            set_tests_properties(
                host_admission_inflight_full_rejects
                host_admission_queue_full_rejects
                host_admission_queue_timeout_returns_504 PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)
            set_tests_properties(
                host_admission_keepalive_streamed PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)

            # M2 E-2 SSE streaming permit (§9.3): Content-Type-only permit,
            # synthesized 503 before the head, exactly-once release on every
            # completion/cancel path, stream idle timeout, 1/1 boundary.
            add_executable(
                test-host-sse-permit
                tests/test_host_sse_permit.cc
                src/host/single_worker_server.cc
                ${CAPSID_STATIC_POOL_SERVER_TEST_SOURCE})
            target_include_directories(
                test-host-sse-permit PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(
                test-host-sse-permit PRIVATE
                capsid_runtime
                capsid_host_core
                Boost::system
                capsid_sanitizers)
            set_target_properties(
                test-host-sse-permit PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(
                        test-host-sse-permit PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        test-host-sse-permit PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                        target_compile_options(
                            test-host-sse-permit PRIVATE
                            -Wno-maybe-uninitialized)
                    endif()
                endif()
            endif()
            add_test(
                NAME host_sse_permit_full_rejects_503
                COMMAND test-host-sse-permit permit-full-rejects-503
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_sse_permit_released_on_completion
                COMMAND test-host-sse-permit permit-released-on-completion
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_sse_idle_timeout_cancels_and_releases
                COMMAND test-host-sse-permit idle-timeout-cancels-and-releases
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_sse_plain_chunked_no_permit
                COMMAND test-host-sse-permit plain-chunked-does-not-hold-permit
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_sse_max_inflight_one_boundary
                COMMAND test-host-sse-permit max-inflight-one-boundary
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_sse_uppercase_content_type_holds_permit
                COMMAND test-host-sse-permit
                    uppercase-content-type-holds-permit
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_sse_parameterized_content_type_holds_permit
                COMMAND test-host-sse-permit
                    parameterized-content-type-holds-permit
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_sse_permit_full_rejects_503
                host_sse_permit_released_on_completion
                host_sse_idle_timeout_cancels_and_releases
                host_sse_plain_chunked_no_permit
                host_sse_max_inflight_one_boundary
                host_sse_uppercase_content_type_holds_permit
                host_sse_parameterized_content_type_holds_permit PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)

            # M2 E-3 slow-client write deadline (§9.2): a socket write that
            # does not complete within write_timeout_ms cancels the request
            # and closes the connection; independent from the worker-side
            # request timeout (§8.3); fast responses untouched.
            add_executable(
                test-host-write-timeout
                tests/test_host_write_timeout.cc
                src/host/single_worker_server.cc
                ${CAPSID_STATIC_POOL_SERVER_TEST_SOURCE})
            target_include_directories(
                test-host-write-timeout PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(
                test-host-write-timeout PRIVATE
                capsid_runtime
                capsid_host_core
                Boost::system
                capsid_sanitizers)
            set_target_properties(
                test-host-write-timeout PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(
                        test-host-write-timeout PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        test-host-write-timeout PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                        target_compile_options(
                            test-host-write-timeout PRIVATE
                            -Wno-maybe-uninitialized)
                    endif()
                endif()
            endif()
            add_test(
                NAME host_write_timeout_cancels
                COMMAND test-host-write-timeout write-timeout-cancels
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_worker_deadline_independent
                COMMAND test-host-write-timeout worker-deadline-independent
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_worker_deadline_cooperative
                COMMAND test-host-write-timeout worker-deadline-cooperative
                    $<TARGET_FILE:capsid-worker>)
            add_test(
                NAME host_fast_response_untouched
                COMMAND test-host-write-timeout fast-response-untouched
                    $<TARGET_FILE:capsid-worker>)
            add_test(NAME host_write_timeout_disabled
                COMMAND test-host-write-timeout write-timeout-disabled $<TARGET_FILE:capsid-worker>)
            add_test(NAME host_pool_write_timeout_cancels
                COMMAND test-host-write-timeout pool-write-timeout-cancels $<TARGET_FILE:capsid-worker>)
            add_test(NAME host_pool_drain_write_deadline
                COMMAND test-host-write-timeout pool-drain-write-deadline $<TARGET_FILE:capsid-worker>)
            add_test(NAME host_pool_stop_pending_write
                COMMAND test-host-write-timeout pool-stop-pending-write $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_write_timeout_cancels
                host_worker_deadline_independent
                host_worker_deadline_cooperative
                host_fast_response_untouched
                host_write_timeout_disabled
                host_pool_write_timeout_cancels
                host_pool_drain_write_deadline
                host_pool_stop_pending_write PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)

            add_executable(
                test-host-static-pool-integration
                tests/test_host_static_pool_integration.cc
                src/host/single_worker_server.cc
                ${CAPSID_STATIC_POOL_SERVER_TEST_SOURCE})
            target_include_directories(
                test-host-static-pool-integration PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(
                test-host-static-pool-integration PRIVATE
                capsid_runtime
                capsid_host_core
                Boost::system
                capsid_sanitizers)
            set_target_properties(
                test-host-static-pool-integration PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(
                        test-host-static-pool-integration PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        test-host-static-pool-integration PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                        target_compile_options(
                            test-host-static-pool-integration PRIVATE
                            -Wno-maybe-uninitialized)
                    endif()
                endif()
            endif()
            # The activation-barrier case wraps the worker in a POSIX shell
            # script (mkdir/sleep/exec semantics); it SKIPs by absence on
            # Windows. The worker-exit case needs /proc evidence and is
            # Linux-only as well.
            if(NOT WIN32)
                add_test(
                    NAME host_static_pool_activation_barrier
                    COMMAND test-host-static-pool-integration activation-barrier
                        $<TARGET_FILE:capsid-worker>)
                add_test(
                    NAME host_static_pool_worker_exit_isolation
                    COMMAND test-host-static-pool-integration worker-exit
                        $<TARGET_FILE:capsid-worker>)
            endif()
            if(NOT WIN32)
                set_tests_properties(
                    host_static_pool_activation_barrier
                    host_static_pool_worker_exit_isolation PROPERTIES
                    LABELS "host;integration;m2"
                    TIMEOUT 30)
                # The worker-exit scenario needs /proc evidence: on macOS it
                # reports SKIP (77) after the portable scenarios run.
                set_tests_properties(
                    host_static_pool_worker_exit_isolation PROPERTIES
                    SKIP_RETURN_CODE 77)
            endif()

            find_program(CAPSID_HOST_TEST_NODE NAMES node REQUIRED)
            # The public development data planes are portable; managed
            # requires Linux. Use one mode list for every WebSocket family.
            set(CAPSID_WS_SECONDARY_MODES static-pool)
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                list(APPEND CAPSID_WS_SECONDARY_MODES managed)
            endif()
            set(CAPSID_WS_BACKEND_MODES single-worker ${CAPSID_WS_SECONDARY_MODES})
            # Original ABI v7 consumer: compiled against the frozen
            # v0.2.2 snapshot with compile-time size assertions.
            add_executable(
                test-abi-v7-original-consumer
                tests/test_abi_v7_original_consumer.c)
            target_include_directories(
                test-abi-v7-original-consumer PRIVATE tests/compat)
            target_link_libraries(
                test-abi-v7-original-consumer PRIVATE capsid_runtime)
            add_test(
                NAME abi_v7_original_consumer
                COMMAND test-abi-v7-original-consumer
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                abi_v7_original_consumer PROPERTIES
                LABELS "abi"
                TIMEOUT 20)

            # Protocol options descriptor: two-way size negotiation,
            # spawn_ex gating, protocol-info lifecycle.
            add_executable(
                test-protocol-options
                tests/test_protocol_options.cc)
            target_include_directories(
                test-protocol-options PRIVATE include src
                "${CAPSID_GENERATED_DIR}")
            target_link_libraries(
                test-protocol-options PRIVATE capsid_runtime)
            add_test(
                NAME protocol_options_size_negotiation
                COMMAND test-protocol-options
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                protocol_options_size_negotiation PROPERTIES
                LABELS "abi;protocol"
                TIMEOUT 30)

            # Spawn failure recovery (T02): rejected spawns leave no
            # processes or descriptors behind.
            add_executable(
                test-spawn-failure-recovery
                tests/test_spawn_failure_recovery.cc)
            target_include_directories(
                test-spawn-failure-recovery PRIVATE include
                "${CAPSID_GENERATED_DIR}")
            target_link_libraries(
                test-spawn-failure-recovery PRIVATE capsid_runtime)
            if(APPLE)
                target_link_libraries(test-spawn-failure-recovery PRIVATE proc)
            endif()
            add_test(
                NAME spawn_failure_recovery
                COMMAND test-spawn-failure-recovery
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                spawn_failure_recovery PROPERTIES
                LABELS "worker;abi"
                TIMEOUT 60)
            if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Windows|Darwin)$")
                set_tests_properties(spawn_failure_recovery PROPERTIES SKIP_RETURN_CODE 77)
            endif()

            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                # Exercise actual forced exits with delayed nonblocking wait
                # observations and EINTR. Link wrapping is confined to this
                # test executable; the production runtime has no test hook.
                add_executable(test-worker-forced-reap
                    tests/test_worker_forced_reap.cc)
                target_include_directories(test-worker-forced-reap PRIVATE
                    include src "${CAPSID_GENERATED_DIR}")
                target_link_libraries(test-worker-forced-reap PRIVATE capsid_runtime)
                target_link_options(test-worker-forced-reap PRIVATE
                    "-Wl,--wrap=waitpid" "-Wl,--wrap=kill")
                add_test(NAME worker_forced_reap
                    COMMAND test-worker-forced-reap $<TARGET_FILE:capsid-worker>)
                set_tests_properties(worker_forced_reap PROPERTIES
                    LABELS "worker;abi;lifecycle" TIMEOUT 15)
            endif()

            add_executable(
                test-worker-v4-negotiation
                tests/test_worker_v4_negotiation.cc)
            target_include_directories(
                test-worker-v4-negotiation PRIVATE include src
                "${CAPSID_GENERATED_DIR}")
            target_link_libraries(
                test-worker-v4-negotiation PRIVATE capsid_runtime)
            add_test(
                NAME worker_v4_negotiation
                COMMAND test-worker-v4-negotiation
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                worker_v4_negotiation PROPERTIES
                LABELS "worker;protocol"
                TIMEOUT 30)
            add_executable(test-worker-websocket tests/test_worker_websocket.cc)
            target_include_directories(test-worker-websocket PRIVATE include src)
            target_link_libraries(test-worker-websocket PRIVATE capsid_runtime)
            add_dependencies(test-worker-websocket capsid-worker)
            add_test(NAME worker_websocket COMMAND test-worker-websocket $<TARGET_FILE:capsid-worker>)
            set_tests_properties(worker_websocket PROPERTIES LABELS "worker;protocol;websocket" TIMEOUT 30)
            add_executable(test-worker-channel-discard tests/test_worker_channel_discard.cc)
            target_include_directories(test-worker-channel-discard PRIVATE include src)
            target_link_libraries(test-worker-channel-discard PRIVATE capsid_runtime)
            add_dependencies(test-worker-channel-discard capsid-worker)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-worker-channel-discard PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-worker-channel-discard PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            add_test(NAME worker_channel_cancel_inflight_data
                COMMAND test-worker-channel-discard $<TARGET_FILE:capsid-worker>)
            set_tests_properties(worker_channel_cancel_inflight_data PROPERTIES
                LABELS "worker;protocol;fetch;backpressure" TIMEOUT 30)
            add_executable(test-fetch-response-limits tests/test_fetch_response_limits.cc)
            target_include_directories(test-fetch-response-limits PRIVATE include src)
            target_link_libraries(test-fetch-response-limits PRIVATE capsid_runtime)
            add_dependencies(test-fetch-response-limits capsid-worker)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-fetch-response-limits PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-fetch-response-limits PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            add_test(NAME fetch_response_limits_native_abi
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fetch_response_limits.mjs"
                    $<TARGET_FILE:test-fetch-response-limits> $<TARGET_FILE:capsid-worker>)
            set_tests_properties(fetch_response_limits_native_abi PROPERTIES
                LABELS "worker;protocol;fetch;backpressure" TIMEOUT 60)
            add_test(NAME fetch_response_limit_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fetch_response_limit_contract.mjs")
            set_tests_properties(fetch_response_limit_contract_controls PROPERTIES
                LABELS "worker;fetch;compatibility" TIMEOUT 30)
            add_test(NAME fetch_response_limits_encoded_upgrade_abi
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fetch_response_limits.mjs"
                    $<TARGET_FILE:test-fetch-response-limits> $<TARGET_FILE:capsid-worker>
                    --encoded-headers)
            set_tests_properties(fetch_response_limits_encoded_upgrade_abi PROPERTIES
                LABELS "worker;protocol;fetch;backpressure" TIMEOUT 60)
            # Header readiness and per-read acknowledgments keep source EOF
            # behind the consumer. Gzip and raw run against the same locked
            # primary program, including cancellation and whole-body controls.
            if(TARGET capsid-host)
                foreach(CAPSID_DEFAULT_READER_TRANSPORT ordinary upgrade)
                    add_test(NAME native_fetch_default_reader_gates_${CAPSID_DEFAULT_READER_TRANSPORT}
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_default_reader_gates.mjs"
                            --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                            --transport ${CAPSID_DEFAULT_READER_TRANSPORT})
                    set_tests_properties(native_fetch_default_reader_gates_${CAPSID_DEFAULT_READER_TRANSPORT}
                        PROPERTIES TIMEOUT 120 LABELS "worker;fetch;compatibility;backpressure;lifecycle")
                endforeach()
            endif()
            add_test(NAME native_fetch_default_reader_gates_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_default_reader_gates_contract.mjs")
            set_tests_properties(native_fetch_default_reader_gates_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;fetch;compatibility;backpressure;lifecycle")

            # Ordinary HTTP uses a real TCP source and the configured C ABI.
            # Workerd supplies the unrestricted native state; Capsid's decoded
            # quota extension has an independent policy and byte/backing model.
            add_executable(test-native-fetch-response-limits tests/test_native_fetch_response_limits.cc)
            add_test(NAME native_fetch_request_targets
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_request_targets.mjs"
                    --helper $<TARGET_FILE:test-native-fetch-response-limits>
                    --worker $<TARGET_FILE:capsid-worker>)
            set_tests_properties(native_fetch_request_targets PROPERTIES
                TIMEOUT 120 LABELS "worker;fetch;compatibility;uri")
            target_include_directories(test-native-fetch-response-limits PRIVATE include src)
            target_link_libraries(test-native-fetch-response-limits PRIVATE capsid_runtime)
            add_dependencies(test-native-fetch-response-limits capsid-worker)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-native-fetch-response-limits PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-native-fetch-response-limits PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            # Ordinary native fetch arguments use the same actual default
            # reference program as the archived argument captures, outside RPC
            # and Upgrade. Keep the helper's5s event and2.5s natural-exit limits.
            foreach(CAPSID_NATIVE_ARGUMENT_KIND raw gzip)
                foreach(CAPSID_NATIVE_ARGUMENT_ROUTE getReader direct prototype subclass reflect)
                    add_test(NAME native_fetch_arguments_${CAPSID_NATIVE_ARGUMENT_KIND}_${CAPSID_NATIVE_ARGUMENT_ROUTE}
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_arguments.mjs"
                            --helper $<TARGET_FILE:test-native-fetch-response-limits>
                            --worker $<TARGET_FILE:capsid-worker>
                            --kind ${CAPSID_NATIVE_ARGUMENT_KIND}
                            --route ${CAPSID_NATIVE_ARGUMENT_ROUTE})
                    set_tests_properties(native_fetch_arguments_${CAPSID_NATIVE_ARGUMENT_KIND}_${CAPSID_NATIVE_ARGUMENT_ROUTE}
                        PROPERTIES TIMEOUT 120 LABELS "worker;fetch;compatibility;lifecycle")
                endforeach()
            endforeach()
            add_test(NAME native_fetch_arguments_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_arguments_contract.mjs")
            set_tests_properties(native_fetch_arguments_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;fetch;compatibility;lifecycle")

            # Two overlapping ordinary HTTP requests borrow one held BYOB
            # reader; a third request proves reuse before natural OS exit.
            add_executable(test-native-reader-context tests/test_native_reader_context.cc)
            target_include_directories(test-native-reader-context PRIVATE include src tests)
            target_link_libraries(test-native-reader-context PRIVATE capsid_runtime)
            add_dependencies(test-native-reader-context capsid-worker)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-native-reader-context PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-native-reader-context PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            foreach(CAPSID_NATIVE_READER_CONTEXT_SHARD released retained)
                add_test(NAME native_reader_context_${CAPSID_NATIVE_READER_CONTEXT_SHARD}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_reader_context.mjs"
                        --helper $<TARGET_FILE:test-native-reader-context>
                        --worker $<TARGET_FILE:capsid-worker>
                        --shard ${CAPSID_NATIVE_READER_CONTEXT_SHARD})
                set_tests_properties(native_reader_context_${CAPSID_NATIVE_READER_CONTEXT_SHARD}
                    PROPERTIES TIMEOUT 60 LABELS "worker;fetch;compatibility;lifecycle;context")
            endforeach()
            add_test(NAME native_reader_context_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_reader_context_contract.mjs")
            set_tests_properties(native_reader_context_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;fetch;compatibility;lifecycle;context")

            # A pending owner read must outrank a foreign-context failure.
            # Each group retains the helper's original5s event and2500ms exit
            # deadlines and also checks source FIN, wire credit and reuse.
            foreach(CAPSID_READER_MATRIX_BODY raw gzip)
                foreach(CAPSID_READER_MATRIX_OWNER none pending)
                    foreach(CAPSID_READER_MATRIX_ROUTE getReader direct prototype subclass reflect)
                        set(CAPSID_READER_MATRIX_NAME
                            native_reader_context_matrix_${CAPSID_READER_MATRIX_BODY}_${CAPSID_READER_MATRIX_OWNER}_${CAPSID_READER_MATRIX_ROUTE})
                        add_test(NAME ${CAPSID_READER_MATRIX_NAME}
                            COMMAND "${CAPSID_HOST_TEST_NODE}"
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_reader_context_matrix.mjs"
                                --helper $<TARGET_FILE:test-native-reader-context>
                                --worker $<TARGET_FILE:capsid-worker>
                                --body-kind ${CAPSID_READER_MATRIX_BODY}
                                --owner-mode ${CAPSID_READER_MATRIX_OWNER}
                                --route ${CAPSID_READER_MATRIX_ROUTE})
                        set_tests_properties(${CAPSID_READER_MATRIX_NAME}
                            PROPERTIES TIMEOUT 60 LABELS "worker;fetch;compatibility;lifecycle;context")
                    endforeach()
                endforeach()
            endforeach()
            add_test(NAME native_reader_context_matrix_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_reader_context_matrix_contract.mjs")
            set_tests_properties(native_reader_context_matrix_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;fetch;compatibility;lifecycle;context")

            # Native HTTP branch graphs retain a real3-byte source prefix
            # and hold the suffix through two request barriers. The helper
            # retains its5s event and2500ms natural-exit budgets.
            foreach(CAPSID_NATIVE_BRANCH_GRAPH_GROUP
                    held_raw held_gzip checkpoints cancel controls_raw controls_gzip priority_raw priority_gzip)
                add_test(NAME native_fetch_branch_graph_${CAPSID_NATIVE_BRANCH_GRAPH_GROUP}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_branch_graph.mjs"
                        --helper $<TARGET_FILE:test-native-reader-context>
                        --worker $<TARGET_FILE:capsid-worker>
                        --group ${CAPSID_NATIVE_BRANCH_GRAPH_GROUP})
                set_tests_properties(native_fetch_branch_graph_${CAPSID_NATIVE_BRANCH_GRAPH_GROUP}
                    PROPERTIES TIMEOUT 60 LABELS "worker;fetch;compatibility;lifecycle;ownership")
            endforeach()
            add_test(NAME native_branch_graph_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_branch_graph_contract.mjs")
            set_tests_properties(native_branch_graph_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;fetch;compatibility;lifecycle;ownership")

            add_executable(test-native-retired-ownership tests/test_native_retired_ownership.cc)
            target_include_directories(test-native-retired-ownership PRIVATE include src tests)
            target_link_libraries(test-native-retired-ownership PRIVATE capsid_runtime)
            add_dependencies(test-native-retired-ownership capsid-worker)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-native-retired-ownership PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-native-retired-ownership PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            # Retire the actual owner and upstream before the observer request.
            # Every group preserves the native5s event and2500ms exit budgets.
            foreach(CAPSID_RETIRED_OWNERSHIP_GROUP
                    promise_direct promise_await promise_await-then promise_await-prelude promise_await-nested promise_new-promise-adopt promise_resolve-adopt promise_all-one promise_all-resolved promise_race-one promise_any-one promise_all-settled promise_finally promise_catch promise_then-adopt promise_async-return branches iterator_next iterator_prevent iterator_loop iterator_prelude controls)
                add_test(NAME native_retired_ownership_${CAPSID_RETIRED_OWNERSHIP_GROUP}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_retired_ownership.mjs"
                        --helper $<TARGET_FILE:test-native-retired-ownership>
                        --worker $<TARGET_FILE:capsid-worker>
                        --group ${CAPSID_RETIRED_OWNERSHIP_GROUP})
                set_tests_properties(native_retired_ownership_${CAPSID_RETIRED_OWNERSHIP_GROUP}
                    PROPERTIES TIMEOUT 60 LABELS "worker;fetch;compatibility;lifecycle;ownership")
            endforeach()
            add_test(NAME native_retired_ownership_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_retired_ownership_contract.mjs")
            set_tests_properties(native_retired_ownership_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;fetch;compatibility;lifecycle;ownership")

            # Exact programs and both native workerd primary assets are archived.
            # Keep the C ABI helper's original5s event and2500ms exit budgets.
            foreach(CAPSID_RUNTIME_EVENT_KIND api mutations)
                add_test(NAME native_runtime_event_${CAPSID_RUNTIME_EVENT_KIND}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_runtime_events.mjs"
                        --helper $<TARGET_FILE:test-native-fetch-response-limits>
                        --worker $<TARGET_FILE:capsid-worker>
                        --kind ${CAPSID_RUNTIME_EVENT_KIND})
            endforeach()
            set_tests_properties(native_runtime_event_api
                PROPERTIES TIMEOUT 30 LABELS "worker;events;compatibility;lifecycle")
            set_tests_properties(native_runtime_event_mutations
                PROPERTIES TIMEOUT 60 LABELS "worker;events;compatibility;lifecycle")
            add_test(NAME native_runtime_event_primary_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_runtime_events_contract.mjs")
            set_tests_properties(native_runtime_event_primary_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;events;compatibility;lifecycle")

            # Keep the ordinary helper's5s deadline. This separate composed
            # probe can also wait for the unchanged3s owner-held source gate.
            add_executable(test-native-fetch-encoded-owner tests/test_native_fetch_response_limits.cc)
            target_compile_definitions(test-native-fetch-encoded-owner PRIVATE CAPSID_TEST_FETCH_EVENT_SECONDS=10)
            target_include_directories(test-native-fetch-encoded-owner PRIVATE include src)
            target_link_libraries(test-native-fetch-encoded-owner PRIVATE capsid_runtime)
            add_dependencies(test-native-fetch-encoded-owner capsid-worker)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-native-fetch-encoded-owner PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-native-fetch-encoded-owner PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            foreach(CAPSID_ENCODED_KIND IN ITEMS finished held owner)
                set(CAPSID_ENCODED_HELPER test-native-fetch-response-limits)
                set(CAPSID_ENCODED_TIMEOUT 420)
                if(CAPSID_ENCODED_KIND STREQUAL "held")
                    set(CAPSID_ENCODED_TIMEOUT 180)
                elseif(CAPSID_ENCODED_KIND STREQUAL "owner")
                    set(CAPSID_ENCODED_HELPER test-native-fetch-encoded-owner)
                    set(CAPSID_ENCODED_TIMEOUT 90)
                endif()
                add_test(NAME native_fetch_encoded_${CAPSID_ENCODED_KIND}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_encoded_branches.mjs"
                        --helper $<TARGET_FILE:${CAPSID_ENCODED_HELPER}>
                        --worker $<TARGET_FILE:capsid-worker>
                        --kind "${CAPSID_ENCODED_KIND}")
                set_tests_properties(native_fetch_encoded_${CAPSID_ENCODED_KIND} PROPERTIES
                    TIMEOUT ${CAPSID_ENCODED_TIMEOUT}
                    LABELS "worker;fetch;compression;compatibility;lifecycle")
            endforeach()
            add_test(NAME native_fetch_encoded_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_encoded_contract.mjs")
            set_tests_properties(native_fetch_encoded_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "worker;fetch;compression;compatibility;lifecycle")

            # The embedding host returns consumed response credit. Exercise
            # both sides of the default256KiB window and three full windows;
            # keep the original helper event deadline and healthy request.
            add_test(NAME native_fetch_response_credit
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_response_credit.mjs"
                    --helper $<TARGET_FILE:test-native-fetch-response-limits>
                    --worker $<TARGET_FILE:capsid-worker>)
            set_tests_properties(native_fetch_response_credit PROPERTIES
                LABELS "worker;protocol;fetch;backpressure" TIMEOUT 30)
            add_test(NAME native_fetch_response_credit_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_response_credit_contract.mjs")
            set_tests_properties(native_fetch_response_credit_contract_controls PROPERTIES
                LABELS "worker;protocol;fetch;backpressure" TIMEOUT 30)

            foreach(CAPSID_NATIVE_LIMIT_SHARD IN ITEMS boundaries geometry)
                add_test(NAME native_fetch_limits_${CAPSID_NATIVE_LIMIT_SHARD}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_response_limits.mjs"
                        --helper $<TARGET_FILE:test-native-fetch-response-limits>
                        --worker $<TARGET_FILE:capsid-worker>
                        --shard ${CAPSID_NATIVE_LIMIT_SHARD})
                set_tests_properties(native_fetch_limits_${CAPSID_NATIVE_LIMIT_SHARD} PROPERTIES
                    LABELS "worker;protocol;fetch;backpressure;compatibility" TIMEOUT 60)
            endforeach()
            add_test(NAME native_fetch_limit_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_limit_contract.mjs")
            set_tests_properties(native_fetch_limit_contract_controls PROPERTIES
                LABELS "worker;fetch;compatibility" TIMEOUT 30)
            # Real ordinary-response cancellation/retirement, with actual
            # pinned workerd state and separate per-request cleanup policy.
            # Exact gzip member boundaries, including empty members, keep
            # their independent actual-workerd state and full backing hashes.
            add_test(NAME native_fetch_gzip_members
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_gzip_members.mjs"
                    --helper $<TARGET_FILE:test-native-fetch-response-limits>
                    --worker $<TARGET_FILE:capsid-worker>)
            set_tests_properties(native_fetch_gzip_members PROPERTIES
                TIMEOUT 60 LABELS "worker;fetch;compression;compatibility")
            add_test(NAME native_fetch_gzip_member_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_gzip_member_contract.mjs")
            set_tests_properties(native_fetch_gzip_member_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "worker;fetch;compression;compatibility")

            # Large ordinary gzip reads stay within the default64MiB JS heap
            # and the original5s C API event deadline. Check every payload and
            # backing byte, actual producer EOF and natural worker exit.
            add_test(NAME native_fetch_large_gzip
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_large_gzip.mjs"
                    --helper $<TARGET_FILE:test-native-fetch-response-limits>
                    --worker $<TARGET_FILE:capsid-worker>)
            set_tests_properties(native_fetch_large_gzip PROPERTIES
                TIMEOUT 60 LABELS "worker;fetch;compression;compatibility")
            add_test(NAME native_fetch_large_gzip_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_large_gzip_contract.mjs")
            set_tests_properties(native_fetch_large_gzip_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "worker;fetch;compression;compatibility")

            # After a real64-byte read, raw abort rejects further input;
            # gzip drains its accepted encoded prefix before the same error.
            add_test(NAME native_fetch_abort_prefix
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_abort_prefix.mjs"
                    --helper $<TARGET_FILE:test-native-fetch-response-limits>
                    --worker $<TARGET_FILE:capsid-worker>)
            set_tests_properties(native_fetch_abort_prefix PROPERTIES
                TIMEOUT 60 LABELS "worker;fetch;compression;compatibility;lifecycle")
            add_test(NAME native_fetch_abort_prefix_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_abort_prefix_contract.mjs")
            set_tests_properties(native_fetch_abort_prefix_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "worker;fetch;compression;compatibility;lifecycle")

            # A real native read is pending while a separate control request
            # proves no source body byte has been released. Keep full typed
            # results, backing, promise order and natural process exits.
            foreach(CAPSID_NATIVE_PENDING_SHARD IN ITEMS
                    raw_overlap raw_cancel raw_abort gzip_overlap gzip_cancel gzip_abort)
                add_test(NAME native_fetch_pending_${CAPSID_NATIVE_PENDING_SHARD}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_pending.mjs"
                        --helper $<TARGET_FILE:test-native-fetch-response-limits>
                        --worker $<TARGET_FILE:capsid-worker>
                        --shard ${CAPSID_NATIVE_PENDING_SHARD})
                set_tests_properties(native_fetch_pending_${CAPSID_NATIVE_PENDING_SHARD} PROPERTIES
                    TIMEOUT 60 LABELS "worker;fetch;lifecycle;compatibility")
            endforeach()
            add_test(NAME native_fetch_pending_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_pending_contract.mjs")
            set_tests_properties(native_fetch_pending_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "worker;fetch;compatibility")

            # Actual native I/O at public signal timeout and host request
            # deadline. Keep real pending/unread state, full ABI events,
            # peer closure before cleanup, healthy reuse and natural exit.
            add_executable(test-native-fetch-deadline tests/test_native_fetch_deadline.cc)
            target_include_directories(test-native-fetch-deadline PRIVATE include src)
            target_link_libraries(test-native-fetch-deadline PRIVATE capsid_runtime)
            add_dependencies(test-native-fetch-deadline capsid-worker)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-native-fetch-deadline PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-native-fetch-deadline PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            foreach(CAPSID_NATIVE_DEADLINE_SHARD IN ITEMS raw_signal gzip_signal raw_host gzip_host)
                add_test(NAME native_fetch_deadline_${CAPSID_NATIVE_DEADLINE_SHARD}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_deadline.mjs"
                        --helper $<TARGET_FILE:test-native-fetch-deadline>
                        --worker $<TARGET_FILE:capsid-worker>
                        --shard ${CAPSID_NATIVE_DEADLINE_SHARD})
                set_tests_properties(native_fetch_deadline_${CAPSID_NATIVE_DEADLINE_SHARD} PROPERTIES
                    TIMEOUT 60 LABELS "worker;fetch;lifecycle;compatibility")
            endforeach()
            add_test(NAME native_fetch_deadline_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_deadline_contract.mjs")
            set_tests_properties(native_fetch_deadline_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "worker;fetch;compatibility")

            # Partial snapshot and Promise-backed response retirement.
            add_executable(test-worker-response-retirement tests/test_worker_response_retirement.cc)
            target_include_directories(test-worker-response-retirement PRIVATE include src)
            target_link_libraries(test-worker-response-retirement PRIVATE capsid_runtime)
            add_dependencies(test-worker-response-retirement capsid-worker)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-worker-response-retirement PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-worker-response-retirement PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            if(APPLE OR CAPSID_ENABLE_ASAN OR CAPSID_ENABLE_TSAN)
                set(CAPSID_RETIREMENT_PROCESS_MEMORY 0)
            else()
                set(CAPSID_RETIREMENT_PROCESS_MEMORY 268435456)
            endif()
            foreach(CAPSID_RETIREMENT_ACTION IN ITEMS timeout cancel grant shutdown)
                add_test(NAME worker_response_retirement_${CAPSID_RETIREMENT_ACTION}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_worker_response_retirement.mjs"
                        --helper $<TARGET_FILE:test-worker-response-retirement>
                        --worker $<TARGET_FILE:capsid-worker>
                        --action ${CAPSID_RETIREMENT_ACTION}
                        --process-memory ${CAPSID_RETIREMENT_PROCESS_MEMORY}
                        --identity-header "${CAPSID_GENERATED_DIR}/build_identity.h")
                set_tests_properties(worker_response_retirement_${CAPSID_RETIREMENT_ACTION} PROPERTIES
                    TIMEOUT 30 LABELS "worker;fetch;lifecycle;backpressure")
            endforeach()
            add_test(NAME worker_response_retirement_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_worker_response_retirement_contract.mjs")
            set_tests_properties(worker_response_retirement_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "worker;fetch;lifecycle;backpressure")

            add_executable(test-worker-poll-shutdown tests/test_worker_poll_shutdown.cc)
            target_include_directories(test-worker-poll-shutdown PRIVATE include src)
            target_link_libraries(test-worker-poll-shutdown PRIVATE capsid_runtime)
            add_dependencies(test-worker-poll-shutdown capsid-worker)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-worker-poll-shutdown PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-worker-poll-shutdown PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            foreach(CAPSID_POLL_SHUTDOWN_ORDER IN ITEMS before after)
                add_test(NAME worker_poll_shutdown_${CAPSID_POLL_SHUTDOWN_ORDER}
                    COMMAND test-worker-poll-shutdown $<TARGET_FILE:capsid-worker>
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/response-retirement.mjs"
                        ${CAPSID_POLL_SHUTDOWN_ORDER})
                set_tests_properties(worker_poll_shutdown_${CAPSID_POLL_SHUTDOWN_ORDER} PROPERTIES
                    TIMEOUT 10 LABELS "worker;protocol;lifecycle")
            endforeach()

            # Small explicit response windows keep the existing helper
            # protocols and prove consumed credit is returned on every path.
            foreach(CAPSID_HELPER_CREDIT_KIND IN ITEMS deadline limits limits_encoded discard negotiation)
                if(CAPSID_HELPER_CREDIT_KIND STREQUAL "deadline")
                    set(CAPSID_HELPER_CREDIT_TARGET test-native-fetch-deadline)
                elseif(CAPSID_HELPER_CREDIT_KIND MATCHES "^limits")
                    set(CAPSID_HELPER_CREDIT_TARGET test-fetch-response-limits)
                elseif(CAPSID_HELPER_CREDIT_KIND STREQUAL "discard")
                    set(CAPSID_HELPER_CREDIT_TARGET test-worker-channel-discard)
                else()
                    set(CAPSID_HELPER_CREDIT_TARGET test-worker-v4-negotiation)
                endif()
                add_test(NAME helper_response_credit_${CAPSID_HELPER_CREDIT_KIND}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_helper_response_credit.mjs"
                        --helper $<TARGET_FILE:${CAPSID_HELPER_CREDIT_TARGET}>
                        --worker $<TARGET_FILE:capsid-worker>
                        --kind ${CAPSID_HELPER_CREDIT_KIND})
                if(CAPSID_HELPER_CREDIT_KIND MATCHES "^limits")
                    set(CAPSID_HELPER_CREDIT_TIMEOUT 60)
                else()
                    set(CAPSID_HELPER_CREDIT_TIMEOUT 30)
                endif()
                set_tests_properties(helper_response_credit_${CAPSID_HELPER_CREDIT_KIND} PROPERTIES
                    TIMEOUT ${CAPSID_HELPER_CREDIT_TIMEOUT} LABELS "worker;protocol;fetch;backpressure")
            endforeach()
            add_test(NAME helper_response_credit_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_helper_response_credit_contract.mjs")
            set_tests_properties(helper_response_credit_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "worker;protocol;fetch;backpressure")

            # Real Binding VM I/O and structured RPC, with pinned User I/O
            # primaries and separate models for Capsid's clone/context policy.
            capsid_add_binding_test(
                test-binding-fetch-body tests/test_binding_fetch_body.cc)
            target_link_libraries(test-binding-fetch-body PRIVATE capsid_runtime)
            add_dependencies(test-binding-fetch-body capsid-worker)
            foreach(CAPSID_BINDING_FETCH_KIND raw gzip)
                add_test(NAME binding_fetch_body_${CAPSID_BINDING_FETCH_KIND}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_fetch_body.mjs"
                        --helper $<TARGET_FILE:test-binding-fetch-body>
                        --worker $<TARGET_FILE:capsid-worker>
                        --kind ${CAPSID_BINDING_FETCH_KIND})
                set_tests_properties(binding_fetch_body_${CAPSID_BINDING_FETCH_KIND}
                    PROPERTIES TIMEOUT 60 LABELS "worker;binding;fetch;compatibility;lifecycle")
            endforeach()
            add_test(NAME binding_fetch_body_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_fetch_body_contract.mjs")
            set_tests_properties(binding_fetch_body_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;binding;fetch;compatibility;lifecycle")

            # Full argument/retry semantics in the actual Binding VM. Each
            # route/kind shard covers13 views and44 read/40 readAtLeast inputs.
            # Reuse the existing C API helper; its15s event and45s peer limits
            # and the default JS heap/RPC clone policy remain unchanged.
            foreach(CAPSID_BINDING_ARGUMENT_KIND raw gzip)
                foreach(CAPSID_BINDING_ARGUMENT_ROUTE getReader direct prototype subclass reflect)
                    add_test(NAME binding_fetch_arguments_${CAPSID_BINDING_ARGUMENT_KIND}_${CAPSID_BINDING_ARGUMENT_ROUTE}
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_fetch_arguments.mjs"
                            --helper $<TARGET_FILE:test-binding-fetch-body>
                            --worker $<TARGET_FILE:capsid-worker>
                            --kind ${CAPSID_BINDING_ARGUMENT_KIND}
                            --route ${CAPSID_BINDING_ARGUMENT_ROUTE})
                    set_tests_properties(binding_fetch_arguments_${CAPSID_BINDING_ARGUMENT_KIND}_${CAPSID_BINDING_ARGUMENT_ROUTE}
                        PROPERTIES TIMEOUT 120 LABELS "worker;binding;fetch;compatibility;lifecycle")
                endforeach()
            endforeach()
            add_test(NAME binding_fetch_arguments_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_fetch_arguments_contract.mjs")
            set_tests_properties(binding_fetch_arguments_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;binding;fetch;compatibility;lifecycle" RUN_SERIAL TRUE)

            # A BindingID owns native handles across calls and User requests.
            # Hold the actual source at headers until request1 has ended and
            # the host begins request2; then check every read and real FIN/exit.
            capsid_add_binding_test(
                test-binding-fetch-retained tests/test_binding_fetch_retained.cc)
            target_link_libraries(test-binding-fetch-retained PRIVATE capsid_runtime)
            add_dependencies(test-binding-fetch-retained capsid-worker)
            foreach(CAPSID_RETAINED_BINDING_KIND raw gzip)
                foreach(CAPSID_RETAINED_BINDING_ROUTE getReader direct prototype subclass reflect)
                    add_test(NAME binding_fetch_retained_${CAPSID_RETAINED_BINDING_KIND}_${CAPSID_RETAINED_BINDING_ROUTE}
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_fetch_retained.mjs"
                            --helper $<TARGET_FILE:test-binding-fetch-retained>
                            --worker $<TARGET_FILE:capsid-worker>
                            --kind ${CAPSID_RETAINED_BINDING_KIND}
                            --route ${CAPSID_RETAINED_BINDING_ROUTE})
                    set_tests_properties(binding_fetch_retained_${CAPSID_RETAINED_BINDING_KIND}_${CAPSID_RETAINED_BINDING_ROUTE}
                        PROPERTIES TIMEOUT 60 LABELS "worker;binding;fetch;compatibility;lifecycle")
                endforeach()
            endforeach()
            add_test(NAME binding_fetch_retained_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_fetch_retained_contract.mjs")
            set_tests_properties(binding_fetch_retained_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;binding;fetch;compatibility;lifecycle")

            # Binding-owned unread or pending native resources survive both
            # User requests, then close on actual C API Worker stop. Exercise
            # one/four independent peers without a fixture-provided body/EOF.
            foreach(CAPSID_UNREAD_BINDING_KIND raw gzip)
                add_test(NAME binding_fetch_unread_${CAPSID_UNREAD_BINDING_KIND}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_fetch_unread.mjs"
                        --helper $<TARGET_FILE:test-binding-fetch-retained>
                        --worker $<TARGET_FILE:capsid-worker>
                        --kind ${CAPSID_UNREAD_BINDING_KIND})
                set_tests_properties(binding_fetch_unread_${CAPSID_UNREAD_BINDING_KIND} PROPERTIES
                    TIMEOUT 60 LABELS "worker;binding;fetch;lifecycle;compatibility")
            endforeach()
            add_test(NAME binding_fetch_unread_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_fetch_unread_contract.mjs")
            set_tests_properties(binding_fetch_unread_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "worker;binding;fetch;lifecycle;compatibility")

            # Native fetch state must not call application replacements of
            # WeakMap get/set. Compare both request results to actual pinned
            # workerd observations, including real source FIN before teardown.
            foreach(CAPSID_INTRINSIC_FETCH_KIND raw gzip)
                add_test(NAME native_fetch_intrinsic_${CAPSID_INTRINSIC_FETCH_KIND}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_intrinsic.mjs"
                        --helper $<TARGET_FILE:test-binding-fetch-retained>
                        --worker $<TARGET_FILE:capsid-worker>
                        --kind ${CAPSID_INTRINSIC_FETCH_KIND})
                set_tests_properties(native_fetch_intrinsic_${CAPSID_INTRINSIC_FETCH_KIND} PROPERTIES
                    TIMEOUT 60 LABELS "worker;binding;fetch;compatibility;lifecycle")
            endforeach()
            add_test(NAME native_fetch_intrinsic_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_intrinsic_contract.mjs")
            set_tests_properties(native_fetch_intrinsic_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "worker;binding;fetch;compatibility;lifecycle")

            # Both native branches stay pending after a real137-byte prefix
            # until the issued32KiB/64KiB minimum, EOF, cancel or abort settles.
            # Keep full backing, exact control wire and the original C ABI helper.
            foreach(CAPSID_BRANCH_PREFIX_METHOD IN ITEMS wide full)
                add_test(NAME native_fetch_branch_prefix_${CAPSID_BRANCH_PREFIX_METHOD}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_branch_prefix.mjs"
                        --helper $<TARGET_FILE:test-binding-fetch-retained>
                        --worker $<TARGET_FILE:capsid-worker>
                        --method ${CAPSID_BRANCH_PREFIX_METHOD})
                set_tests_properties(native_fetch_branch_prefix_${CAPSID_BRANCH_PREFIX_METHOD} PROPERTIES
                    TIMEOUT 60 LABELS "worker;binding;fetch;compatibility;lifecycle")
            endforeach()
            add_test(NAME native_fetch_branch_prefix_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_branch_prefix_contract.mjs")
            set_tests_properties(native_fetch_branch_prefix_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "worker;binding;fetch;compatibility;lifecycle")

            # Request completion cancels pending Binding calls. A call.signal
            # fetch/read must close while an unrelated BindingID-owned retained
            # connection survives and is fully read by the next User request.
            # Reuse the real two-request helper and original event/heap bounds.
            foreach(CAPSID_CANCEL_BINDING_KIND raw gzip)
                foreach(CAPSID_CANCEL_BINDING_ROUTE getReader direct prototype subclass reflect)
                    add_test(NAME binding_fetch_cancel_${CAPSID_CANCEL_BINDING_KIND}_${CAPSID_CANCEL_BINDING_ROUTE}
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_fetch_cancel.mjs"
                            --helper $<TARGET_FILE:test-binding-fetch-retained>
                            --worker $<TARGET_FILE:capsid-worker>
                            --kind ${CAPSID_CANCEL_BINDING_KIND}
                            --route ${CAPSID_CANCEL_BINDING_ROUTE})
                    set_tests_properties(binding_fetch_cancel_${CAPSID_CANCEL_BINDING_KIND}_${CAPSID_CANCEL_BINDING_ROUTE}
                        PROPERTIES TIMEOUT 60 LABELS "worker;binding;fetch;compatibility;lifecycle")
                endforeach()
            endforeach()
            add_test(NAME binding_fetch_cancel_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_fetch_cancel_contract.mjs")
            set_tests_properties(binding_fetch_cancel_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;binding;fetch;compatibility;lifecycle")

            # Pure Headers API: same99-case function in pinned workerd and
            # actual User/Binding VMs, each Binding case a separate real RPC.
            # Independent Python input model is checked against its artifact.
            add_test(NAME headers_compatibility_actual_vms
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_headers_compatibility.mjs"
                    --helper $<TARGET_FILE:test-binding-fetch-body>
                    --worker $<TARGET_FILE:capsid-worker>)
            set_tests_properties(headers_compatibility_actual_vms
                PROPERTIES TIMEOUT 60 LABELS "worker;binding;fetch;compatibility")
            add_test(NAME headers_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_headers_contract.mjs")
            set_tests_properties(headers_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;binding;fetch;compatibility;negative-controls")
            if(NOT CAPSID_PYTHON3_EXECUTABLE)
                find_program(CAPSID_PYTHON3_EXECUTABLE NAMES python3 python REQUIRED)
            endif()
            add_test(NAME headers_expected_model_source
                COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/headers-input-model.py" --check)
            set_tests_properties(headers_expected_model_source
                PROPERTIES TIMEOUT 30 LABELS "worker;binding;fetch;compatibility;provenance")

            # Binding quotas are Capsid policy:64 outstanding calls/request,
            #1024/worker. Real C API requests prove pre-clone refusal, explicit
            # host cancellation isolation, slot refill and natural exit.
            capsid_add_binding_test(test-binding-quota-worker tests/test_binding_quota_worker.cc)
            target_link_libraries(test-binding-quota-worker PRIVATE capsid_runtime)
            add_dependencies(test-binding-quota-worker capsid-worker)
            add_test(NAME binding_rpc_request_quota
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_quota.mjs"
                    --helper $<TARGET_FILE:test-binding-fetch-retained>
                    --worker $<TARGET_FILE:capsid-worker> --kind request)
            add_test(NAME binding_rpc_worker_quota
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_quota.mjs"
                    --helper $<TARGET_FILE:test-binding-quota-worker>
                    --worker $<TARGET_FILE:capsid-worker> --kind worker)
            set_tests_properties(binding_rpc_request_quota binding_rpc_worker_quota
                PROPERTIES TIMEOUT 60 LABELS "worker;binding;quota;lifecycle")
            add_test(NAME binding_rpc_quota_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding_quota_contract.mjs")
            set_tests_properties(binding_rpc_quota_contract_controls
                PROPERTIES TIMEOUT 30 LABELS "worker;binding;quota;lifecycle;negative-controls")

            add_executable(test-native-fetch-owner tests/test_native_fetch_owner.cc)
            target_include_directories(test-native-fetch-owner PRIVATE include src)
            target_link_libraries(test-native-fetch-owner PRIVATE capsid_runtime)
            add_dependencies(test-native-fetch-owner capsid-worker)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-native-fetch-owner PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-native-fetch-owner PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            foreach(CAPSID_NATIVE_OWNER_SHARD IN ITEMS cancel retirement)
                add_test(NAME native_fetch_owner_${CAPSID_NATIVE_OWNER_SHARD}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_owner.mjs"
                        --helper $<TARGET_FILE:test-native-fetch-owner>
                        --worker $<TARGET_FILE:capsid-worker>
                        --shard ${CAPSID_NATIVE_OWNER_SHARD})
                set_tests_properties(native_fetch_owner_${CAPSID_NATIVE_OWNER_SHARD} PROPERTIES
                    LABELS "worker;protocol;fetch;lifecycle;compatibility" TIMEOUT 60)
            endforeach()
            add_test(NAME native_fetch_owner_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test --test-reporter=tap
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fetch_owner_contract.mjs")
            set_tests_properties(native_fetch_owner_contract_controls PROPERTIES
                LABELS "worker;fetch;compatibility" TIMEOUT 30)
            add_executable(test-worker-outbound tests/test_worker_outbound.cc)
            target_include_directories(test-worker-outbound PRIVATE include src)
            target_link_libraries(test-worker-outbound PRIVATE capsid_runtime)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-worker-outbound PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-worker-outbound PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            add_executable(test-worker-writer-gate tests/test_worker_writer_gate.cc)
            target_include_directories(test-worker-writer-gate PRIVATE src)
            target_link_libraries(test-worker-writer-gate PRIVATE capsid_runtime)
            target_compile_features(test-worker-writer-gate PRIVATE cxx_std_11)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-worker-writer-gate PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-worker-writer-gate PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
            add_test(NAME worker_writer_gate_contract_controls COMMAND test-worker-writer-gate)
            set_tests_properties(worker_writer_gate_contract_controls PROPERTIES
                LABELS "worker;protocol;backpressure;contract" TIMEOUT 10)
            # Embed the same Worker sources, bootstrap and launch path. The
            # fixture only injects socket EAGAIN and observes actual queue
            # transitions; it cannot manufacture protocol/JS events.
            get_target_property(CAPSID_WRITER_SOURCES capsid-worker SOURCES)
            add_executable(test-worker-writer-fixture ${CAPSID_WRITER_SOURCES} tests/worker_writer_fixture.cc)
            foreach(CAPSID_WRITER_PROPERTY IN ITEMS INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS
                    CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS INTERPROCEDURAL_OPTIMIZATION)
                get_target_property(CAPSID_WRITER_VALUE capsid-worker "${CAPSID_WRITER_PROPERTY}")
                if(NOT CAPSID_WRITER_VALUE STREQUAL "CAPSID_WRITER_VALUE-NOTFOUND")
                    set_property(TARGET test-worker-writer-fixture PROPERTY "${CAPSID_WRITER_PROPERTY}" "${CAPSID_WRITER_VALUE}")
                endif()
            endforeach()
            target_compile_definitions(test-worker-writer-fixture PRIVATE CAPSID_WORKER_CUSTOM_ENTRY=1)
            target_link_libraries(test-worker-writer-fixture PRIVATE tjs capsid_sanitizers)
            add_dependencies(test-worker-writer-fixture capsid-worker)
            add_dependencies(test-worker-outbound capsid-worker test-worker-writer-fixture)
            add_test(NAME worker_outbound_websocket COMMAND test-worker-outbound $<TARGET_FILE:capsid-worker> $<TARGET_FILE:test-worker-writer-fixture>)
            set_tests_properties(worker_outbound_websocket PROPERTIES LABELS "worker;protocol;websocket" TIMEOUT 30)
            add_test(NAME worker_outbound_result_ack_pressure
                COMMAND test-worker-outbound $<TARGET_FILE:test-worker-writer-fixture> ack-pressure)
            set_tests_properties(worker_outbound_result_ack_pressure PROPERTIES LABELS "worker;protocol;backpressure" TIMEOUT 30)
            foreach(head_version IN ITEMS v3 v4)
                add_test(NAME "worker_response_head_pressure_${head_version}"
                    COMMAND test-worker-outbound $<TARGET_FILE:capsid-worker> "head-pressure-${head_version}")
                set_tests_properties("worker_response_head_pressure_${head_version}" PROPERTIES
                    LABELS "worker;protocol;backpressure" TIMEOUT 30)
            endforeach()
            add_executable(test-host-outbound-policy tests/test_host_outbound_policy.cc)
            set_target_properties(test-host-outbound-policy PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
            target_include_directories(test-host-outbound-policy PRIVATE include src)
            target_link_libraries(test-host-outbound-policy PRIVATE capsid_host_core Boost::system)
            add_dependencies(test-host-outbound-policy capsid-worker)
            add_test(NAME host_outbound_policy COMMAND test-host-outbound-policy $<TARGET_FILE:capsid-worker>)
            set_tests_properties(host_outbound_policy PROPERTIES LABELS "host;protocol;websocket" TIMEOUT 30)
            add_test(
                NAME host_ws_upgrade_gates
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_ws_upgrade.mjs"
                    --host "$<TARGET_FILE:capsid-host>"
                    --worker "$<TARGET_FILE:capsid-worker>")
            set_tests_properties(
                host_ws_upgrade_gates PROPERTIES
                LABELS "host;integration"
                TIMEOUT 30)
            foreach(ws_mode IN LISTS CAPSID_WS_SECONDARY_MODES)
                add_test(
                    NAME host_ws_upgrade_gates_${ws_mode}
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_ws_upgrade.mjs"
                        --host "$<TARGET_FILE:capsid-host>"
                        --worker "$<TARGET_FILE:capsid-worker>"
                        --mode ${ws_mode})
                set_tests_properties(host_ws_upgrade_gates_${ws_mode} PROPERTIES
                    LABELS "host;integration" TIMEOUT 45)
            endforeach()
            add_test(NAME host_cli_unknown_arguments
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_cli_unknown_arguments.mjs"
                    --host "$<TARGET_FILE:capsid-host>"
                    --worker "$<TARGET_FILE:capsid-worker>")
            set_tests_properties(host_cli_unknown_arguments PROPERTIES
                LABELS "host;integration;cli" TIMEOUT 60)
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

            # Read SDK defaults before registering any client that needs
            # them, including the first configure with environment-only inputs.
            set(CAPSID_WS_INTEROP_PYTHON "$ENV{CAPSID_WS_INTEROP_PYTHON}" CACHE FILEPATH
                "Python interpreter with the locked WebSocket interop client")
            set(CAPSID_WS_INTEROP_NODE_PACKAGE "$ENV{CAPSID_WS_INTEROP_NODE_PACKAGE}" CACHE PATH
                "Installed source of the locked Node ws test package")

            # Each stop case executes a real Host and its actual Workers.
            add_test(NAME posix_worker_process_observation_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_posix_worker_processes.mjs")
            set_tests_properties(posix_worker_process_observation_controls PROPERTIES
                LABELS "host;lifecycle;test-driver" TIMEOUT 20)

            # Windows uses the explicit parent pipe; POSIX also keeps the
            # process-signal route under the same terminal assertions.
            set(CAPSID_PARENT_STOP_CASES idle parent-eof repeat-stop active-ws
                no-close-reply pending-dial startup-fail invalid-fd ready-alias
                stdio-fd malformed-fd file-fd)
            if(NOT WIN32)
                list(APPEND CAPSID_PARENT_STOP_CASES signal)
            endif()
            foreach(stop_mode IN ITEMS single-worker static-pool)
                foreach(stop_case IN LISTS CAPSID_PARENT_STOP_CASES)
                    add_test(NAME "host_parent_stop_${stop_mode}_${stop_case}"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_parent_stop.mjs"
                            --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                            --mode "${stop_mode}" --case "${stop_case}")
                    set_tests_properties("host_parent_stop_${stop_mode}_${stop_case}" PROPERTIES
                        LABELS "host;integration;websocket;lifecycle" TIMEOUT 40)
                    if(stop_case STREQUAL "active-ws")
                        set_tests_properties("host_parent_stop_${stop_mode}_${stop_case}" PROPERTIES
                            ENVIRONMENT "CAPSID_WS_INTEROP_NODE_PACKAGE=${CAPSID_WS_INTEROP_NODE_PACKAGE}")
                    endif()
                endforeach()
            endforeach()
            add_executable(test-host-static-pool-faults
                tests/test_host_static_pool_faults.cc
                src/host/single_worker_server.cc
                ${CAPSID_STATIC_POOL_SERVER_TEST_SOURCE})
            target_include_directories(test-host-static-pool-faults PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(test-host-static-pool-faults PRIVATE
                capsid_runtime capsid_host_core Boost::system capsid_sanitizers)
            set_target_properties(test-host-static-pool-faults PROPERTIES
                CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-host-static-pool-faults PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-host-static-pool-faults PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                        target_compile_options(test-host-static-pool-faults PRIVATE -Wno-maybe-uninitialized)
                    endif()
                endif()
            endif()
            set(CAPSID_POOL_FAULT_CLIENT_ARGS)
            if(CAPSID_WS_INTEROP_NODE_PACKAGE)
                list(APPEND CAPSID_POOL_FAULT_CLIENT_ARGS --node-package "${CAPSID_WS_INTEROP_NODE_PACKAGE}")
            endif()
            foreach(pool_fault IN ITEMS first-worker-exit last-worker-exit first-worker-exit-websocket
                all-workers-exit last-ready-worker-exit all-workers-exit-websocket)
                add_test(NAME "host_static_pool_fault_${pool_fault}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_static_pool_faults.mjs"
                        --fixture $<TARGET_FILE:test-host-static-pool-faults>
                        --worker $<TARGET_FILE:capsid-worker> --case "${pool_fault}"
                        ${CAPSID_POOL_FAULT_CLIENT_ARGS})
                set_tests_properties("host_static_pool_fault_${pool_fault}" PROPERTIES
                    LABELS "host;integration;lifecycle;websocket" TIMEOUT 45)
            endforeach()
            add_executable(test-host-static-pool-adoption
                tests/test_host_static_pool_adoption.cc
                src/host/single_worker_server.cc
                ${CAPSID_STATIC_POOL_SERVER_TEST_SOURCE})
            target_include_directories(test-host-static-pool-adoption PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(test-host-static-pool-adoption PRIVATE
                capsid_runtime capsid_host_core Boost::system capsid_sanitizers)
            set_target_properties(test-host-static-pool-adoption PROPERTIES
                CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
            # Only this fixture needs local function symbols to identify a
            # real adoption allocation. Optimization remains the build's own.
            if(MSVC)
                target_compile_options(test-host-static-pool-adoption PRIVATE /Zi)
                target_link_options(test-host-static-pool-adoption PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
                target_link_libraries(test-host-static-pool-adoption PRIVATE dbghelp)
                if(CAPSID_STRICT_WARNINGS)
                    target_compile_options(test-host-static-pool-adoption PRIVATE /W4 /WX)
                endif()
            else()
                target_compile_options(test-host-static-pool-adoption PRIVATE -g -fno-omit-frame-pointer)
                target_link_libraries(test-host-static-pool-adoption PRIVATE ${CMAKE_DL_LIBS})
                if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
                   CMAKE_EXE_LINKER_FLAGS MATCHES "(^| )-static( |$)")
                    target_compile_definitions(test-host-static-pool-adoption PRIVATE
                        CAPSID_TEST_STATIC_SYMBOLS=1
                        "CAPSID_TEST_NM_PATH=\"${CMAKE_NM}\"")
                else()
                    set_target_properties(test-host-static-pool-adoption PROPERTIES ENABLE_EXPORTS ON)
                endif()
                if(CAPSID_STRICT_WARNINGS)
                    target_compile_options(test-host-static-pool-adoption PRIVATE -Wall -Wextra -Wpedantic -Werror)
                    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                        target_compile_options(test-host-static-pool-adoption PRIVATE -Wno-maybe-uninitialized)
                    endif()
                endif()
            endif()
            foreach(pool_adoption IN ITEMS reselect-worker-exit reselect-last-workers-exit
                pre-post-stop pre-post-drain active-drain)
                add_test(NAME "host_static_pool_adoption_${pool_adoption}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_static_pool_adoption.mjs"
                        --fixture $<TARGET_FILE:test-host-static-pool-adoption>
                        --worker $<TARGET_FILE:capsid-worker> --case "${pool_adoption}"
                        ${CAPSID_POOL_FAULT_CLIENT_ARGS})
                set_tests_properties("host_static_pool_adoption_${pool_adoption}" PROPERTIES
                    LABELS "host;integration;lifecycle;websocket" TIMEOUT 60)
            endforeach()
            add_executable(test-host-external-drain
                tests/test_host_external_drain.cc
                src/host/single_worker_server.cc)
            target_include_directories(test-host-external-drain PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(test-host-external-drain PRIVATE
                capsid_runtime capsid_host_core Boost::system capsid_sanitizers)
            set_target_properties(test-host-external-drain PROPERTIES
                CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-host-external-drain PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-host-external-drain PRIVATE -Wall -Wextra -Wpedantic -Werror)
                    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                        target_compile_options(test-host-external-drain PRIVATE -Wno-maybe-uninitialized)
                    endif()
                endif()
            endif()
            foreach(drain_case IN ITEMS drain-before-submit drain-during-prepare queued-before-drain
                owner-drain-before-submit healthy-control)
                add_test(NAME "host_external_drain_${drain_case}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_external_drain.mjs"
                        --fixture $<TARGET_FILE:test-host-external-drain>
                        --worker $<TARGET_FILE:capsid-worker> --case "${drain_case}"
                        ${CAPSID_POOL_FAULT_CLIENT_ARGS})
                set_tests_properties("host_external_drain_${drain_case}" PROPERTIES
                    LABELS "host;integration;lifecycle;websocket" TIMEOUT 60)
            endforeach()
            add_executable(test-host-unavailable-lifecycle
                tests/test_host_unavailable_lifecycle.cc
                src/host/single_worker_server.cc)
            add_executable(test-host-static-pool-local-shutdown
                tests/test_host_static_pool_local_shutdown.cc
                src/host/single_worker_server.cc
                ${CAPSID_STATIC_POOL_SERVER_TEST_SOURCE})
            foreach(local_target IN ITEMS test-host-unavailable-lifecycle test-host-static-pool-local-shutdown)
                target_include_directories(${local_target} PRIVATE include src "${CAPSID_GENERATED_DIR}")
                target_link_libraries(${local_target} PRIVATE
                    capsid_runtime capsid_host_core Boost::system capsid_sanitizers)
                set_target_properties(${local_target} PROPERTIES
                    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
                if(CAPSID_STRICT_WARNINGS)
                    if(MSVC)
                        target_compile_options(${local_target} PRIVATE /W4 /WX)
                    else()
                        target_compile_options(${local_target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
                        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                            target_compile_options(${local_target} PRIVATE -Wno-maybe-uninitialized)
                        endif()
                    endif()
                endif()
            endforeach()
            foreach(local_case IN ITEMS get-close-before-completion head-close-before-completion get-normal-completion)
                add_test(NAME "host_unavailable_lifecycle_${local_case}"
                    COMMAND $<TARGET_FILE:test-host-unavailable-lifecycle> "${local_case}")
                set_tests_properties("host_unavailable_lifecycle_${local_case}" PROPERTIES
                    LABELS "host;integration;lifecycle" TIMEOUT 20)
            endforeach()
            foreach(local_case IN ITEMS stop drain)
                add_test(NAME "host_static_pool_local_shutdown_${local_case}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_static_pool_local_shutdown.mjs"
                        --fixture $<TARGET_FILE:test-host-static-pool-local-shutdown>
                        --worker $<TARGET_FILE:capsid-worker> --case "${local_case}"
                        ${CAPSID_POOL_FAULT_CLIENT_ARGS})
                set_tests_properties("host_static_pool_local_shutdown_${local_case}" PROPERTIES
                    LABELS "host;integration;lifecycle;websocket" TIMEOUT 45)
            endforeach()
            foreach(exit_case IN ITEMS before-completion before-adoption cancel-pending queued-before-exit
                idle-after-adoption keepalive-after-adoption pre-post-worker-exit pre-post-stop pre-post-drain)
                add_test(NAME "host_external_accept_exit_${exit_case}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_external_accept_exit.mjs"
                        --fixture $<TARGET_FILE:test-host-single-worker-lifecycle>
                        --worker $<TARGET_FILE:capsid-worker> --case "${exit_case}")
                set_tests_properties("host_external_accept_exit_${exit_case}" PROPERTIES
                    LABELS "host;integration;lifecycle" TIMEOUT 45)
            endforeach()
            add_test(NAME websocket_host_cleanup_contract
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ws_host_cleanup.mjs")
            set_tests_properties(websocket_host_cleanup_contract PROPERTIES
                LABELS "host;websocket;lifecycle" TIMEOUT 30)

            # M2 concurrent-wait gate: two threads calling wait() at once
            # must not double-join, and the concurrent caller must block
            # until the joins complete (never return early with a
            # "stopped" claim).
            add_executable(
                test-host-concurrent-wait
                tests/test_host_concurrent_wait.cc
                src/host/single_worker_server.cc
                ${CAPSID_STATIC_POOL_SERVER_TEST_SOURCE})
            target_include_directories(
                test-host-concurrent-wait PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(test-host-concurrent-wait PRIVATE
                capsid_runtime
                capsid_host_core
                Boost::system
                capsid_sanitizers)
            set_target_properties(test-host-concurrent-wait PROPERTIES
                CXX_STANDARD 20
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(
                        test-host-concurrent-wait PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        test-host-concurrent-wait PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                        target_compile_options(
                            test-host-concurrent-wait PRIVATE
                            -Wno-maybe-uninitialized)
                    endif()
                endif()
            endif()
            add_test(
                NAME host_concurrent_shard_wait
                COMMAND test-host-concurrent-wait shard
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_concurrent_shard_wait PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)
            # The pool variant needs a multi-shard pool: SO_REUSEPORT on
            # Linux/macOS, pool-owned shared acceptor on Windows.
            add_test(
                NAME host_concurrent_pool_wait
                COMMAND test-host-concurrent-wait pool
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_concurrent_pool_wait PROPERTIES
                LABELS "host;integration;m2"
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

            find_program(CAPSID_PYTHON3_EXECUTABLE NAMES python3 python REQUIRED)
            add_test(
                NAME benchmark_tool_boundaries
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_tool_boundaries.mjs"
                    --collector
                        "${CMAKE_CURRENT_SOURCE_DIR}/bench/collect-perf-point.py"
                    --qps-lane
                        "${CMAKE_CURRENT_SOURCE_DIR}/bench/qps-lane.sh"
                    --python "${CAPSID_PYTHON3_EXECUTABLE}")
            set_tests_properties(
                benchmark_tool_boundaries
                PROPERTIES
                    LABELS "host;benchmark"
                    TIMEOUT 30)

            add_test(
                NAME benchmark_evidence_contract
                COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_perf_evidence.py"
                    --collector "${CMAKE_CURRENT_SOURCE_DIR}/bench/collect-perf-point.py")
            set_tests_properties(
                benchmark_evidence_contract PROPERTIES
                LABELS "host;benchmark;evidence" TIMEOUT 90)

            add_test(
                NAME benchmark_measurement_runtime
                COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_measurement_runtime.py"
                    --runtime "${CMAKE_CURRENT_SOURCE_DIR}/bench/measurement_runtime.py")
            set_tests_properties(
                benchmark_measurement_runtime PROPERTIES
                LABELS "host;benchmark;lifecycle" TIMEOUT 30)
            add_test(
                NAME benchmark_density_control
                COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_density_control.py"
                    --module "${CMAKE_CURRENT_SOURCE_DIR}/bench/density_control.py"
                    --node "${CAPSID_HOST_TEST_NODE}")
            set_tests_properties(
                benchmark_density_control PROPERTIES
                LABELS "host;benchmark;evidence" TIMEOUT 30)
            add_test(
                NAME benchmark_density_producer
                COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_density_lane.py"
                    --lane "${CMAKE_CURRENT_SOURCE_DIR}/bench/density-lane.py")
            set_tests_properties(
                benchmark_density_producer PROPERTIES
                LABELS "host;benchmark;evidence" TIMEOUT 30)
            add_test(NAME benchmark_qps_control
                COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_qps_control.py"
                    --module "${CMAKE_CURRENT_SOURCE_DIR}/bench/qps_control.py")
            set_tests_properties(benchmark_qps_control PROPERTIES
                LABELS "host;benchmark;measurement" TIMEOUT 30)
            add_test(NAME benchmark_qps_producer
                COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_qps_lane.py"
                    --lane "${CMAKE_CURRENT_SOURCE_DIR}/bench/qps-lane.py")
            set_tests_properties(benchmark_qps_producer PROPERTIES
                LABELS "host;benchmark;evidence" TIMEOUT 60)
            add_test(NAME benchmark_cold_control
                COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_cold_control.py"
                    --module "${CMAKE_CURRENT_SOURCE_DIR}/bench/cold_control.py")
            set_tests_properties(benchmark_cold_control PROPERTIES
                LABELS "host;benchmark;measurement" TIMEOUT 60 RUN_SERIAL TRUE)
            # Real /proc PSS samples include shared-library page fractions.
            # Unrelated CTest processes must not change those denominators.
            # Each test still runs its own concurrent-controller scenarios.
            add_test(NAME benchmark_cold_producer
                COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_cold_lane.py"
                    --lane "${CMAKE_CURRENT_SOURCE_DIR}/bench/cold-start.py")
            set_tests_properties(benchmark_cold_producer PROPERTIES
                LABELS "host;benchmark;measurement;lifecycle;evidence" TIMEOUT 60 RUN_SERIAL TRUE)

            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                # Go is optional for runtime consumers. When available, ALL
                # builds the actual repository loadgen and CTest exercises its
                # HTTP correctness, including timeout and warm-up failures.
                find_program(CAPSID_GO_EXECUTABLE NAMES go)
                if(CAPSID_GO_EXECUTABLE)
                    set(CAPSID_BENCH_LOADGEN "${CMAKE_CURRENT_BINARY_DIR}/benchmark-tools/loadgen")
                    add_custom_command(OUTPUT "${CAPSID_BENCH_LOADGEN}"
                        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/benchmark-tools"
                        COMMAND "${CMAKE_COMMAND}" -E env "GOWORK=off" "GOPROXY=off" "GOFLAGS=" "GOTOOLCHAIN=local"
                            "${CAPSID_GO_EXECUTABLE}" build -trimpath -buildvcs=false
                            -o "${CAPSID_BENCH_LOADGEN}" .
                        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bench/loadgen"
                        DEPENDS bench/loadgen/main.go bench/loadgen/go.mod
                        COMMENT "Building the repository benchmark load generator" VERBATIM)
                    add_custom_target(benchmark-loadgen ALL DEPENDS "${CAPSID_BENCH_LOADGEN}")
                    add_test(NAME benchmark_loadgen_correctness
                        COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_loadgen.py"
                            --loadgen "${CAPSID_BENCH_LOADGEN}")
                    set_tests_properties(benchmark_loadgen_correctness PROPERTIES
                        LABELS "host;benchmark;correctness" TIMEOUT 90)
                    add_test(NAME benchmark_qps_probe
                        COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_qps_probe.py"
                            --loadgen "${CAPSID_BENCH_LOADGEN}"
                            --probe "${CMAKE_CURRENT_SOURCE_DIR}/bench/qps_probe.py")
                    set_tests_properties(benchmark_qps_probe PROPERTIES
                        LABELS "host;benchmark;measurement;lifecycle" TIMEOUT 90 RUN_SERIAL TRUE)
                endif()
                if(TARGET benchmark-cold-start)
                    # Actual /proc mapping controls show Clang ASan's default
                    # FakeStack pages settle after the one-second window.
                    # Keep that instrumentation and the strict PSS predicate;
                    # wait before sampling, outside the cold timing region.
                    set(CAPSID_COLD_TEST_SETTLE_ARGS)
                    if(CAPSID_ENABLE_ASAN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                        list(APPEND CAPSID_COLD_TEST_SETTLE_ARGS --settle-ms 20000)
                    endif()
                    add_test(NAME benchmark_cold_tool
                        COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_cold_tool.py"
                            --tool "$<TARGET_FILE:benchmark-cold-start>"
                            --worker "$<TARGET_FILE:capsid-worker>"
                            ${CAPSID_COLD_TEST_SETTLE_ARGS})
                    set_tests_properties(benchmark_cold_tool PROPERTIES
                        LABELS "host;benchmark;measurement;lifecycle" TIMEOUT 100 RUN_SERIAL TRUE)
                    add_test(NAME benchmark_cold_descendants
                        COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_cold_descendants.py"
                            --tool "$<TARGET_FILE:benchmark-cold-start>"
                            --worker "$<TARGET_FILE:capsid-worker>"
                            --descendant-helper "$<TARGET_FILE:test-cold-worker-descendant>"
                            ${CAPSID_COLD_TEST_SETTLE_ARGS}
                            DescendantTests.test_cancel_reaps_both_kinds_of_real_descendant
                            DescendantTests.test_wrong_response_reaps_descendant_without_sample
                            DescendantTests.test_timeout_reaps_descendant_without_sample)
                    set_tests_properties(benchmark_cold_descendants PROPERTIES
                        LABELS "host;benchmark;measurement;lifecycle" TIMEOUT 30 RUN_SERIAL TRUE)
                    foreach(CAPSID_COLD_DESCENDANT_MODE fork clone)
                        add_test(NAME "benchmark_cold_descendant_sample_${CAPSID_COLD_DESCENDANT_MODE}"
                            COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_cold_descendants.py"
                                --tool "$<TARGET_FILE:benchmark-cold-start>"
                                --worker "$<TARGET_FILE:capsid-worker>"
                                --descendant-helper "$<TARGET_FILE:test-cold-worker-descendant>"
                                --descendant-mode "${CAPSID_COLD_DESCENDANT_MODE}"
                                ${CAPSID_COLD_TEST_SETTLE_ARGS}
                                DescendantTests.test_normal_sample_includes_descendant_and_reaps_it_before_output)
                        set_tests_properties("benchmark_cold_descendant_sample_${CAPSID_COLD_DESCENDANT_MODE}" PROPERTIES
                            LABELS "host;benchmark;measurement;lifecycle" TIMEOUT 30 RUN_SERIAL TRUE)
                    endforeach()
                endif()
                add_test(
                    NAME benchmark_fake_worker_lifecycle
                    COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_fake_worker_lifecycle.mjs")
                set_tests_properties(
                    benchmark_fake_worker_lifecycle PROPERTIES
                    LABELS "host;benchmark;lifecycle" TIMEOUT 10)
            endif()

            add_test(
                NAME workerd_ws_fixtures
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_ws_fixtures.mjs"
                    --host $<TARGET_FILE:capsid-host>
                    --worker $<TARGET_FILE:capsid-worker>
                    --mode single-worker)
            set_tests_properties(
                workerd_ws_fixtures
                PROPERTIES
                    LABELS "host"
                    TIMEOUT 240)
            foreach(CAPSID_WS_MODE IN LISTS CAPSID_WS_SECONDARY_MODES)
                add_test(NAME "workerd_ws_fixtures_${CAPSID_WS_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_ws_fixtures.mjs"
                        --host $<TARGET_FILE:capsid-host>
                        --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_WS_MODE}")
                set_tests_properties("workerd_ws_fixtures_${CAPSID_WS_MODE}" PROPERTIES TIMEOUT 240 LABELS "host;websocket")
            endforeach()
            if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
                foreach(CAPSID_MESSAGE_PORT_MODE IN LISTS CAPSID_WS_BACKEND_MODES)
                    add_test(NAME "native_message_port_order_${CAPSID_MESSAGE_PORT_MODE}"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_message_port_order.mjs"
                            --host $<TARGET_FILE:capsid-host>
                            --worker $<TARGET_FILE:capsid-worker>
                            --mode "${CAPSID_MESSAGE_PORT_MODE}")
                    set_tests_properties("native_message_port_order_${CAPSID_MESSAGE_PORT_MODE}" PROPERTIES
                        TIMEOUT 60 LABELS "host;events;messagechannel;reference-gate")
                endforeach()
            endif()
            add_test(NAME workerd_ws_mutation_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_ws_mutations.mjs"
                    --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>)
            set_tests_properties(workerd_ws_mutation_controls PROPERTIES
                TIMEOUT 120 LABELS "host;websocket;reference-gate")
            add_test(NAME workerd_throw_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_throw_contract.mjs")
            set_tests_properties(workerd_throw_contract_controls PROPERTIES
                TIMEOUT 10 LABELS "host;websocket;reference-gate")
            add_test(NAME workerd_handler_error_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_handler_error_contract.mjs")
            set_tests_properties(workerd_handler_error_contract_controls PROPERTIES
                TIMEOUT 10 LABELS "host;websocket;reference-gate")
            add_test(NAME workerd_owner_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_owner_contract.mjs")
            set_tests_properties(workerd_owner_contract_controls PROPERTIES
                TIMEOUT 10 LABELS "host;websocket;lifecycle;reference-gate")
            add_test(NAME workerd_async_callback_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_async_callback_contract.mjs")
            set_tests_properties(workerd_async_callback_contract_controls PROPERTIES
                TIMEOUT 10 LABELS "host;websocket;lifecycle;reference-gate")
            add_test(NAME workerd_cross_context_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_cross_context_contract.mjs")
            set_tests_properties(workerd_cross_context_contract_controls PROPERTIES
                TIMEOUT 10 LABELS "host;websocket;lifecycle;reference-gate")
            add_test(NAME workerd_fetch_resource_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_fetch_resource_contract.mjs")
            set_tests_properties(workerd_fetch_resource_contract_controls PROPERTIES
                TIMEOUT 10 LABELS "host;websocket;lifecycle;reference-gate")
            add_test(NAME workerd_ordinary_fetch_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_ordinary_fetch_contract.mjs")
            set_tests_properties(workerd_ordinary_fetch_contract_controls PROPERTIES
                TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
            add_test(NAME workerd_http_codec_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_http_codec_contract.mjs")
            set_tests_properties(workerd_http_codec_contract_controls PROPERTIES
                TIMEOUT 120 LABELS "host;websocket;fetch;compression;reference-gate")
            add_test(NAME workerd_fetch_upgrade_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_fetch_upgrade_contract.mjs")
            set_tests_properties(workerd_fetch_upgrade_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "host;websocket;fetch;reference-gate")
            add_test(NAME workerd_fetch_upgrade_lifetime_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_fetch_upgrade_lifetime_contract.mjs")
            set_tests_properties(workerd_fetch_upgrade_lifetime_contract_controls PROPERTIES
                TIMEOUT 15 LABELS "host;websocket;lifecycle;reference-gate")
            add_test(NAME workerd_fetch_proxy_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_fetch_proxy_contract.mjs")
            set_tests_properties(workerd_fetch_proxy_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "host;websocket;fetch;reference-gate")
            find_program(CAPSID_TEST_OPENSSL_EXECUTABLE NAMES openssl)
            add_test(NAME workerd_fetch_boundary_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_fetch_boundary_contract.mjs")
            set_tests_properties(workerd_fetch_boundary_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "host;websocket;fetch;reference-gate")
            add_test(NAME workerd_fetch_transition_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_fetch_transition_contract.mjs")
            set_tests_properties(workerd_fetch_transition_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "host;websocket;fetch;reference-gate")
            add_test(NAME workerd_fetch_byob_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_fetch_byob_contract.mjs")
            set_tests_properties(workerd_fetch_byob_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "host;websocket;fetch;reference-gate")
            add_test(NAME workerd_fetch_arguments_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_fetch_arguments_contract.mjs")
            set_tests_properties(workerd_fetch_arguments_contract_controls PROPERTIES
                TIMEOUT 60 LABELS "host;websocket;fetch;reference-gate")
            add_test(NAME workerd_fetch_reader_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_fetch_reader_contract.mjs")
            set_tests_properties(workerd_fetch_reader_contract_controls PROPERTIES
                TIMEOUT 60 LABELS "host;websocket;fetch;reference-gate")
            add_test(NAME workerd_reader_intrinsic_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_reader_intrinsic_contract.mjs")
            set_tests_properties(workerd_reader_intrinsic_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "host;websocket;streams;reference-gate")
            add_test(NAME workerd_native_reader_lifecycle_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_native_reader_lifecycle_contract.mjs")
            set_tests_properties(workerd_native_reader_lifecycle_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "host;websocket;fetch;lifecycle;reference-gate")
            if(UNIX)
                add_test(NAME websocket_host_backend_startup_diagnostics
                    COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ws_host_backend_diagnostics.mjs")
                set_tests_properties(websocket_host_backend_startup_diagnostics PROPERTIES
                    TIMEOUT 30 LABELS "host;websocket;lifecycle;test-control")
            endif()
            add_test(NAME workerd_reader_scope_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_reader_scope_contract.mjs")
            set_tests_properties(workerd_reader_scope_contract_controls PROPERTIES
                TIMEOUT 60 LABELS "host;websocket;fetch;lifecycle;reference-gate")
            add_test(NAME workerd_standard_reader_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_standard_reader_contract.mjs")
            set_tests_properties(workerd_standard_reader_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "host;websocket;fetch;lifecycle;reference-gate")
            add_test(NAME web_streams_source_contract
                COMMAND "${CAPSID_HOST_TEST_NODE}" --experimental-vm-modules --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_web_streams_source.mjs")
            set_tests_properties(web_streams_source_contract PROPERTIES
                TIMEOUT 10 LABELS "worker;streams;compatibility")
            add_test(NAME workerd_crypto_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_crypto_contract.mjs")
            set_tests_properties(workerd_crypto_contract_controls PROPERTIES
                TIMEOUT 10 LABELS "host;websocket;crypto;reference-gate")
            foreach(CAPSID_OUTBOUND_MODE IN LISTS CAPSID_WS_BACKEND_MODES)
                add_test(NAME "websocket_crypto_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_crypto.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}")
                set_tests_properties("websocket_crypto_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;crypto;reference-gate")
                add_test(NAME "websocket_cross_context_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_cross_context.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}")
                set_tests_properties("websocket_cross_context_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;lifecycle;reference-gate")
                add_test(NAME "websocket_fetch_resources_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_resources.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}")
                set_tests_properties("websocket_fetch_resources_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;lifecycle;reference-gate")
                # Five mandatory shards retain every case under the existing
                # per-test 120 second limit: 600 seconds total per backend.
                foreach(CAPSID_ORDINARY_FETCH_SHARD core pipelines gated views bodies)
                    add_test(NAME "websocket_ordinary_fetch_${CAPSID_ORDINARY_FETCH_SHARD}_${CAPSID_OUTBOUND_MODE}"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_ordinary_fetch.mjs"
                            --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                            --mode "${CAPSID_OUTBOUND_MODE}" --shard "${CAPSID_ORDINARY_FETCH_SHARD}")
                    set_tests_properties("websocket_ordinary_fetch_${CAPSID_ORDINARY_FETCH_SHARD}_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                        TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                endforeach()
                # Additional compression coverage has two mandatory shards;
                # the existing ordinary-fetch shards and budgets are unchanged.
                foreach(CAPSID_HTTP_CODEC_SHARD decode metadata)
                    add_test(NAME "websocket_http_codec_${CAPSID_HTTP_CODEC_SHARD}_${CAPSID_OUTBOUND_MODE}"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_http_codec.mjs"
                            --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                            --mode "${CAPSID_OUTBOUND_MODE}" --shard "${CAPSID_HTTP_CODEC_SHARD}")
                    set_tests_properties("websocket_http_codec_${CAPSID_HTTP_CODEC_SHARD}_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                        TIMEOUT 120 LABELS "host;websocket;fetch;compression;reference-gate")
                endforeach()
                add_test(NAME "websocket_fetch_upgrade_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_upgrade.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}")
                set_tests_properties("websocket_fetch_upgrade_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                add_test(NAME "websocket_fetch_upgrade_lifetime_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_upgrade_lifetime.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}")
                set_tests_properties("websocket_fetch_upgrade_lifetime_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;lifecycle;reference-gate")
                add_test(NAME "websocket_fetch_proxy_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_proxy.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}")
                set_tests_properties("websocket_fetch_proxy_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 180 LABELS "host;websocket;fetch;reference-gate")
                add_test(NAME "websocket_fetch_boundaries_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_boundaries.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}" --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                set_tests_properties("websocket_fetch_boundaries_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                add_test(NAME "websocket_fetch_transitions_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_transitions.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}" --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                set_tests_properties("websocket_fetch_transitions_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                foreach(CAPSID_BYOB_SHARD readers constructors reflect-standard views-brands bodies standard-edges standard-close)
                    add_test(NAME "websocket_fetch_byob_${CAPSID_BYOB_SHARD}_${CAPSID_OUTBOUND_MODE}"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_byob.mjs"
                            --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                            --mode "${CAPSID_OUTBOUND_MODE}" --shard "${CAPSID_BYOB_SHARD}"
                            --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                    set_tests_properties("websocket_fetch_byob_${CAPSID_BYOB_SHARD}_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                        TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                endforeach()
                foreach(CAPSID_ARGUMENT_ROUTE getReader direct prototype subclass reflect)
                    add_test(NAME "websocket_fetch_arguments_${CAPSID_ARGUMENT_ROUTE}_${CAPSID_OUTBOUND_MODE}"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_arguments.mjs"
                            --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                            --mode "${CAPSID_OUTBOUND_MODE}" --shard "${CAPSID_ARGUMENT_ROUTE}"
                            --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                    set_tests_properties("websocket_fetch_arguments_${CAPSID_ARGUMENT_ROUTE}_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                        TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                endforeach()
                add_test(NAME "websocket_fetch_readers_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_readers.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}" --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                set_tests_properties("websocket_fetch_readers_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                add_test(NAME "websocket_async_callbacks_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_async_callbacks.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}")
                set_tests_properties("websocket_async_callbacks_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;lifecycle;reference-gate")
                add_test(NAME "websocket_reader_intrinsics_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_reader_intrinsics.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}")
                set_tests_properties("websocket_reader_intrinsics_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 60 LABELS "host;websocket;streams;reference-gate")
                add_test(NAME "websocket_native_reader_lifecycle_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_native_reader_lifecycle.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}" --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                set_tests_properties("websocket_native_reader_lifecycle_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;lifecycle;reference-gate")
                add_test(NAME "websocket_reader_scope_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_reader_scope.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}" --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                set_tests_properties("websocket_reader_scope_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;lifecycle;reference-gate")
                foreach(CAPSID_STANDARD_READER_SHARD IN ITEMS shapes ownership transfers)
                    add_test(NAME "websocket_standard_readers_${CAPSID_STANDARD_READER_SHARD}_${CAPSID_OUTBOUND_MODE}"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_standard_readers.mjs"
                            --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                            --shard "${CAPSID_STANDARD_READER_SHARD}" --mode "${CAPSID_OUTBOUND_MODE}" --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                    set_tests_properties("websocket_standard_readers_${CAPSID_STANDARD_READER_SHARD}_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                        TIMEOUT 120 LABELS "host;websocket;fetch;lifecycle;reference-gate")
                endforeach()
                add_test(NAME "websocket_async_budget_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_async_callbacks.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}" --budgets 1)
                set_tests_properties("websocket_async_budget_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;lifecycle;budget;reference-gate")
                add_test(NAME "websocket_owner_lifecycle_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_owner_lifecycle.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}")
                set_tests_properties("websocket_owner_lifecycle_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;lifecycle;reference-gate")
                add_test(NAME "websocket_throw_wire_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_throw_wire.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}")
                set_tests_properties("websocket_throw_wire_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    LABELS "host;websocket;framing;reference-gate" TIMEOUT 45)
                add_test(NAME "websocket_frame_boundaries_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_frame_boundaries.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --mode "${CAPSID_OUTBOUND_MODE}")
                set_tests_properties("websocket_frame_boundaries_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                    LABELS "host;websocket;framing" TIMEOUT 120)
                if(NOT CAPSID_OUTBOUND_MODE STREQUAL "managed" OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
                    add_test(NAME "host_session_tracking_${CAPSID_OUTBOUND_MODE}"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_session_tracking.mjs"
                            --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                            --mode "${CAPSID_OUTBOUND_MODE}")
                    set_tests_properties("host_session_tracking_${CAPSID_OUTBOUND_MODE}" PROPERTIES
                        TIMEOUT 45 LABELS "host;http;lifecycle")
                endif()
                add_test(NAME "websocket_outbound_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_outbound.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker> --mode "${CAPSID_OUTBOUND_MODE}" --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                set_tests_properties("websocket_outbound_${CAPSID_OUTBOUND_MODE}" PROPERTIES TIMEOUT 120 LABELS "host;websocket")
                add_test(NAME "websocket_budget_${CAPSID_OUTBOUND_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_budget.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker> --mode "${CAPSID_OUTBOUND_MODE}")
                set_tests_properties("websocket_budget_${CAPSID_OUTBOUND_MODE}" PROPERTIES TIMEOUT 120 LABELS "host;websocket;capacity;metrics")
            endforeach()
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                set(CAPSID_MANAGED_SOAK_MEMORY_MODE production)
                if(CAPSID_ENABLE_ASAN OR CAPSID_ENABLE_UBSAN OR CAPSID_ENABLE_TSAN)
                    set(CAPSID_MANAGED_SOAK_MEMORY_MODE sanitizer)
                endif()
                add_test(NAME websocket_managed_soak_controls
                    COMMAND "${CAPSID_HOST_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_managed_websocket_soak.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                        --memory-mode "${CAPSID_MANAGED_SOAK_MEMORY_MODE}")
                set_tests_properties(websocket_managed_soak_controls PROPERTIES TIMEOUT 90 LABELS "host;websocket;managed;lifecycle;soak")
                add_test(NAME websocket_managed_local_owner
                    COMMAND "${CAPSID_HOST_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_managed_local_owner.mjs"
                        --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker> --mode managed)
                set_tests_properties(websocket_managed_local_owner PROPERTIES TIMEOUT 45 LABELS "host;websocket;managed;lifecycle")
                foreach(CAPSID_MANAGED_WS_CASE IN ITEMS deploy retire-deadline replacement replacement-recursion deploy-persist-failure reclamation-idle)
                    add_test(NAME "websocket_managed_${CAPSID_MANAGED_WS_CASE}"
                        COMMAND "${CAPSID_HOST_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_managed_lifecycle.mjs"
                            --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker> --case "${CAPSID_MANAGED_WS_CASE}")
                    set_tests_properties("websocket_managed_${CAPSID_MANAGED_WS_CASE}" PROPERTIES TIMEOUT 120 LABELS "host;websocket;managed;lifecycle")
                endforeach()
                if(CAPSID_ENABLE_ASAN)
                    foreach(CAPSID_ASAN_RESOURCE_CASE IN ITEMS replacement-recursion reclamation-idle)
                        add_test(NAME "websocket_managed_asan_resources_${CAPSID_ASAN_RESOURCE_CASE}"
                            COMMAND "${CAPSID_HOST_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_managed_lifecycle.mjs"
                                --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                                --case "${CAPSID_ASAN_RESOURCE_CASE}" --memory-acceptance asan-resources
                                --report "${CMAKE_CURRENT_BINARY_DIR}/asan-memory-${CAPSID_ASAN_RESOURCE_CASE}.json")
                        set_tests_properties("websocket_managed_asan_resources_${CAPSID_ASAN_RESOURCE_CASE}" PROPERTIES
                            TIMEOUT 120 LABELS "host;websocket;managed;lifecycle;asan-resources"
                            ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1;LSAN_OPTIONS=")
                    endforeach()
                endif()
                if(CAPSID_ENABLE_TSAN)
                    foreach(CAPSID_TSAN_RESOURCE_CASE IN ITEMS replacement-recursion reclamation-idle)
                        add_test(NAME "websocket_managed_tsan_resources_${CAPSID_TSAN_RESOURCE_CASE}"
                            COMMAND "${CAPSID_HOST_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_managed_lifecycle.mjs"
                                --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                                --case "${CAPSID_TSAN_RESOURCE_CASE}" --memory-acceptance tsan-resources
                                --report "${CMAKE_CURRENT_BINARY_DIR}/tsan-memory-${CAPSID_TSAN_RESOURCE_CASE}.json")
                        set_tests_properties("websocket_managed_tsan_resources_${CAPSID_TSAN_RESOURCE_CASE}" PROPERTIES
                            TIMEOUT 120 LABELS "host;websocket;managed;lifecycle;tsan-resources"
                            ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:exitcode=66")
                    endforeach()
                endif()
            endif()
            # Always registered (review §7: conditional registration
            # silently vanishes the gate when configure runs without
            # the env); the runner itself skips at TEST time when
            # CAPSID_WORKERD_REF is unset.
            add_test(NAME websocket_outbound_reference
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_outbound.mjs"
                    --reference-from-env --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
            set_tests_properties(websocket_outbound_reference PROPERTIES
                TIMEOUT 120 LABELS "host;websocket;reference"
                SKIP_RETURN_CODE 77)
            # locked workerd binary with Node's native WebSocket client.
            # Runs only when CAPSID_WORKERD_REF is set at TEST time and
            # points at the binary whose SHA-256 matches
            # tests/fixtures/workerd-ws/reference-lock.json; otherwise
            # skips (77) so the reference gate never silently vanishes.
            add_test(
                NAME workerd_ws_reference
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_ws_fixtures.mjs"
                    --reference-from-env)
            set_tests_properties(
                workerd_ws_reference PROPERTIES
                LABELS "host;workerd-reference"
                TIMEOUT 180
                ENVIRONMENT "CAPSID_WDREF_UNSET_GUARD=1"
                SKIP_RETURN_CODE 77)
            set(CAPSID_WS_INTEROP_PYTHON_ARGS --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
            if(CAPSID_WS_INTEROP_PYTHON)
                list(APPEND CAPSID_WS_INTEROP_PYTHON_ARGS --python "${CAPSID_WS_INTEROP_PYTHON}")
            endif()
            if(CAPSID_WS_INTEROP_NODE_PACKAGE)
                list(APPEND CAPSID_WS_INTEROP_PYTHON_ARGS --node-package "${CAPSID_WS_INTEROP_NODE_PACKAGE}")
            endif()
            foreach(CAPSID_PYTHON_PEER_CASE idle exchange tls-reset reset-session close-reset)
                add_test(NAME "websocket_python_peer_${CAPSID_PYTHON_PEER_CASE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_python_peer_lifecycle.mjs"
                        --case "${CAPSID_PYTHON_PEER_CASE}" ${CAPSID_WS_INTEROP_PYTHON_ARGS})
                set_tests_properties("websocket_python_peer_${CAPSID_PYTHON_PEER_CASE}" PROPERTIES
                    TIMEOUT 45 LABELS "host;websocket;independent-client;peer-lifecycle")
            endforeach()
            set(CAPSID_WS_COMPAT_MODES ${CAPSID_WS_BACKEND_MODES})
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
                list(PREPEND CAPSID_WS_COMPAT_MODES reference)
                add_test(NAME websocket_async_callbacks_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_async_callbacks.mjs"
                        --reference-from-env 1)
                set_tests_properties(websocket_async_callbacks_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;lifecycle;reference-gate")
                add_test(NAME websocket_cross_context_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_cross_context.mjs"
                        --reference-from-env 1)
                set_tests_properties(websocket_cross_context_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;lifecycle;reference-gate")
                add_test(NAME websocket_fetch_resources_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_resources.mjs"
                        --reference-from-env 1)
                set_tests_properties(websocket_fetch_resources_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;lifecycle;reference-gate")
                foreach(CAPSID_ORDINARY_FETCH_SHARD core pipelines gated views bodies)
                    add_test(NAME "websocket_ordinary_fetch_${CAPSID_ORDINARY_FETCH_SHARD}_reference"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_ordinary_fetch.mjs"
                            --reference-from-env 1 --shard "${CAPSID_ORDINARY_FETCH_SHARD}")
                    set_tests_properties("websocket_ordinary_fetch_${CAPSID_ORDINARY_FETCH_SHARD}_reference" PROPERTIES
                        TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                endforeach()
                foreach(CAPSID_HTTP_CODEC_SHARD decode metadata)
                    add_test(NAME "websocket_http_codec_${CAPSID_HTTP_CODEC_SHARD}_reference"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_http_codec.mjs"
                            --reference-from-env 1 --shard "${CAPSID_HTTP_CODEC_SHARD}")
                    set_tests_properties("websocket_http_codec_${CAPSID_HTTP_CODEC_SHARD}_reference" PROPERTIES
                        TIMEOUT 120 LABELS "host;websocket;fetch;compression;reference-gate")
                endforeach()
                add_test(NAME websocket_fetch_upgrade_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_upgrade.mjs"
                        --reference-from-env 1)
                set_tests_properties(websocket_fetch_upgrade_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                add_test(NAME websocket_fetch_upgrade_lifetime_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_upgrade_lifetime.mjs"
                        --reference-from-env 1)
                set_tests_properties(websocket_fetch_upgrade_lifetime_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;lifecycle;reference-gate")
                add_test(NAME websocket_fetch_proxy_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_proxy.mjs"
                        --reference-from-env 1)
                set_tests_properties(websocket_fetch_proxy_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                add_test(NAME websocket_fetch_boundaries_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_boundaries.mjs"
                        --reference-from-env 1 --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                set_tests_properties(websocket_fetch_boundaries_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                add_test(NAME websocket_fetch_transitions_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_transitions.mjs"
                        --reference-from-env 1 --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                set_tests_properties(websocket_fetch_transitions_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                foreach(CAPSID_BYOB_SHARD readers constructors reflect-standard views-brands bodies standard-edges standard-close)
                    add_test(NAME "websocket_fetch_byob_${CAPSID_BYOB_SHARD}_reference"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_byob.mjs"
                            --reference-from-env 1 --shard "${CAPSID_BYOB_SHARD}"
                            --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                    set_tests_properties("websocket_fetch_byob_${CAPSID_BYOB_SHARD}_reference" PROPERTIES
                        TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                endforeach()
                foreach(CAPSID_ARGUMENT_ROUTE getReader direct prototype subclass reflect)
                    add_test(NAME "websocket_fetch_arguments_${CAPSID_ARGUMENT_ROUTE}_reference"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_arguments.mjs"
                            --reference-from-env 1 --shard "${CAPSID_ARGUMENT_ROUTE}"
                            --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                    set_tests_properties("websocket_fetch_arguments_${CAPSID_ARGUMENT_ROUTE}_reference" PROPERTIES
                        TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                endforeach()
                add_test(NAME websocket_fetch_readers_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_fetch_readers.mjs"
                        --reference-from-env 1 --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                set_tests_properties(websocket_fetch_readers_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;reference-gate")
                add_test(NAME websocket_reader_intrinsics_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_reader_intrinsics.mjs"
                        --reference-from-env 1)
                set_tests_properties(websocket_reader_intrinsics_reference PROPERTIES
                    TIMEOUT 60 LABELS "host;websocket;streams;reference-gate")
                add_test(NAME websocket_native_reader_lifecycle_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_native_reader_lifecycle.mjs"
                        --reference-from-env 1 --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                set_tests_properties(websocket_native_reader_lifecycle_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;lifecycle;reference-gate")
                add_test(NAME websocket_reader_scope_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_reader_scope.mjs"
                        --reference-from-env 1 --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                set_tests_properties(websocket_reader_scope_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;fetch;lifecycle;reference-gate")
                foreach(CAPSID_STANDARD_READER_SHARD IN ITEMS shapes ownership transfers)
                    add_test(NAME "websocket_standard_readers_${CAPSID_STANDARD_READER_SHARD}_reference"
                        COMMAND "${CAPSID_HOST_TEST_NODE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_standard_readers.mjs"
                            --shard "${CAPSID_STANDARD_READER_SHARD}" --reference-from-env 1 --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}")
                    set_tests_properties("websocket_standard_readers_${CAPSID_STANDARD_READER_SHARD}_reference" PROPERTIES
                        TIMEOUT 120 LABELS "host;websocket;fetch;lifecycle;reference-gate")
                endforeach()
                add_test(NAME websocket_crypto_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_crypto.mjs"
                        --reference-from-env 1)
                set_tests_properties(websocket_crypto_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;crypto;reference-gate")
                add_test(NAME websocket_owner_lifecycle_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_owner_lifecycle.mjs"
                        --reference-from-env 1)
                set_tests_properties(websocket_owner_lifecycle_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;lifecycle;reference-gate")
                add_test(NAME websocket_frame_boundaries_reference
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_frame_boundaries.mjs"
                        --reference-from-env 1)
                set_tests_properties(websocket_frame_boundaries_reference PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;framing;reference-gate")
            endif()
            add_test(NAME websocket_frame_contract_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_frame_contract.mjs")
            set_tests_properties(websocket_frame_contract_controls PROPERTIES
                TIMEOUT 30 LABELS "host;websocket;framing;contract")
            foreach(CAPSID_INTEROP_MODE IN LISTS CAPSID_WS_COMPAT_MODES)
                if(CAPSID_INTEROP_MODE STREQUAL "reference")
                    set(CAPSID_INTEROP_BACKEND_ARGS --reference-from-env 1)
                else()
                    set(CAPSID_INTEROP_BACKEND_ARGS --host $<TARGET_FILE:capsid-host>
                        --worker $<TARGET_FILE:capsid-worker> --mode "${CAPSID_INTEROP_MODE}")
                endif()
                add_test(NAME "websocket_interop_${CAPSID_INTEROP_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_interop.mjs"
                        ${CAPSID_INTEROP_BACKEND_ARGS} ${CAPSID_WS_INTEROP_PYTHON_ARGS})
                set_tests_properties("websocket_interop_${CAPSID_INTEROP_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;independent-client;reference-gate")
            endforeach()
            set(CAPSID_FANOUT_NODE_ARGS)
            if(CAPSID_WS_INTEROP_NODE_PACKAGE)
                list(APPEND CAPSID_FANOUT_NODE_ARGS --node-package "${CAPSID_WS_INTEROP_NODE_PACKAGE}")
            endif()
            foreach(CAPSID_FANOUT_MODE IN LISTS CAPSID_WS_COMPAT_MODES)
                if(CAPSID_FANOUT_MODE STREQUAL "reference")
                    set(CAPSID_FANOUT_BACKEND_ARGS --reference-from-env 1)
                else()
                    set(CAPSID_FANOUT_BACKEND_ARGS --host $<TARGET_FILE:capsid-host>
                        --worker $<TARGET_FILE:capsid-worker> --mode "${CAPSID_FANOUT_MODE}")
                endif()
                add_test(NAME "workerd_fanout_${CAPSID_FANOUT_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_fanout.mjs"
                        ${CAPSID_FANOUT_BACKEND_ARGS} ${CAPSID_FANOUT_NODE_ARGS})
                set_tests_properties("workerd_fanout_${CAPSID_FANOUT_MODE}" PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;multiworker;reference-gate")
            endforeach()
            set(CAPSID_ENCODING_MODES single-worker static-pool)
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                list(APPEND CAPSID_ENCODING_MODES managed)
            endif()
            foreach(CAPSID_ENCODING_MODE IN LISTS CAPSID_ENCODING_MODES)
                add_test(NAME "gb18030_compatibility_${CAPSID_ENCODING_MODE}"
                    COMMAND "${CAPSID_HOST_TEST_NODE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gb18030_compatibility.mjs"
                        $<TARGET_FILE:capsid-host> $<TARGET_FILE:capsid-worker>
                        "${CAPSID_ENCODING_MODE}")
                set_tests_properties("gb18030_compatibility_${CAPSID_ENCODING_MODE}" PROPERTIES
                    TIMEOUT 60 LABELS "host;encoding;compatibility")
            endforeach()
            add_test(NAME websocket_platform_matrix
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_platform_matrix.mjs"
                    --ctest "${CMAKE_CTEST_COMMAND}" --build-dir "${CMAKE_BINARY_DIR}"
                    --system "${CMAKE_SYSTEM_NAME}")
            set_tests_properties(websocket_platform_matrix PROPERTIES
                TIMEOUT 40 LABELS "host;websocket;platform;registration")
            add_test(NAME websocket_platform_matrix_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_platform_matrix_contract.mjs")
            set_tests_properties(websocket_platform_matrix_controls PROPERTIES
                TIMEOUT 60 LABELS "host;websocket;platform;registration"
                ENVIRONMENT "CAPSID_PLATFORM_TEST_CTEST=${CMAKE_CTEST_COMMAND}")
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
                add_test(NAME workerd_ws_gate_controls
                    COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_reference_gate.mjs")
                add_test(NAME workerd_ws_gate_archive_controls
                    COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_reference_gate.mjs")
                add_test(NAME workerd_ws_gate_extension_controls
                    COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_reference_gate.mjs")
                add_test(NAME workerd_ws_gate_support_controls
                    COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_outbound_contract.mjs"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_response_contract.mjs"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_websocket_interop_contract.mjs"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_fanout_contract.mjs"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_frame_contract.mjs")
                set(CAPSID_WORKERD_CONTROL_TESTS workerd_ws_gate_controls
                    workerd_ws_gate_archive_controls workerd_ws_gate_extension_controls workerd_ws_gate_support_controls)
                set_tests_properties(${CAPSID_WORKERD_CONTROL_TESTS} PROPERTIES
                    TIMEOUT 120 LABELS "host;websocket;reference-gate"
                    ENVIRONMENT "CAPSID_FANOUT_TEST_HOST=$<TARGET_FILE:capsid-host>;CAPSID_FANOUT_TEST_WORKER=$<TARGET_FILE:capsid-worker>")
                if(CAPSID_WS_INTEROP_PYTHON AND CAPSID_WS_INTEROP_NODE_PACKAGE)
                    set_tests_properties(${CAPSID_WORKERD_CONTROL_TESTS} PROPERTIES
                        ENVIRONMENT "CAPSID_WS_INTEROP_PYTHON=${CAPSID_WS_INTEROP_PYTHON};CAPSID_WS_INTEROP_NODE_PACKAGE=${CAPSID_WS_INTEROP_NODE_PACKAGE};CAPSID_FANOUT_TEST_HOST=$<TARGET_FILE:capsid-host>;CAPSID_FANOUT_TEST_WORKER=$<TARGET_FILE:capsid-worker>")
                endif()
                set_property(TEST workerd_ws_gate_controls APPEND PROPERTY ENVIRONMENT "CAPSID_WS_GATE_CONTROL_SHARD=primary")
                set_property(TEST workerd_ws_gate_archive_controls APPEND PROPERTY ENVIRONMENT "CAPSID_WS_GATE_CONTROL_SHARD=archives")
                set_property(TEST workerd_ws_gate_extension_controls APPEND PROPERTY ENVIRONMENT "CAPSID_WS_GATE_CONTROL_SHARD=extensions")
            endif()
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

        # WP-07, spec §11.4: a caller compiled against the build-info v1
        # headers (frozen fixture) links against the current library and
        # negotiates with the smaller struct_size. This is RED while the
        # library demands struct_size >= sizeof(current struct).
        add_executable(test-build-info-v1-link
            tests/test_build_info_v1_link.c)
        target_include_directories(test-build-info-v1-link PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_link_libraries(test-build-info-v1-link PRIVATE
            capsid_runtime
            capsid_sanitizers)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(test-build-info-v1-link PRIVATE
                    /W4 /WX)
            else()
                target_compile_options(test-build-info-v1-link PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(
            NAME runtime_build_info_v1_negotiation
            COMMAND test-build-info-v1-link)

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

        if(CAPSID_ENABLE_ASAN OR CAPSID_ENABLE_UBSAN OR CAPSID_ENABLE_TSAN)
            find_program(CAPSID_HOST_TEST_NODE NAMES node REQUIRED)
            add_test(
                NAME host_sanitizer_instrumentation
                COMMAND "${CMAKE_COMMAND}"
                    "-DCAPSID_COMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json"
                    "-DCAPSID_NODE_EXECUTABLE=${CAPSID_HOST_TEST_NODE}"
                    "-DCAPSID_VENDOR_ROOT=${CAPSID_TXIKI_OVERLAY}"
                    "-DCAPSID_WORKER_WRITER_FIXTURE=$<TARGET_EXISTS:test-worker-writer-fixture>"
                    "-DCAPSID_WORKER_TIMEOUT_WRITER_FIXTURE=$<TARGET_EXISTS:test-worker-timeout-writer-fixture>"
                    "-DCAPSID_EXPECT_ASAN=${CAPSID_ENABLE_ASAN}"
                    "-DCAPSID_EXPECT_UBSAN=${CAPSID_ENABLE_UBSAN}"
                    "-DCAPSID_EXPECT_TSAN=${CAPSID_ENABLE_TSAN}"
                    "-DCAPSID_EXPECT_FUZZERS=${CAPSID_BUILD_FUZZERS}"
                    -P
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestHostSanitizerInstrumentation.cmake"
            )
            set_tests_properties(
                host_sanitizer_instrumentation
                PROPERTIES LABELS "host;security;sanitizer")
        endif()
        find_program(CAPSID_HOST_TEST_NODE NAMES node REQUIRED)
        add_test(NAME host_sanitizer_audit_controls
            COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_sanitizers.mjs")
        set_tests_properties(host_sanitizer_audit_controls PROPERTIES
            ENVIRONMENT "CAPSID_SANITIZER_AUDIT_CMAKE=${CMAKE_COMMAND}"
            TIMEOUT 30)
    endif()

    add_executable(test-protocol tests/test_protocol.cc src/protocol.cc)
    target_include_directories(test-protocol PRIVATE src)
    target_link_libraries(test-protocol PRIVATE capsid_sanitizers)
    add_test(NAME protocol COMMAND test-protocol)

    add_executable(test-fetch-upgrade-metadata
        tests/test_fetch_upgrade_metadata.cc src/protocol.cc
        src/channel_registry.cc src/channel_io.cc)
    target_include_directories(test-fetch-upgrade-metadata PRIVATE src)
    target_link_libraries(test-fetch-upgrade-metadata PRIVATE capsid_sanitizers)
    if(CAPSID_STRICT_WARNINGS)
        if(MSVC)
            target_compile_options(test-fetch-upgrade-metadata PRIVATE /W4 /WX)
        else()
            target_compile_options(test-fetch-upgrade-metadata PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()
    add_test(NAME fetch_upgrade_metadata COMMAND test-fetch-upgrade-metadata)
    set_tests_properties(fetch_upgrade_metadata PROPERTIES TIMEOUT 10 LABELS "protocol;fetch;websocket")
    add_executable(test-fetch-upgrade-abi tests/test_fetch_upgrade_abi.cc)
    target_include_directories(test-fetch-upgrade-abi PRIVATE src)
    target_link_libraries(test-fetch-upgrade-abi PRIVATE capsid_runtime)
    if(CAPSID_STRICT_WARNINGS)
        if(MSVC)
            target_compile_options(test-fetch-upgrade-abi PRIVATE /W4 /WX)
        else()
            target_compile_options(test-fetch-upgrade-abi PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()
    add_test(NAME fetch_upgrade_abi COMMAND test-fetch-upgrade-abi)
    set_tests_properties(fetch_upgrade_abi PROPERTIES TIMEOUT 30 LABELS "protocol;fetch;websocket;abi")

    add_executable(
        test-fetchrpc-v4
        tests/test_fetchrpc_v4.cc
        src/capability_policy.cc
        src/egress_policy.cc
        src/ipc_validation.cc
        src/protocol.cc
        src/response_headers.cc
    )
    # RFC 6455 host codec (FetchRPC v4 WS-C3)
    add_executable(test-ws-codec tests/test_ws_codec.cc src/host/ws_codec.cc)
    target_include_directories(test-ws-codec PRIVATE src src/host)
    target_link_libraries(test-ws-codec PRIVATE capsid_sanitizers)
    add_test(NAME ws_codec COMMAND test-ws-codec)
    target_include_directories(test-fetchrpc-v4 PRIVATE
        include src "${CAPSID_GENERATED_DIR}")
    target_compile_definitions(test-fetchrpc-v4 PRIVATE FETCHRPC_V4_SURFACE)
    target_link_libraries(test-fetchrpc-v4 PRIVATE
        capsid_sanitizers capsid_jansson)
    add_test(
        NAME fetchrpc_v4_boundary
        COMMAND test-fetchrpc-v4
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fetchrpc-v4/vectors.json")

    if(TARGET capsid_host_core)
    # GLM T08: channel capacity ledger + protocol identity Red tests
    add_executable(
        test-host-channel-ledger
        tests/test_host_channel_ledger.cc
        src/host/channel_ledger.cc
        src/host/generation_identity.cc
    )
    target_include_directories(test-host-channel-ledger PRIVATE
        include src src/host "${CAPSID_GENERATED_DIR}")
    target_link_libraries(test-host-channel-ledger PRIVATE
        capsid_host_core
        OpenSSL::Crypto
        capsid_sanitizers capsid_jansson)
    set_target_properties(test-host-channel-ledger PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON)
    add_test(NAME host_channel_ledger COMMAND test-host-channel-ledger)
    add_executable(test-host-channel-budget tests/test_host_channel_budget.cc)
    target_include_directories(test-host-channel-budget PRIVATE include src)
    target_link_libraries(test-host-channel-budget PRIVATE capsid_host_core capsid_sanitizers)
    set_target_properties(test-host-channel-budget PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME host_channel_budget COMMAND test-host-channel-budget)
    set_tests_properties(host_channel_budget PROPERTIES TIMEOUT 30 LABELS "host;websocket;capacity;metrics")
    endif()

    # GLM T07: C11 embedding example, built and executed as a test
    # (real worker, real request, gated channel surface)
    add_executable(c11-channel-probe examples/c11-embed/channel_probe.c)
    target_include_directories(c11-channel-probe PRIVATE include)
    target_link_libraries(c11-channel-probe PRIVATE capsid_runtime)
    if(TARGET capsid-worker)
        add_test(
            NAME c11_channel_probe
            COMMAND c11-channel-probe "$<TARGET_FILE:capsid-worker>")
    endif()

    # GLM T07: fake worker-side peer + the channel C ABI tests
    # No sanitizers: the peer runs under the worker's spawn preamble
    # (closed stdio, restricted env) where sanitizer runtimes are not
    # part of the contract; it mirrors the production worker binary.
    # Native Windows channel-peer coverage is still a release gate.
    # This fixture uses the POSIX --ipc-fd bootstrap and unistd APIs.
    if(NOT WIN32)
        add_executable(fake-worker-peer tests/fake_worker_peer.cc
            src/protocol.cc)
        target_include_directories(fake-worker-peer PRIVATE src)

        add_executable(
            test-channel-abi
            tests/test_channel_abi.cc
        )
        target_include_directories(test-channel-abi PRIVATE
            include src)
        target_compile_definitions(test-channel-abi PRIVATE
            CAPSID_FAKE_PEER_PATH="${CMAKE_CURRENT_BINARY_DIR}/fake-worker-peer")
        target_link_libraries(test-channel-abi PRIVATE capsid_runtime)
        add_dependencies(test-channel-abi fake-worker-peer)
        add_test(NAME channel_abi COMMAND test-channel-abi)
        set_tests_properties(channel_abi PROPERTIES TIMEOUT 60)

    endif()

    # GLM T06: production channel registry runs the frozen v4
    # sequences plus its budget-guarantee units
    add_executable(
        test-channel-registry
        tests/test_channel_registry.cc
        src/channel_registry.cc
        src/protocol.cc
    )
    target_include_directories(test-channel-registry PRIVATE
        src)
    target_link_libraries(test-channel-registry PRIVATE
        capsid_sanitizers capsid_jansson)
    add_test(
        NAME channel_registry
        COMMAND test-channel-registry
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fetchrpc-v4/vectors.json")

    add_executable(test-channel-races
        tests/test_channel_races.cc src/channel_registry.cc src/protocol.cc)
    target_include_directories(test-channel-races PRIVATE src)
    target_link_libraries(test-channel-races PRIVATE capsid_sanitizers)
    set_target_properties(test-channel-races PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    add_test(NAME channel_two_peer_races COMMAND test-channel-races)
    set_tests_properties(channel_two_peer_races PROPERTIES TIMEOUT 15)

    add_executable(test-channel-io tests/test_channel_io.cc
        src/channel_io.cc src/channel_registry.cc src/protocol.cc)
    target_include_directories(test-channel-io PRIVATE src)
    target_link_libraries(test-channel-io PRIVATE capsid_sanitizers)
    set_target_properties(test-channel-io PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    if(MSVC)
        # Global allocation faults must not target Debug STL's diagnostic
        # _Container_proxy allocation in noexcept basic_string construction.
        # Keep every actual receive/send/control fault and rollback assertion
        # in this standalone target, using the production iterator ABI.
        target_compile_definitions(test-channel-io PRIVATE _ITERATOR_DEBUG_LEVEL=0)
        # Independently execute the real Debug iterator contracts with no
        # synthetic heap failure inside the CRT's noexcept diagnostics.
        # Product, sanitizer and warning flags remain unchanged.
        add_executable(test-channel-io-debug tests/test_channel_io.cc
            src/channel_io.cc src/channel_registry.cc src/protocol.cc)
        target_include_directories(test-channel-io-debug PRIVATE src)
        target_link_libraries(test-channel-io-debug PRIVATE capsid_sanitizers)
        target_compile_definitions(test-channel-io-debug PRIVATE
            _ITERATOR_DEBUG_LEVEL=2 CAPSID_CHANNEL_IO_NO_FAULT_SWEEP=1)
        set_target_properties(test-channel-io-debug PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
            MSVC_RUNTIME_LIBRARY MultiThreadedDebug)
        add_test(NAME channel_owned_io_debug COMMAND test-channel-io-debug)
        set_tests_properties(channel_owned_io_debug PROPERTIES TIMEOUT 15 LABELS "windows;protocol;debug")
    endif()
    add_test(NAME channel_owned_io COMMAND test-channel-io)
    add_executable(test-ws-stream tests/test_ws_stream.cc src/host/ws_stream.cc src/protocol.cc)
    target_include_directories(test-ws-stream PRIVATE src)
    target_link_libraries(test-ws-stream PRIVATE capsid_sanitizers)
    add_test(NAME ws_incremental_stream COMMAND test-ws-stream)
    set_tests_properties(ws_incremental_stream PROPERTIES TIMEOUT 15 LABELS "host;websocket")
    if(TARGET capsid_host_core AND Boost_FOUND)
        add_executable(test-host-websocket-session tests/test_host_websocket_session.cc)
        target_include_directories(test-host-websocket-session PRIVATE include src)
        target_link_libraries(test-host-websocket-session PRIVATE capsid_host_core Boost::system capsid_sanitizers)
        set_target_properties(test-host-websocket-session PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        add_test(NAME host_websocket_session_lifecycle COMMAND test-host-websocket-session)
        set_tests_properties(host_websocket_session_lifecycle PROPERTIES TIMEOUT 15 LABELS "host;websocket;lifecycle")
        add_executable(test-host-dns-dialer tests/test_host_dns_dialer.cc)
        target_include_directories(test-host-dns-dialer PRIVATE include src)
        target_link_libraries(test-host-dns-dialer PRIVATE capsid_host_core Boost::system capsid_sanitizers)
        set_target_properties(test-host-dns-dialer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_definitions(test-host-dns-dialer PRIVATE CAPSID_TEST_WRAP_CONNECT=1)
            target_link_options(test-host-dns-dialer PRIVATE "LINKER:--wrap=connect")
        endif()
        add_test(NAME host_dns_dialer_lifecycle COMMAND test-host-dns-dialer)
        set_tests_properties(host_dns_dialer_lifecycle PROPERTIES TIMEOUT 30 LABELS "host;websocket;lifecycle")
        add_test(NAME host_dns_reserved_invalid COMMAND test-host-dns-dialer --reserved-invalid)
        set_tests_properties(host_dns_reserved_invalid PROPERTIES TIMEOUT 30 LABELS "host;websocket;lifecycle")
        # Exercise the controlled absolute DNS names with a real search list.
        # This process-local configuration also catches accidental dependence
        # on a runner's ambient resolv.conf without changing native resolution.
        set_tests_properties(host_dns_dialer_lifecycle host_dns_reserved_invalid
            PROPERTIES ENVIRONMENT "LOCALDOMAIN=search.capsid.test;RES_OPTIONS=ndots:5")
        if(TARGET capsid-worker)
            add_executable(test-host-channel-lifecycle tests/test_host_channel_lifecycle.cc)
            target_include_directories(test-host-channel-lifecycle PRIVATE include src)
            target_link_libraries(test-host-channel-lifecycle PRIVATE capsid_host_core Boost::system capsid_sanitizers)
            set_target_properties(test-host-channel-lifecycle PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
            add_dependencies(test-host-channel-lifecycle capsid-worker)
            add_test(NAME host_channel_generation_lifecycle COMMAND test-host-channel-lifecycle $<TARGET_FILE:capsid-worker>)
            set_tests_properties(host_channel_generation_lifecycle PROPERTIES TIMEOUT 45 LABELS "host;websocket;lifecycle")
        endif()
    endif()
    set_tests_properties(channel_owned_io PROPERTIES TIMEOUT 15)

    add_executable(
        test-outbound-buffer
        tests/test_outbound_buffer.cc
        src/protocol.cc
    )
    target_include_directories(test-outbound-buffer PRIVATE src)
    target_link_libraries(test-outbound-buffer PRIVATE capsid_sanitizers)
    add_test(NAME outbound_buffer COMMAND test-outbound-buffer)

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

    # The ABI guard deliberately defines its own operator new/delete to
    # audit the exported allocator surface; under TSan the clang tsan_cxx
    # runtime defines the same operators and the link fails with multiple
    # definitions. The guard's contract is allocator-ABI only — it is not
    # a TSan target — so it is not built in the tsan matrix.
    if(NOT CAPSID_ENABLE_TSAN)
    add_executable(test-abi-guard tests/test_abi_guard.cc)
    target_link_libraries(test-abi-guard PRIVATE capsid_runtime)
    if(CAPSID_STRICT_WARNINGS)
        if(MSVC)
            target_compile_options(test-abi-guard PRIVATE /W4 /WX)
        else()
            target_compile_options(test-abi-guard PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()
    if(TARGET capsid-worker)
        add_test(
            NAME abi_guard_oom_countdown
            COMMAND test-abi-guard $<TARGET_FILE:capsid-worker>)
        foreach(CAPSID_ABI_OOM_MODE IN ITEMS
                egress-empty egress-rules capability-empty permission-rule
                capability-snapshot v4-carriage v4-channels snapshot-v4-channels)
            string(REPLACE "-" "_" CAPSID_ABI_OOM_CASE "${CAPSID_ABI_OOM_MODE}")
            set(CAPSID_ABI_OOM_TEST "abi_guard_oom_${CAPSID_ABI_OOM_CASE}")
            add_test(NAME "${CAPSID_ABI_OOM_TEST}"
                COMMAND test-abi-guard $<TARGET_FILE:capsid-worker> "${CAPSID_ABI_OOM_MODE}")
            set_tests_properties("${CAPSID_ABI_OOM_TEST}" PROPERTIES
                TIMEOUT 60 LABELS "abi;memory")
        endforeach()
    else()
        add_test(NAME abi_guard_oom_countdown COMMAND test-abi-guard)
    endif()
    add_test(NAME abi_guard_without_worker COMMAND test-abi-guard)
    set_tests_properties(abi_guard_oom_countdown abi_guard_without_worker PROPERTIES
        TIMEOUT 60 LABELS "abi;memory")
    endif()

    add_executable(test-abi-guard-c tests/test_abi_guard_c.c)
    target_link_libraries(test-abi-guard-c PRIVATE capsid_runtime)
    if(TARGET capsid-worker)
        add_test(
            NAME abi_guard_c_caller
            COMMAND test-abi-guard-c $<TARGET_FILE:capsid-worker>)
    else()
        add_test(NAME abi_guard_c_caller COMMAND test-abi-guard-c)
    endif()
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
        find_program(CAPSID_PYTHON3_EXECUTABLE NAMES python3 python REQUIRED)
        add_custom_target(verify-fetchrpc-fuzz-corpus
            COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-fetchrpc-fuzz-corpus.py" --check
            VERBATIM)
        add_test(NAME fetchrpc_fuzz_corpus_generated
            COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-fetchrpc-fuzz-corpus.py" --check)
        set_tests_properties(fetchrpc_fuzz_corpus_generated PROPERTIES
            LABELS "fuzz;source" TIMEOUT 10)
        file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus"
            DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/fuzz")

        function(capsid_add_fuzzer TARGET_NAME SOURCE_FILE CORPUS_NAME)
            add_executable("${TARGET_NAME}" "${SOURCE_FILE}" ${ARGN})
            target_include_directories("${TARGET_NAME}" PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_compile_options("${TARGET_NAME}" PRIVATE
                -O1
                -fsanitize=fuzzer,address,undefined
                -fno-omit-frame-pointer
                -fno-sanitize-recover=all
            )
            target_link_options("${TARGET_NAME}" PRIVATE
                -O1
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
                    -print_final_stats=1
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
        capsid_add_fuzzer(
            fuzz-channel-registry
            tests/fuzz/fuzz_channel_registry.cc
            channel_registry
            src/channel_registry.cc
            src/protocol.cc
        )
        capsid_add_fuzzer(
            fuzz-websocket-frames
            tests/fuzz/fuzz_websocket_frames.cc
            websocket_frames
            src/host/ws_codec.cc
            src/host/ws_stream.cc
            src/protocol.cc
        )
        # Keep full fuzz/ASan/UBSan instrumentation and every existing gate.
        target_compile_options(fuzz-websocket-frames PRIVATE -O2)
        # Compile every exercised project source with fuzzer/ASan/UBSan flags.
        # OpenSSL is only an independent digest oracle. Do not substitute
        # an ordinary, uninstrumented capsid_host_core archive here.
        find_package(Boost 1.74 REQUIRED)
        find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
        capsid_add_fuzzer(
            fuzz-websocket-handshake
            tests/fuzz/fuzz_websocket_handshake.cc
            websocket_handshake
            src/host/ws_codec.cc
            src/host/request_normalization.cc
            src/host/websocket_session.cc
            src/host/ws_stream.cc
            src/host/channel_ledger.cc
            src/protocol.cc
        )
        set_target_properties(fuzz-websocket-handshake PROPERTIES CXX_STANDARD 20)
        target_link_libraries(fuzz-websocket-handshake PRIVATE OpenSSL::Crypto)
        if(TARGET Boost::headers)
            target_link_libraries(fuzz-websocket-handshake PRIVATE Boost::headers)
        else()
            target_include_directories(fuzz-websocket-handshake SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
        endif()
        add_custom_target(verify-channel-websocket-fuzz-corpus
            COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-channel-websocket-fuzz-corpus.py" --check
            VERBATIM)
        add_test(NAME channel_websocket_fuzz_corpus_generated
            COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-channel-websocket-fuzz-corpus.py" --check)
        set_tests_properties(channel_websocket_fuzz_corpus_generated PROPERTIES
            LABELS "fuzz;source" TIMEOUT 10)
        add_dependencies(fuzz-channel-registry verify-channel-websocket-fuzz-corpus)
        add_dependencies(fuzz-websocket-frames verify-channel-websocket-fuzz-corpus)
        add_dependencies(fuzz-websocket-handshake verify-channel-websocket-fuzz-corpus)
        add_dependencies(fuzz-protocol-parser verify-fetchrpc-fuzz-corpus)
        add_dependencies(fuzz-worker-protocol verify-fetchrpc-fuzz-corpus)
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

        set(CAPSID_BINDING_IMPORT_FIXTURE
            "${CAPSID_GENERATED_DIR}/test-binding-import.js")
        add_custom_command(
            OUTPUT "${CAPSID_BINDING_IMPORT_FIXTURE}"
            COMMAND "${CAPSID_ESBUILD}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/binding-import.js"
                --bundle
                --target=esnext
                --platform=neutral
                --format=esm
                "--external:capsid:binding/*"
                "--outfile=${CAPSID_BINDING_IMPORT_FIXTURE}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/binding-import.js"
            VERBATIM
        )
        add_custom_target(test-binding-import-fixture
            DEPENDS "${CAPSID_BINDING_IMPORT_FIXTURE}")

        set(CAPSID_BINDING_CALL_FIXTURE
            "${CAPSID_GENERATED_DIR}/test-binding-call.js")
        add_custom_command(
            OUTPUT "${CAPSID_BINDING_CALL_FIXTURE}"
            COMMAND "${CAPSID_ESBUILD}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/binding-call.js"
                --bundle
                --target=esnext
                --platform=neutral
                --format=esm
                "--external:capsid:binding/*"
                "--outfile=${CAPSID_BINDING_CALL_FIXTURE}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/binding-call.js"
            VERBATIM
        )
        add_custom_target(test-binding-call-fixture
            DEPENDS "${CAPSID_BINDING_CALL_FIXTURE}")

        set(CAPSID_BINDING_OWNER_FIXTURE
            "${CAPSID_GENERATED_DIR}/test-binding-owner.js")
        add_custom_command(
            OUTPUT "${CAPSID_BINDING_OWNER_FIXTURE}"
            COMMAND "${CAPSID_ESBUILD}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/binding-owner.js"
                --bundle
                --target=esnext
                --platform=neutral
                --format=esm
                "--external:capsid:binding/*"
                "--outfile=${CAPSID_BINDING_OWNER_FIXTURE}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/binding-owner.js"
            VERBATIM
        )
        add_custom_target(test-binding-owner-fixture
            DEPENDS "${CAPSID_BINDING_OWNER_FIXTURE}")

        set(CAPSID_BINDING_LIFECYCLE_FIXTURE
            "${CAPSID_GENERATED_DIR}/test-binding-lifecycle.js")
        add_custom_command(
            OUTPUT "${CAPSID_BINDING_LIFECYCLE_FIXTURE}"
            COMMAND "${CAPSID_ESBUILD}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/binding-lifecycle.js"
                --bundle
                --target=esnext
                --platform=neutral
                --format=esm
                "--external:capsid:binding/*"
                "--outfile=${CAPSID_BINDING_LIFECYCLE_FIXTURE}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/binding-lifecycle.js"
            VERBATIM
        )
        add_custom_target(test-binding-lifecycle-fixture
            DEPENDS "${CAPSID_BINDING_LIFECYCLE_FIXTURE}")

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
        add_test(NAME testing_validity_workflow_controls
            COMMAND "${CAPSID_NODE_EXECUTABLE}" --test
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_testing_validity_workflow.mjs")
        set_tests_properties(testing_validity_workflow_controls PROPERTIES
            TIMEOUT 30 LABELS "ci;audit;contract")
        if(CAPSID_PYTHON3_EXECUTABLE)
            add_test(NAME native_provider_patch_controls
                COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_provider_patches.py")
            set_tests_properties(native_provider_patch_controls PROPERTIES
                TIMEOUT 20 LABELS "build;dependency;sanitizer;contract")
        endif()
        add_test(NAME documentation_archive_contract_controls
            COMMAND "${CAPSID_NODE_EXECUTABLE}" --test
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_documentation_archive.mjs")
        set_tests_properties(documentation_archive_contract_controls PROPERTIES
            TIMEOUT 20 LABELS "documentation;audit;contract")
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
        add_test(NAME cli_entrypoint_controls
            COMMAND "${CAPSID_NODE_EXECUTABLE}" --test
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli_entrypoints.mjs")
        set_tests_properties(cli_entrypoint_controls PROPERTIES
            TIMEOUT 30 LABELS "audit")
        # WP-03 §7.3: every async entry point into JS from the txiki
        # overlay must be classified (context-wired / profile-unreachable /
        # synchronous-reentry / value-only) in
        # tools/audit-txiki-async-context.py; an unclassified site fails.
        # Windows Python installs expose python.exe; POSIX exposes python3.
        find_program(CAPSID_PYTHON3_EXECUTABLE NAMES python3 python REQUIRED)
        add_test(
            NAME txiki_async_context_inventory_audit
            COMMAND "${CAPSID_PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/audit-txiki-async-context.py"
                "${CAPSID_TXIKI_OVERLAY}/src"
        )
        set_tests_properties(
            txiki_async_context_inventory_audit
            PROPERTIES TIMEOUT 20 LABELS "security;ci;audit"
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

        set(CAPSID_ELYSIA_REFERENCE_ROOT
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/elysia-reference")
        set(CAPSID_ELYSIA_VERSION "1.4.29")
        if(NOT EXISTS
           "${CAPSID_ELYSIA_REFERENCE_ROOT}/node_modules/elysia/package.json")
            message(FATAL_ERROR
                "Elysia ${CAPSID_ELYSIA_VERSION} is missing; run "
                "npm ci --ignore-scripts --prefix examples/elysia-reference")
        endif()
        file(READ
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/package.json"
            CAPSID_ELYSIA_REFERENCE_PACKAGE)
        file(READ
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/package-lock.json"
            CAPSID_ELYSIA_REFERENCE_LOCK)
        file(READ
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/node_modules/elysia/package.json"
            CAPSID_ELYSIA_INSTALLED_PACKAGE)
        string(FIND
            "${CAPSID_ELYSIA_REFERENCE_PACKAGE}"
            "\"elysia\": \"${CAPSID_ELYSIA_VERSION}\""
            CAPSID_ELYSIA_PACKAGE_VERSION_OFFSET)
        string(FIND
            "${CAPSID_ELYSIA_REFERENCE_LOCK}"
            "\"elysia\": \"${CAPSID_ELYSIA_VERSION}\""
            CAPSID_ELYSIA_LOCK_VERSION_OFFSET)
        string(FIND
            "${CAPSID_ELYSIA_INSTALLED_PACKAGE}"
            "\"version\": \"${CAPSID_ELYSIA_VERSION}\""
            CAPSID_ELYSIA_INSTALLED_VERSION_OFFSET)
        if(CAPSID_ELYSIA_PACKAGE_VERSION_OFFSET EQUAL -1 OR
           CAPSID_ELYSIA_LOCK_VERSION_OFFSET EQUAL -1 OR
           CAPSID_ELYSIA_INSTALLED_VERSION_OFFSET EQUAL -1)
            message(FATAL_ERROR
                "examples/elysia-reference package, lockfile and install "
                "must all pin exactly Elysia ${CAPSID_ELYSIA_VERSION}")
        endif()
        file(GLOB_RECURSE CAPSID_ELYSIA_BUNDLE_DEPS CONFIGURE_DEPENDS
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/src/*.js"
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/node_modules/elysia/dist/*"
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/node_modules/cookie/dist/*"
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/node_modules/memoirist/dist/*"
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/node_modules/exact-mirror/dist/*"
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/node_modules/fast-decode-uri-component/*"
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/node_modules/@sinclair/typebox/build/*"
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/node_modules/@elysiajs/*/dist/*"
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/node_modules/jose/dist/*"
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/node_modules/nanoid/*"
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/package.json"
            "${CAPSID_ELYSIA_REFERENCE_ROOT}/package-lock.json"
        )

        # Elysia-specific esbuild flags: --main-fields=main,module because
        # cookie ships a CJS main and the neutral platform ignores it; and
        # --external:file-type because Elysia imports it dynamically behind a
        # runtime catch, so leaving it external keeps the bundle self-contained.
        function(capsid_add_elysia_fixture name entry)
            set(output
                "${CAPSID_GENERATED_DIR}/test-elysia-${name}.js")
            set(metafile "${output}.meta.json")
            add_custom_command(
                OUTPUT "${output}" "${metafile}"
                COMMAND "${CAPSID_ESBUILD}"
                    "${CAPSID_ELYSIA_REFERENCE_ROOT}/src/${entry}"
                    --bundle
                    --minify
                    --keep-names
                    --tree-shaking=true
                    --target=esnext
                    --platform=neutral
                    --format=esm
                    --main-fields=main,module
                    --external:file-type
                    "--alias:nanoid=${CAPSID_ELYSIA_REFERENCE_ROOT}/node_modules/nanoid/index.browser.js"
                    "--metafile=${metafile}"
                    "--outfile=${output}"
                COMMAND "${CAPSID_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/elysia/audit-bundle.mjs"
                    "${output}"
                    "${metafile}"
                DEPENDS
                    ${CAPSID_ELYSIA_BUNDLE_DEPS}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/elysia/audit-bundle.mjs"
                VERBATIM
            )
            add_custom_target("test-elysia-${name}-fixture"
                DEPENDS "${output}" "${metafile}")
            set("CAPSID_ELYSIA_${name}_FIXTURE"
                "${output}" PARENT_SCOPE)
        endfunction()

        capsid_add_elysia_fixture(DEFAULT entry-default.js)
        capsid_add_elysia_fixture(NAMED entry-named.js)

        if(CAPSID_WPT_ROOT)
            set(CAPSID_WPT_CONFIGURE_MANIFEST
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/manifest.json")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${CAPSID_WPT_CONFIGURE_MANIFEST}")
            file(READ
                "${CAPSID_WPT_CONFIGURE_MANIFEST}"
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
                string(JSON CAPSID_EXPECTED_ERROR_NAME GET
                    "${CAPSID_WPT_MANIFEST_JSON}"
                    expectedFailures ${CAPSID_EXPECTED_INDEX} errorName)
                string(JSON CAPSID_EXPECTED_MESSAGE GET
                    "${CAPSID_WPT_MANIFEST_JSON}"
                    expectedFailures ${CAPSID_EXPECTED_INDEX} message)
                string(REPLACE "\\" "\\\\" CAPSID_EXPECTED_SUBTEST_JS
                    "${CAPSID_EXPECTED_SUBTEST}")
                string(REPLACE "'" "\\'" CAPSID_EXPECTED_SUBTEST_JS
                    "${CAPSID_EXPECTED_SUBTEST_JS}")
                string(REPLACE "\\" "\\\\" CAPSID_EXPECTED_DEVIATION_JS
                    "${CAPSID_EXPECTED_DEVIATION}")
                string(REPLACE "'" "\\'" CAPSID_EXPECTED_DEVIATION_JS
                    "${CAPSID_EXPECTED_DEVIATION_JS}")
                foreach(CAPSID_EXPECTED_FIELD ERROR_NAME MESSAGE)
                    string(REPLACE "\\" "\\\\"
                        CAPSID_EXPECTED_${CAPSID_EXPECTED_FIELD}_JS
                        "${CAPSID_EXPECTED_${CAPSID_EXPECTED_FIELD}}")
                    string(REPLACE "'" "\\'"
                        CAPSID_EXPECTED_${CAPSID_EXPECTED_FIELD}_JS
                        "${CAPSID_EXPECTED_${CAPSID_EXPECTED_FIELD}_JS}")
                endforeach()
                string(MAKE_C_IDENTIFIER
                    "${CAPSID_EXPECTED_PATH}" CAPSID_EXPECTED_PATH_ID)
                set(CAPSID_EXPECTED_VARIABLE
                    "CAPSID_WPT_EXPECTED_FAILURES_${CAPSID_EXPECTED_PATH_ID}")
                set(CAPSID_EXPECTED_OBJECT
                    "{ name: '${CAPSID_EXPECTED_SUBTEST_JS}', deviation: '${CAPSID_EXPECTED_DEVIATION_JS}', errorName: '${CAPSID_EXPECTED_ERROR_NAME_JS}', message: '${CAPSID_EXPECTED_MESSAGE_JS}' }")
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
            set(CAPSID_WPT_INPUT_PATHS resources/testharness.js)
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
                if(CAPSID_WPT_PATH STREQUAL
                   "html/webappapis/scripting/processing-model-2/unhandled-promise-rejections/support/promise-rejection-events.js")
                    set(CAPSID_WPT_SOURCE
                        "${CAPSID_GENERATED_DIR}/wpt-${CAPSID_WPT_TEST_ID}-guarded.js")
                    add_custom_command(
                        OUTPUT "${CAPSID_WPT_SOURCE}"
                        COMMAND "${CAPSID_NODE_EXECUTABLE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/guard-rejection-events.mjs"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                            "${CAPSID_WPT_SOURCE}"
                        DEPENDS
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/guard-rejection-events.mjs"
                            "${CAPSID_WPT_ROOT}/${CAPSID_WPT_PATH}"
                        VERBATIM)
                    list(APPEND CAPSID_WPT_SOURCE_DEPS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/guard-rejection-events.mjs")
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
                foreach(CAPSID_WPT_DEP IN LISTS CAPSID_WPT_SOURCE_DEPS)
                    string(FIND "${CAPSID_WPT_DEP}" "${CAPSID_WPT_ROOT}/"
                        CAPSID_WPT_DEP_PREFIX)
                    if(CAPSID_WPT_DEP_PREFIX EQUAL 0)
                        file(RELATIVE_PATH CAPSID_WPT_RELATIVE_DEP
                            "${CAPSID_WPT_ROOT}" "${CAPSID_WPT_DEP}")
                        list(APPEND CAPSID_WPT_INPUT_PATHS "${CAPSID_WPT_RELATIVE_DEP}")
                    endif()
                endforeach()
                set("CAPSID_WPT_FIXTURE_${CAPSID_WPT_TEST_ID}"
                    "${CAPSID_WPT_FIXTURE}")
            endforeach()
            list(REMOVE_DUPLICATES CAPSID_WPT_INPUT_PATHS)
            list(SORT CAPSID_WPT_INPUT_PATHS)
            string(REPLACE ";" "\n" CAPSID_WPT_INPUT_PATHS_TEXT "${CAPSID_WPT_INPUT_PATHS}")
            set(CAPSID_WPT_INPUT_LIST "${CAPSID_GENERATED_DIR}/wpt-input-paths.txt")
            file(GENERATE OUTPUT "${CAPSID_WPT_INPUT_LIST}"
                CONTENT "${CAPSID_WPT_INPUT_PATHS_TEXT}\n")
            set(CAPSID_WPT_AUDIT_COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/audit-source.mjs"
                "${CAPSID_WPT_ROOT}" "${CAPSID_WPT_REVISION}" "${CAPSID_WPT_INPUT_LIST}")
            add_custom_target(test-wpt-source-integrity
                COMMAND ${CAPSID_WPT_AUDIT_COMMAND} VERBATIM)
            foreach(CAPSID_WPT_FIXTURE_TARGET IN LISTS CAPSID_WPT_FIXTURE_TARGETS)
                add_dependencies("${CAPSID_WPT_FIXTURE_TARGET}" test-wpt-source-integrity)
            endforeach()
            add_test(NAME wpt_source_integrity COMMAND ${CAPSID_WPT_AUDIT_COMMAND})
            set_tests_properties(wpt_source_integrity PROPERTIES
                TIMEOUT 60 LABELS "conformance" FIXTURES_SETUP capsid_wpt_source)
            add_test(NAME gb18030_ranges_generated
                COMMAND "${CAPSID_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-gb18030-ranges.mjs"
                    "${CAPSID_WPT_ROOT}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/js/encoding-data/gb18030-ranges.js" --check)
            set_tests_properties(gb18030_ranges_generated PROPERTIES
                TIMEOUT 30 LABELS "conformance;encoding" FIXTURES_REQUIRED capsid_wpt_source)
            add_test(NAME multibyte_indexes_generated
                COMMAND "${CAPSID_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-multibyte-indexes.mjs"
                    "${CAPSID_WPT_ROOT}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/js/encoding-data/multibyte-indexes.js" --check)
            set_tests_properties(multibyte_indexes_generated PROPERTIES
                TIMEOUT 30 LABELS "conformance;encoding" FIXTURES_REQUIRED capsid_wpt_source)
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
        add_executable(
            test-worker-exit-fields
            tests/test_worker_exit_fields.cc)
        target_include_directories(test-worker-exit-fields PRIVATE tests)
        target_link_libraries(
            test-worker-exit-fields
            PRIVATE capsid_runtime
        )
        add_test(
            NAME worker_exit_fields
            COMMAND test-worker-exit-fields
                $<TARGET_FILE:capsid-worker>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ipc-sync-response.js"
        )
        set_tests_properties(worker_exit_fields PROPERTIES TIMEOUT 20)
        foreach(CAPSID_EXIT_MODE IN ITEMS cleared-timer retained-timer poisoned-timer
                gc-cycle denied-empty denied-known denied-stream)
            string(REPLACE "-" "_" CAPSID_EXIT_NAME "${CAPSID_EXIT_MODE}")
            add_test(NAME "worker_exit_${CAPSID_EXIT_NAME}"
                COMMAND test-worker-integration
                    $<TARGET_FILE:capsid-worker>
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worker-exit-timer.js"
                    "exit-${CAPSID_EXIT_MODE}")
            set_tests_properties("worker_exit_${CAPSID_EXIT_NAME}"
                PROPERTIES TIMEOUT 20 LABELS "worker;exit")
        endforeach()
        add_test(
            NAME worker_bodyless_end_failure
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ipc-sync-response.js"
                bodyless-end-failure
        )
        set_tests_properties(worker_bodyless_end_failure PROPERTIES TIMEOUT 20)
        add_test(
            NAME worker_incoming_request_fast_path
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/incoming-request-fast-path.js"
                incoming-request-fast-path
        )
        set_tests_properties(worker_incoming_request_fast_path PROPERTIES TIMEOUT 20)
        add_test(
            NAME worker_incoming_request_invalid_header_name
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ipc-sync-response.js"
                invalid-request-header-name
        )
        set_tests_properties(
            worker_incoming_request_invalid_header_name PROPERTIES TIMEOUT 20)
        add_test(
            NAME worker_incoming_request_invalid_header_value
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ipc-sync-response.js"
                invalid-request-header-value
        )
        set_tests_properties(
            worker_incoming_request_invalid_header_value PROPERTIES TIMEOUT 20)
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
                "${CMAKE_CURRENT_SOURCE_DIR}/docs/conformance.md"
        )
        add_test(
            NAME wpt_metadata_negative_controls
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/audit-metadata.test.mjs"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/manifest.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/docs/conformance.md"
        )
        add_test(
            NAME wpt_report_contract_controls
            COMMAND "${CAPSID_NODE_EXECUTABLE}" --test
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/report-contract.test.mjs"
        )
        add_test(
            NAME wpt_adapter_contract_controls
            COMMAND "${CAPSID_NODE_EXECUTABLE}" --test
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/adapter-contract.test.mjs"
        )
        add_test(
            NAME wpt_source_integrity_controls
            COMMAND "${CAPSID_NODE_EXECUTABLE}" --test
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/audit-source.test.mjs"
        )
        add_test(NAME workerd_wpt_profile_metadata
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/workerd-wpt-profile.mjs" metadata)
        add_test(NAME workerd_wpt_profile_controls
            COMMAND "${CAPSID_NODE_EXECUTABLE}" --test
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_wpt_profile.mjs")
        set_tests_properties(workerd_wpt_profile_metadata workerd_wpt_profile_controls
            PROPERTIES TIMEOUT 30 LABELS "conformance;workerd-profile")
        # v1 remains a historical two-file contract. The accepted default
        # behavior now has a separately versioned five-file comparison.
        add_test(NAME workerd_wpt_profile_v2_metadata
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/workerd-wpt-profile-v2.mjs" metadata)
        add_test(NAME workerd_wpt_profile_v2_controls
            COMMAND "${CAPSID_NODE_EXECUTABLE}" --test
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workerd_wpt_profile_v2.mjs")
        set_tests_properties(workerd_wpt_profile_v2_metadata workerd_wpt_profile_v2_controls
            PROPERTIES TIMEOUT 30 LABELS "conformance;workerd-profile")
        if(CAPSID_WPT_ROOT)
            add_test(NAME wpt_rejection_presence_controls
                COMMAND "${CAPSID_NODE_EXECUTABLE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/guard-rejection-events.test.mjs")
            set_tests_properties(wpt_rejection_presence_controls PROPERTIES
                TIMEOUT 30 LABELS "conformance"
                FIXTURES_REQUIRED capsid_wpt_source
                ENVIRONMENT "CAPSID_WPT_ROOT=${CAPSID_WPT_ROOT}")
            add_test(NAME wpt_rejection_presence_runtime_controls
                COMMAND "${CAPSID_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt/guard-rejection-runtime.mjs"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --wpt-root "${CAPSID_WPT_ROOT}"
                    --integration $<TARGET_FILE:test-worker-integration>
                    --worker $<TARGET_FILE:capsid-worker>
                    --esbuild "${CAPSID_ESBUILD}")
            set_tests_properties(wpt_rejection_presence_runtime_controls PROPERTIES
                TIMEOUT 60 LABELS "conformance"
                FIXTURES_REQUIRED capsid_wpt_source)
            set_tests_properties(wpt_adapter_contract_controls PROPERTIES
                FIXTURES_REQUIRED capsid_wpt_source
                ENVIRONMENT
                    "CAPSID_WPT_HARNESS=${CAPSID_WPT_ROOT}/resources/testharness.js")
        endif()
        set_tests_properties(
            wpt_metadata_manifest
            wpt_metadata_negative_controls
            wpt_report_contract_controls
            wpt_adapter_contract_controls
            wpt_source_integrity_controls
            PROPERTIES TIMEOUT 30 LABELS "conformance")
        add_test(
            NAME txiki_vendor_patch_integrity
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_TXIKI_VENDOR=${CMAKE_CURRENT_SOURCE_DIR}/vendor/txiki.js"
                "-DCAPSID_TXIKI_PATCH_DIR=${CMAKE_CURRENT_SOURCE_DIR}/patches/txiki"
                "-DCAPSID_TXIKI_EXPECTED_TAG=v26.6.0"
                "-DCAPSID_TXIKI_OVERLAY_STAMP=${CAPSID_OVERLAY_STAMP}"
                "-DCAPSID_TXIKI_PREPARE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareTxiki.cmake"
                "-DCAPSID_TXIKI_PROBE_DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/txiki-patch-probe"
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
                "-DCAPSID_LOCKED_IDENTITY_MANIFEST=${CAPSID_LOCKED_IDENTITY_MANIFEST}"
                "-DCAPSID_WPT_MANIFEST=${CAPSID_WPT_CONFIGURE_MANIFEST}"
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
            NAME worker_fetch_hostname_authorizes_resolved_loopback
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_FETCH_FIXTURE}"
                fetch-hostname-egress
        )
        set_tests_properties(
            worker_fetch_hostname_authorizes_resolved_loopback
            PROPERTIES TIMEOUT 20)
        add_test(
            NAME worker_fetch_host_deny_diagnostic
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_FETCH_FIXTURE}"
                fetch-host-denied
        )
        set_tests_properties(
            worker_fetch_host_deny_diagnostic PROPERTIES TIMEOUT 20)
        add_test(
            NAME worker_fetch_protected_deny_diagnostic
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_FETCH_FIXTURE}"
                fetch-protected-denied
        )
        set_tests_properties(
            worker_fetch_protected_deny_diagnostic PROPERTIES TIMEOUT 20)
        add_test(
            NAME worker_fetch_explicit_deny_diagnostic
            COMMAND test-worker-integration
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_FETCH_FIXTURE}"
                fetch-address-explicit-deny
        )
        set_tests_properties(
            worker_fetch_explicit_deny_diagnostic PROPERTIES TIMEOUT 20)
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
                    PROPERTIES TIMEOUT 20 FIXTURES_REQUIRED capsid_wpt_source
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
            # claims in docs/conformance.md.
            # If it silently does not run, `ctest` reports all-green while the
            # conformance surface is entirely untested. Register a test that fails
            # loudly so that absence of coverage can never be mistaken for a pass.
            # Windows supports the same opt-in WPT build. Its default build
            # still omits the loud-fail gate; that build alone is not WPT evidence.
            if(NOT WIN32)
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
        target_compile_definitions(test-permissions-integration PRIVATE
            "CAPSID_TEST_RUNTIME_VERSION=\"${PROJECT_VERSION}\"")
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
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(
            NAME worker_fs_module_contract
            COMMAND test-permissions-integration
                $<TARGET_FILE:capsid-worker>
                --fs
        )
        endif()
        set_tests_properties(
            worker_permissions_contract
            worker_utility_modules_contract
            worker_environment_module_contract
            worker_system_module_contract
            worker_storage_module_contract
            worker_stdio_module_contract
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
                "-DCAPSID_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/Testing/ec"
                "-DCAPSID_GENERATOR=${CMAKE_GENERATOR}"
                "-DCAPSID_C_COMPILER=${CMAKE_C_COMPILER}"
                "-DCAPSID_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
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
        # The binary audit drives GNU nm/ar/objcopy/strip semantics that
        # do not exist on the macOS toolchain; it is Linux-only.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(
            NAME worker_binary_audit
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_WORKER=$<TARGET_FILE:capsid-worker>"
                "-DCAPSID_TJS_ARCHIVE=$<TARGET_FILE:tjs>"
                "-DCAPSID_NM=${CMAKE_NM}"
                "-DCAPSID_AR=${CMAKE_AR}"
                "-DCAPSID_EXPECT_LTO=${CAPSID_RESTRICTED_AUDIT_EXPECT_LTO}"
                "-DCAPSID_EXPECT_RETAINED_TU_MARKERS=${CAPSID_RESTRICTED_RETAINED_TU_MARKERS}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuditRestrictedWorker.cmake"
        )
        set_tests_properties(worker_binary_audit PROPERTIES TIMEOUT 120)
        endif()
        set(CAPSID_RESTRICTED_AUDIT_CAN_INJECT OFF)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_OBJCOPY)
            set(CAPSID_RESTRICTED_AUDIT_CAN_INJECT ON)
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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
                "-DCAPSID_EXPECT_RETAINED_TU_MARKERS=${CAPSID_RESTRICTED_RETAINED_TU_MARKERS}"
                "-DCAPSID_TEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/restricted-worker-audit"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestRestrictedWorkerAuditNegative.cmake"
        )
        set_tests_properties(
            worker_binary_audit_negative_controls PROPERTIES TIMEOUT 120)
        endif()

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

        # These contracts require the optional Host core. Do not create a
        # plain `-lcapsid_host_core` link item in worker-only builds where
        # the CMake target does not exist.
        if(TARGET capsid_host_core)
        # WP-04 PR-06 (spec §8.1/§8.4): the WorkerExecutor ownership
        # contract — startup failure, factory/adopted lifecycle and
        # exactly-once reap. RED gate for the extraction: the header does
        # not exist until the executor is extracted from
        # single_worker_server.cc, so this target fails to compile on the
        # pre-extraction tree.
        add_executable(
            test-host-worker-executor
            tests/test_host_worker_executor.cc)
        # Debug also pulls the pool implementation from the Host core.
        # Supply its single-worker implementation without relying on DCE.
        target_sources(test-host-worker-executor PRIVATE
            src/host/single_worker_server.cc)
        target_include_directories(
            test-host-worker-executor PRIVATE
            include src "${CAPSID_GENERATED_DIR}")
        target_link_libraries(test-host-worker-executor PRIVATE
            capsid_runtime
            capsid_host_core
            Boost::system
            capsid_jansson
            OpenSSL::Crypto
            capsid_sanitizers)
        set_target_properties(test-host-worker-executor PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-worker-executor PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-worker-executor PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
                # Match the existing Beast data-plane test compilation policy.
                if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                    target_compile_options(test-host-worker-executor PRIVATE
                        -Wno-maybe-uninitialized)
                endif()
            endif()
        endif()
        add_dependencies(test-host-worker-executor capsid-worker)
        add_test(
            NAME host_worker_executor_contract
            COMMAND test-host-worker-executor $<TARGET_FILE:capsid-worker>)
        set_tests_properties(host_worker_executor_contract PROPERTIES
            TIMEOUT 40)
        add_test(NAME host_ipc_metrics_retirement
            COMMAND test-host-worker-executor $<TARGET_FILE:capsid-worker> --metrics-retirement)
        set_tests_properties(host_ipc_metrics_retirement PROPERTIES TIMEOUT 40 LABELS "host;metrics;lifecycle")
        add_test(NAME host_ipc_metrics_concurrency
            COMMAND test-host-worker-executor $<TARGET_FILE:capsid-worker> --metrics-concurrency)
        set_tests_properties(host_ipc_metrics_concurrency PROPERTIES TIMEOUT 40 LABELS "host;metrics;lifecycle")
        add_test(NAME host_ipc_metrics_bank
            COMMAND test-host-worker-executor $<TARGET_FILE:capsid-worker> --metrics-bank)
        set_tests_properties(host_ipc_metrics_bank PROPERTIES TIMEOUT 15 LABELS "host;metrics;unit")

        add_executable(test-host-ipc-metrics-format tests/test_ipc_metrics_format.cc)
        target_include_directories(test-host-ipc-metrics-format PRIVATE include src)
        target_link_libraries(test-host-ipc-metrics-format PRIVATE capsid_host_core capsid_runtime capsid_sanitizers)
        set_target_properties(test-host-ipc-metrics-format PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(test-host-ipc-metrics-format PRIVATE /W4 /WX)
            else()
                target_compile_options(test-host-ipc-metrics-format PRIVATE -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(NAME host_ipc_metrics_format COMMAND test-host-ipc-metrics-format)
        set_tests_properties(host_ipc_metrics_format PROPERTIES TIMEOUT 15 LABELS "host;metrics;format")
        if(CAPSID_HOST_TEST_NODE)
            set(CAPSID_HOST_QUALIFICATION_WPT_ARGS)
            if(CAPSID_WPT_ROOT)
                list(APPEND CAPSID_HOST_QUALIFICATION_WPT_ARGS --wpt-root "${CAPSID_WPT_ROOT}")
            endif()
            add_test(NAME host_qualification_matrix
                COMMAND "${CAPSID_HOST_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_qualification_matrix.mjs"
                    --ctest "${CMAKE_CTEST_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}" --system "${CMAKE_SYSTEM_NAME}"
                    --host $<TARGET_FILE:capsid-host> --worker $<TARGET_FILE:capsid-worker>
                    --node "${CAPSID_HOST_TEST_NODE}" --build-node "${CAPSID_NODE_EXECUTABLE}"
                    --tsan "$<BOOL:${CAPSID_ENABLE_TSAN}>"
                    ${CAPSID_HOST_QUALIFICATION_WPT_ARGS})
            set_tests_properties(host_qualification_matrix PROPERTIES TIMEOUT 60 LABELS "host;registration")
            add_test(NAME host_qualification_matrix_controls
                COMMAND "${CAPSID_HOST_TEST_NODE}" --test
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_qualification_contract.mjs")
            set_tests_properties(host_qualification_matrix_controls PROPERTIES
                TIMEOUT 30 LABELS "host;registration;contract"
                ENVIRONMENT "CAPSID_HOST_QUALIFICATION_CTEST=${CMAKE_CTEST_COMMAND}")
        endif()

        add_executable(test-host-worker-supervisor tests/test_host_worker_supervisor.cc)
        target_include_directories(test-host-worker-supervisor PRIVATE include src "${CAPSID_GENERATED_DIR}")
        target_link_libraries(test-host-worker-supervisor PRIVATE
            capsid_runtime capsid_host_core capsid_jansson OpenSSL::Crypto capsid_sanitizers)
        set_target_properties(test-host-worker-supervisor PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(test-host-worker-supervisor PRIVATE /W4 /WX)
            else()
                target_compile_options(test-host-worker-supervisor PRIVATE -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        foreach(CAPSID_SUPERVISOR_CASE IN ITEMS local local-no-external external destructor)
            string(REPLACE "-" "_" CAPSID_SUPERVISOR_SUFFIX "${CAPSID_SUPERVISOR_CASE}")
            add_test(NAME "host_worker_supervisor_${CAPSID_SUPERVISOR_SUFFIX}"
                COMMAND test-host-worker-supervisor "${CAPSID_SUPERVISOR_CASE}")
            set_tests_properties("host_worker_supervisor_${CAPSID_SUPERVISOR_SUFFIX}" PROPERTIES
                TIMEOUT 10 LABELS "host;lifecycle")
        endforeach()

        add_executable(test-host-worker-supervisor-ownership tests/test_host_worker_supervisor_ownership.cc)
        target_include_directories(test-host-worker-supervisor-ownership PRIVATE include src "${CAPSID_GENERATED_DIR}")
        target_link_libraries(test-host-worker-supervisor-ownership PRIVATE
            capsid_runtime capsid_host_core capsid_jansson OpenSSL::Crypto capsid_sanitizers)
        if(Boost_FOUND AND NOT WIN32)
            # ManagedHostOptions owns legacy activation callbacks whose
            # Debug destructors reference the single-worker transport.
            # Keep the actual dependency, including under project TSan.
            target_sources(test-host-worker-supervisor-ownership PRIVATE src/host/single_worker_server.cc)
            target_link_libraries(test-host-worker-supervisor-ownership PRIVATE Boost::system)
            if(CAPSID_STRICT_WARNINGS AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                target_compile_options(test-host-worker-supervisor-ownership PRIVATE -Wno-maybe-uninitialized)
            endif()
        endif()
        set_target_properties(test-host-worker-supervisor-ownership PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(test-host-worker-supervisor-ownership PRIVATE /W4 /WX)
            else()
                target_compile_options(test-host-worker-supervisor-ownership PRIVATE -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_dependencies(test-host-worker-supervisor-ownership capsid-worker)
        foreach(CAPSID_SUPERVISOR_CASE IN ITEMS pool-lease executor-lease stale-pool stop-local stop-external active-probe exit-notifier)
            string(REPLACE "-" "_" CAPSID_SUPERVISOR_SUFFIX "${CAPSID_SUPERVISOR_CASE}")
            add_test(NAME "host_worker_supervisor_${CAPSID_SUPERVISOR_SUFFIX}"
                COMMAND test-host-worker-supervisor-ownership $<TARGET_FILE:capsid-worker> "${CAPSID_SUPERVISOR_CASE}")
            set_tests_properties("host_worker_supervisor_${CAPSID_SUPERVISOR_SUFFIX}" PROPERTIES
                TIMEOUT 40 LABELS "host;lifecycle")
        endforeach()

        # WP-04 PR-07 (spec §8.2/§8.4): the GenerationPool fleet +
        # replacement contract — N→READY barrier, least-loaded pick,
        # N→N-1→N replacement, 0-ready 503 point, crash-budget quarantine
        # and the host-shutdown vs replacement race. Kill injection scans
        # /proc for the pool's worker children (Linux only; the kill tests
        # skip elsewhere). RED gate: generation_pool.h does not exist on
        # the PR-06 tree.
        add_executable(
            test-host-generation-pool
            tests/test_host_generation_pool.cc)
        target_include_directories(
            test-host-generation-pool PRIVATE
            include src "${CAPSID_GENERATED_DIR}")
        target_link_libraries(test-host-generation-pool PRIVATE
            capsid_runtime
            capsid_host_core
            capsid_jansson
            OpenSSL::Crypto
            capsid_sanitizers)
        set_target_properties(test-host-generation-pool PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-generation-pool PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-generation-pool PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_dependencies(test-host-generation-pool capsid-worker)
        add_test(
            NAME host_generation_pool_contract
            COMMAND test-host-generation-pool $<TARGET_FILE:capsid-worker>)
        add_test(NAME host_generation_pool_portable_contract
            COMMAND test-host-generation-pool $<TARGET_FILE:capsid-worker> --portable)
        set_tests_properties(host_generation_pool_portable_contract PROPERTIES TIMEOUT 40 LABELS "host;managed;lifecycle")
        set_tests_properties(host_generation_pool_contract PROPERTIES
            TIMEOUT 90)
        if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Windows)$")
            set_tests_properties(host_generation_pool_contract PROPERTIES SKIP_RETURN_CODE 77)
        endif()
        add_test(NAME host_generation_reclamation_idle
            COMMAND test-host-generation-pool $<TARGET_FILE:capsid-worker> --reclamation)
        set_tests_properties(host_generation_reclamation_idle PROPERTIES
            TIMEOUT 20 LABELS "host;lifecycle")
        add_test(NAME host_generation_replacement_observer_drain
            COMMAND test-host-generation-pool $<TARGET_FILE:capsid-worker> --observer-drain)
        set_tests_properties(host_generation_replacement_observer_drain PROPERTIES
            TIMEOUT 30 LABELS "host;lifecycle")
        add_test(NAME host_generation_quarantine_callback_lock
            COMMAND test-host-generation-pool $<TARGET_FILE:capsid-worker> --quarantine-lock)
        add_test(NAME host_generation_retired_exit_barrier
            COMMAND test-host-generation-pool $<TARGET_FILE:capsid-worker> --retired-exit)
        set_tests_properties(host_generation_quarantine_callback_lock host_generation_retired_exit_barrier
            PROPERTIES TIMEOUT 40 LABELS "host;lifecycle")
        if(Boost_FOUND)
            target_sources(test-host-generation-pool PRIVATE src/host/single_worker_server.cc)
            target_link_libraries(test-host-generation-pool PRIVATE Boost::system)
            if(CAPSID_STRICT_WARNINGS AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                target_compile_options(test-host-generation-pool PRIVATE -Wno-maybe-uninitialized)
            endif()

            add_executable(test-host-process-observation
                tests/test_host_process_observation.cc src/host/single_worker_server.cc)
            if(WIN32)
                target_sources(test-host-process-observation PRIVATE src/host/routing_snapshot.cc)
            endif()
            target_include_directories(test-host-process-observation PRIVATE
                include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(test-host-process-observation PRIVATE
                capsid_runtime capsid_host_core Boost::system capsid_jansson OpenSSL::Crypto
                capsid_sanitizers)
            set_target_properties(test-host-process-observation PROPERTIES
                CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
            if(CAPSID_STRICT_WARNINGS)
                if(MSVC)
                    target_compile_options(test-host-process-observation PRIVATE /W4 /WX)
                else()
                    target_compile_options(test-host-process-observation PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                        target_compile_options(test-host-process-observation PRIVATE -Wno-maybe-uninitialized)
                    endif()
                endif()
            endif()
            add_dependencies(test-host-process-observation capsid-worker)
            foreach(CAPSID_PROCESS_CASE IN ITEMS publication replacement)
                add_test(NAME "host_process_observation_${CAPSID_PROCESS_CASE}"
                    COMMAND test-host-process-observation "${CAPSID_PROCESS_CASE}" $<TARGET_FILE:capsid-worker>)
                set_tests_properties("host_process_observation_${CAPSID_PROCESS_CASE}" PROPERTIES
                    TIMEOUT 45 LABELS "host;lifecycle")
            endforeach()
        endif()

        add_executable(test-host-generation-adoption
            tests/test_host_generation_adoption.cc
            src/host/single_worker_server.cc)
        target_include_directories(test-host-generation-adoption PRIVATE
            include src "${CAPSID_GENERATED_DIR}")
        target_link_libraries(test-host-generation-adoption PRIVATE
            capsid_runtime capsid_host_core Boost::system capsid_jansson OpenSSL::Crypto
            capsid_sanitizers)
        set_target_properties(test-host-generation-adoption PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(test-host-generation-adoption PRIVATE /W4 /WX)
            else()
                target_compile_options(test-host-generation-adoption PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
                if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                    target_compile_options(test-host-generation-adoption PRIVATE
                        -Wno-maybe-uninitialized)
                endif()
            endif()
        endif()
        add_dependencies(test-host-generation-adoption capsid-worker)
        foreach(CAPSID_ADOPTION_CASE IN ITEMS
                null-first null-middle null-last count zero identity policy factory success)
            string(REPLACE "-" "_" CAPSID_ADOPTION_TEST "${CAPSID_ADOPTION_CASE}")
            add_test(NAME "host_generation_adoption_${CAPSID_ADOPTION_TEST}"
                COMMAND test-host-generation-adoption "${CAPSID_ADOPTION_CASE}"
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties("host_generation_adoption_${CAPSID_ADOPTION_TEST}" PROPERTIES
                TIMEOUT 20 LABELS "host;lifecycle")
        endforeach()
        endif()  # TARGET capsid_host_core

        # WP-05 PR-09 §9.2: RoutingSnapshot / RoutingTable + the adopt-create
        # pool entry (pre-warmed fleet, no respawn). RED gate: the snapshot
        # and create_adopted do not exist on the PR-08 tree. RoutingTable
        # stores an atomic shared_ptr; Apple libc++ cannot compile it, so
        # the contract joins the same Boost gate as the data plane itself
        # (build_host.cmake) and is not registered on Boost-less platforms
        # (spec §9.6-14: unsupported platforms SKIP, never FAIL).
        if(UNIX AND Boost_FOUND)
        add_executable(
            test-host-routing-snapshot
            tests/test_host_routing_snapshot.cc)
        target_include_directories(
            test-host-routing-snapshot PRIVATE
            include src "${CAPSID_GENERATED_DIR}")
        target_link_libraries(test-host-routing-snapshot PRIVATE
            capsid_runtime
            capsid_host_core
            capsid_jansson
            OpenSSL::Crypto
            capsid_sanitizers)
        set_target_properties(test-host-routing-snapshot PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-routing-snapshot PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-routing-snapshot PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_dependencies(test-host-routing-snapshot capsid-worker)
        add_test(
            NAME host_routing_snapshot_contract
            COMMAND test-host-routing-snapshot $<TARGET_FILE:capsid-worker>)
        set_tests_properties(host_routing_snapshot_contract PROPERTIES
            TIMEOUT 90)

        # WP-05 PR-09 §9.2: ManagedListener — the Managed data-plane
        # listener over RoutingSnapshot-pinned GenerationPools: HTTP App
        # routing, the event-sink bridge (kExit forwarded to fail pinned
        # requests), connection-ceiling RST, trusted-header gate. RED gate:
        # the listener and its test do not exist on the PR-09a tree.
        # Same Boost gate as RoutingTable above: the listener routes through
        # the atomic-shared_ptr snapshot, which Apple libc++ rejects.
        add_executable(
            test-host-managed-listener
            tests/test_host_managed_listener.cc)
        target_include_directories(
            test-host-managed-listener PRIVATE
            include src "${CAPSID_GENERATED_DIR}")
        target_link_libraries(test-host-managed-listener PRIVATE
            capsid_runtime
            capsid_host_core
            capsid_jansson
            OpenSSL::Crypto
            capsid_sanitizers)
        set_target_properties(test-host-managed-listener PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(
                    test-host-managed-listener PRIVATE /W4 /WX)
            else()
                target_compile_options(
                    test-host-managed-listener PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_dependencies(test-host-managed-listener capsid-worker)
        # The managed listener contract drives request bodies through the
        # managed data plane; on macOS the POST echo stalls (504) — the
        # managed data plane is Linux-only, like the managed executable
        # suite, so the contract registers only there.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(
            NAME host_managed_listener_contract
            COMMAND test-host-managed-listener $<TARGET_FILE:capsid-worker>)
        set_tests_properties(host_managed_listener_contract PROPERTIES
            TIMEOUT 90)
        endif()
        endif()  # UNIX AND Boost_FOUND — RoutingTable contract tests

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
        add_test(NAME worker_framework_concurrent_diagnostics
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_framework_concurrent_diagnostics.mjs"
                --driver $<TARGET_FILE:test-hono-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/framework-concurrent-failure.js")
        set_tests_properties(worker_framework_concurrent_diagnostics PROPERTIES
            TIMEOUT 30 LABELS "framework;lifecycle;diagnostics")
        add_test(NAME worker_quickjs_call_regexp
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_quickjs_call_regexp.mjs"
                --driver $<TARGET_FILE:test-hono-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/quickjs-call-regexp.mjs")
        set_tests_properties(worker_quickjs_call_regexp PROPERTIES
            TIMEOUT 45 LABELS "worker;engine;conformance")
        foreach(stack_mode default 262144)
            add_test(NAME worker_js_stack_limits_${stack_mode}
                COMMAND "${CAPSID_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_worker_stack_limits.mjs"
                    --driver $<TARGET_FILE:test-hono-worker-driver>
                    --worker $<TARGET_FILE:capsid-worker>
                    --bundle "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/js-stack-limits.js"
                    --stack-bytes ${stack_mode})
            set_tests_properties(worker_js_stack_limits_${stack_mode} PROPERTIES
                TIMEOUT 30 LABELS "worker;resource;lifecycle")
        endforeach()
        # Bytecode value helpers keep ordinary and instrumented Debug Hono
        # on the public default, also enforced by the overflow/recovery tests.
        set(CAPSID_HONO_TEST_STACK_BYTES 1048576)
        add_test(
            NAME worker_hono_compatibility
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/hono/differential.mjs"
                --driver $<TARGET_FILE:test-hono-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_HONO_DEFAULT_FIXTURE}"
                --js-stack-bytes ${CAPSID_HONO_TEST_STACK_BYTES}
        )
        add_test(
            NAME worker_hono_entry_object
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/hono/differential.mjs"
                --driver $<TARGET_FILE:test-hono-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_HONO_OBJECT_FIXTURE}"
                --smoke true
                --js-stack-bytes ${CAPSID_HONO_TEST_STACK_BYTES}
        )
        add_test(
            NAME worker_hono_entry_named
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/hono/differential.mjs"
                --driver $<TARGET_FILE:test-hono-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_HONO_NAMED_FIXTURE}"
                --smoke true
                --js-stack-bytes ${CAPSID_HONO_TEST_STACK_BYTES}
        )
        add_test(
            NAME worker_hono_lifecycle
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/hono/lifecycle.mjs"
                --driver $<TARGET_FILE:test-hono-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_HONO_DEFAULT_FIXTURE}"
                --js-stack-bytes ${CAPSID_HONO_TEST_STACK_BYTES}
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

        add_executable(
            test-elysia-worker-driver
            tests/test_framework_worker_driver.cc
        )
        target_link_libraries(
            test-elysia-worker-driver PRIVATE capsid_runtime
        )
        add_dependencies(
            test-elysia-worker-driver
            test-elysia-DEFAULT-fixture
            test-elysia-NAMED-fixture
        )
        add_test(
            NAME worker_elysia_compatibility
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/elysia/differential.mjs"
                --driver $<TARGET_FILE:test-elysia-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_ELYSIA_DEFAULT_FIXTURE}"
        )
        add_test(
            NAME worker_elysia_entry_named
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/elysia/differential.mjs"
                --driver $<TARGET_FILE:test-elysia-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_ELYSIA_NAMED_FIXTURE}"
                --smoke true
        )
        add_test(
            NAME worker_elysia_lifecycle
            COMMAND "${CAPSID_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/elysia/lifecycle.mjs"
                --driver $<TARGET_FILE:test-elysia-worker-driver>
                --worker $<TARGET_FILE:capsid-worker>
                --bundle "${CAPSID_ELYSIA_DEFAULT_FIXTURE}"
        )
        set_tests_properties(
            worker_elysia_compatibility
            PROPERTIES TIMEOUT 180 LABELS "framework;elysia;differential"
        )
        set_tests_properties(
            worker_elysia_entry_named
            PROPERTIES TIMEOUT 30 LABELS "framework;elysia;entry"
        )
        set_tests_properties(
            worker_elysia_lifecycle
            PROPERTIES TIMEOUT 60 LABELS "framework;elysia;lifecycle"
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

        # WP-02 §6.5 QuickJS job-context hook unit gate. RED until
        # 0012-capsid-async-context.patch exists (compile error: missing
        # JSJobContextHooks / JS_SetJobContextHooks); GREEN after.
        add_executable(
            test-quickjs-job-context
            tests/test_quickjs_job_context.cc
        )
        target_link_libraries(test-quickjs-job-context PRIVATE tjs)
        add_test(
            NAME quickjs_job_context_hooks
            COMMAND test-quickjs-job-context
        )
        set_tests_properties(quickjs_job_context_hooks
            PROPERTIES TIMEOUT 20)

        add_executable(test-quickjs-iterator-storage tests/test_quickjs_iterator_storage.cc)
        target_link_libraries(test-quickjs-iterator-storage PRIVATE tjs)
        target_compile_features(test-quickjs-iterator-storage PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(test-quickjs-iterator-storage PRIVATE /W4 /WX)
        else()
            target_compile_options(test-quickjs-iterator-storage PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_test(NAME quickjs_iterator_storage COMMAND test-quickjs-iterator-storage)
        set_tests_properties(quickjs_iterator_storage
            PROPERTIES TIMEOUT 20 LABELS "worker;quickjs;iterators;lifecycle;allocation")

        add_executable(test-quickjs-async-gc tests/test_quickjs_async_gc.cc)
        target_link_libraries(test-quickjs-async-gc PRIVATE tjs)
        add_test(NAME quickjs_async_closure_gc COMMAND test-quickjs-async-gc)
        add_executable(test-quickjs-call-dispatch tests/test_quickjs_call_dispatch.cc)
        target_link_libraries(test-quickjs-call-dispatch PRIVATE tjs)
        target_compile_features(test-quickjs-call-dispatch PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(test-quickjs-call-dispatch PRIVATE /W4 /WX)
        else()
            target_compile_options(test-quickjs-call-dispatch PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_test(NAME quickjs_call_dispatch COMMAND test-quickjs-call-dispatch)
        set_tests_properties(quickjs_call_dispatch PROPERTIES TIMEOUT 10 LABELS "worker;engine;interrupt")
        set_tests_properties(quickjs_async_closure_gc
            PROPERTIES TIMEOUT 20 LABELS "worker;quickjs;lifecycle")

        add_executable(test-quickjs-bigint-width tests/test_quickjs_bigint_width.cc)
        target_link_libraries(test-quickjs-bigint-width PRIVATE tjs)
        target_compile_features(test-quickjs-bigint-width PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(test-quickjs-bigint-width PRIVATE /W4 /WX)
        else()
            target_compile_options(test-quickjs-bigint-width PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_test(NAME quickjs_bigint_width COMMAND test-quickjs-bigint-width)
        set_tests_properties(quickjs_bigint_width
            PROPERTIES TIMEOUT 20 LABELS "worker;quickjs;compatibility")

        add_executable(test-quickjs-stream-observers tests/test_quickjs_stream_observers.cc)
        target_link_libraries(test-quickjs-stream-observers PRIVATE tjs)
        target_compile_features(test-quickjs-stream-observers PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(test-quickjs-stream-observers PRIVATE /W4 /WX)
        else()
            target_compile_options(test-quickjs-stream-observers PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_test(NAME quickjs_stream_observers COMMAND test-quickjs-stream-observers)
        set_tests_properties(quickjs_stream_observers PROPERTIES TIMEOUT 20 LABELS "worker;streams;lifecycle")

        add_executable(test-quickjs-bytecode-values tests/test_quickjs_bytecode_values.cc)
        target_link_libraries(test-quickjs-bytecode-values PRIVATE tjs)
        target_compile_features(test-quickjs-bytecode-values PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(test-quickjs-bytecode-values PRIVATE /W4 /WX)
        else()
            target_compile_options(test-quickjs-bytecode-values PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_test(NAME quickjs_bytecode_values COMMAND test-quickjs-bytecode-values)
        set_tests_properties(quickjs_bytecode_values
            PROPERTIES TIMEOUT 20 LABELS "worker;quickjs;ownership;exceptions")

        add_executable(test-quickjs-arraybuffer-alias tests/test_quickjs_arraybuffer_alias.cc)
        target_link_libraries(test-quickjs-arraybuffer-alias PRIVATE tjs)
        target_compile_features(test-quickjs-arraybuffer-alias PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(test-quickjs-arraybuffer-alias PRIVATE /W4 /WX)
        else()
            target_compile_options(test-quickjs-arraybuffer-alias PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_test(NAME quickjs_arraybuffer_alias COMMAND test-quickjs-arraybuffer-alias)
        set_tests_properties(quickjs_arraybuffer_alias
            PROPERTIES TIMEOUT 20 LABELS "worker;quickjs;streams;lifecycle")

        # Binding v1 §7.5: two TJS runtimes on one Capsid-owned loop
        # (0016-capsid-shared-loop.patch). RED until the patch exists.
        add_executable(
            test-tjs-shared-loop
            tests/test_tjs_shared_loop.cc
        )
        target_link_libraries(test-tjs-shared-loop PRIVATE tjs)
        add_test(
            NAME tjs_shared_loop
            COMMAND test-tjs-shared-loop
        )
        set_tests_properties(tjs_shared_loop PROPERTIES TIMEOUT 20)

        add_executable(test-http-gzip tests/test_http_gzip.c)
        set_target_properties(test-http-gzip PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        target_link_libraries(test-http-gzip PRIVATE tjs)
        add_test(NAME http_gzip_parser COMMAND test-http-gzip)
        set_tests_properties(http_gzip_parser PROPERTIES
            TIMEOUT 60 LABELS "worker;fetch;compression;reference-gate")

        add_executable(test-platform-digest tests/test_platform_digest.c)
        set_target_properties(test-platform-digest PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        target_link_libraries(test-platform-digest PRIVATE tjs)
        add_test(NAME platform_digest_backend COMMAND test-platform-digest)
        set_tests_properties(platform_digest_backend PROPERTIES
            TIMEOUT 30 LABELS "worker;crypto;compatibility")

        add_executable(test-tjs-fetch-abort tests/test_tjs_fetch_abort.cc)
        # Exercise real positive short writes through the native LWS client.
        # --wrap belongs only to the test executable and is a GNU ELF feature.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
            add_executable(test-tjs-http-short-write tests/test_tjs_http_short_write.cc)
            target_link_libraries(test-tjs-http-short-write PRIVATE tjs capsid_runtime Threads::Threads)
            target_include_directories(test-tjs-http-short-write PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CAPSID_TXIKI_OVERLAY}/src")
            target_link_options(test-tjs-http-short-write PRIVATE "LINKER:--wrap=lws_ssl_capable_write")
            if(CAPSID_STRICT_WARNINGS)
                target_compile_options(test-tjs-http-short-write PRIVATE -Wall -Wextra -Wpedantic -Werror)
            endif()
            foreach(CAPSID_SHORT_WRITE_BYTES IN ITEMS 64 8192)
                foreach(CAPSID_SHORT_WRITE_MODE IN ITEMS normal short)
                    add_test(NAME tjs_http_${CAPSID_SHORT_WRITE_MODE}_write_${CAPSID_SHORT_WRITE_BYTES}
                        COMMAND test-tjs-http-short-write ${CAPSID_SHORT_WRITE_MODE} ${CAPSID_SHORT_WRITE_BYTES})
                    set_tests_properties(tjs_http_${CAPSID_SHORT_WRITE_MODE}_write_${CAPSID_SHORT_WRITE_BYTES}
                        PROPERTIES TIMEOUT 15 LABELS "worker;fetch;native;short-write")
                endforeach()
            endforeach()
        endif()
        target_link_libraries(test-tjs-fetch-abort
            PRIVATE tjs capsid_runtime Threads::Threads)
        target_include_directories(test-tjs-fetch-abort
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CAPSID_TXIKI_OVERLAY}/src")
        add_test(NAME tjs_fetch_abort_pending_handshake
            COMMAND test-tjs-fetch-abort)
        set_tests_properties(tjs_fetch_abort_pending_handshake
            PROPERTIES TIMEOUT 15 LABELS "worker;fetch;lifecycle")
        add_test(NAME tjs_fetch_context_cancel_pending_handshake
            COMMAND test-tjs-fetch-abort --context-cancel)
        set_tests_properties(tjs_fetch_context_cancel_pending_handshake
            PROPERTIES TIMEOUT 15 LABELS "worker;fetch;lifecycle")
        add_test(NAME tjs_fetch_context_cancel_dns
            COMMAND test-tjs-fetch-abort --dns-context-cancel)
        set_tests_properties(tjs_fetch_context_cancel_dns
            PROPERTIES TIMEOUT 15 LABELS "worker;fetch;lifecycle")
        foreach(context_kind IN ITEMS async binding unowned)
            add_test(NAME tjs_fetch_policy_${context_kind}_context
                COMMAND test-tjs-fetch-abort --policy-${context_kind}-context)
            set_tests_properties(tjs_fetch_policy_${context_kind}_context
                PROPERTIES TIMEOUT 15 LABELS "worker;fetch;lifecycle")
        endforeach()

        # Binding v1 §7.6: the neutral-value structured clone between the
        # User and Binding Runtimes.
        add_executable(
            test-binding-rpc
            tests/test_binding_rpc.cc
            src/binding_rpc.cc
        )
        target_include_directories(test-binding-rpc PRIVATE
            include src
            "${CAPSID_TXIKI_OVERLAY}/src"
            "${CAPSID_TXIKI_OVERLAY}/deps/quickjs"
            "${CAPSID_TXIKI_OVERLAY}/deps/libuv/include")
        target_link_libraries(test-binding-rpc PRIVATE tjs)
        add_test(
            NAME binding_rpc
            COMMAND test-binding-rpc
        )
        set_tests_properties(binding_rpc PROPERTIES TIMEOUT 20)

        # WP-00/PR-01 RED gates. All three worker tests are expected to
        # FAIL on the pre-fix bridge (identity collapse P0-1, request
        # context loss P0-2, terminal continuation survival P0-3); the
        # script tests fail on the pre-fix distribution (P0-6) and identity
        # truncation (P0-7).

        add_executable(
            test-worker-request-id-boundaries
            tests/test_worker_request_id_boundaries.cc
        )
        target_link_libraries(
            test-worker-request-id-boundaries PRIVATE capsid_runtime
        )
        add_test(
            NAME worker_request_id_boundaries
            COMMAND test-worker-request-id-boundaries
                $<TARGET_FILE:capsid-worker>
        )
        set_tests_properties(
            worker_request_id_boundaries PROPERTIES TIMEOUT 20)

        add_executable(
            test-worker-async-request-context
            tests/test_worker_async_request_context.cc
        )
        target_link_libraries(
            test-worker-async-request-context PRIVATE capsid_runtime
        )
        add_test(
            NAME worker_async_request_context
            COMMAND test-worker-async-request-context
                $<TARGET_FILE:capsid-worker>
        )
        set_tests_properties(
            worker_async_request_context PROPERTIES TIMEOUT 20)

        add_executable(
            test-worker-terminal-continuation
            tests/test_worker_terminal_continuation.cc
        )
        target_link_libraries(
            test-worker-terminal-continuation PRIVATE capsid_runtime
        )
        add_test(
            NAME worker_terminal_continuation
            COMMAND test-worker-terminal-continuation
                $<TARGET_FILE:capsid-worker>
        )
        set_tests_properties(
            worker_terminal_continuation PROPERTIES TIMEOUT 30)

        # Build identity matrix (P0-7, hardened by WP-07 per spec §11.4):
        # every controlled build difference must change the build ID, a
        # real bytecode-ABI difference must change the compatibility ID,
        # and identical configures must not. Each variant is a fresh
        # configure; the test requires a clean worktree because the
        # Release variants fail closed on dirtiness (spec §11.3).
        add_test(
            NAME worker_build_identity_matrix
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DCAPSID_CMAKE_COMMAND=${CMAKE_COMMAND}"
                "-DCAPSID_MATRIX_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/identity-matrix"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_build_identity_matrix.cmake"
        )
        set_tests_properties(
            worker_build_identity_matrix PROPERTIES TIMEOUT 300)

        # Install tree and package contents (P0-6): the frozen runtime
        # manifest must exist in a fresh install prefix and inside the
        # package archive, and the package target must not be taken over by
        # a third-party CPack configuration.
        # capsid-host is skipped when system Boost is missing, so the
        # manifest expectation follows target existence, not the option.
        # The packaging gates themselves require the worker pair: in a
        # CAPSID_BUILD_WORKER=OFF matrix (host-only) they are not registered.
        if(TARGET capsid-worker)
        if(TARGET capsid-host)
            set(CAPSID_HOST_TARGET_PRESENT ON)
        else()
            set(CAPSID_HOST_TARGET_PRESENT OFF)
        endif()

        add_test(NAME package_freshness_controls
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DCAPSID_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-freshness-controls"
                "-DCAPSID_TEST_GENERATOR=${CMAKE_GENERATOR}"
                "-DCAPSID_TEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_package_freshness_controls.cmake")
        set_tests_properties(package_freshness_controls PROPERTIES
            TIMEOUT 120 LABELS "package;contract")

        add_test(NAME package_reproducibility_controls
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DCAPSID_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-comparison-controls"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_package_repro_controls.cmake")
        set_tests_properties(package_reproducibility_controls PROPERTIES
            TIMEOUT 30 LABELS "package;contract")

        add_test(
            NAME worker_install_tree
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DCAPSID_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/install-tree-prefix"
                -DCAPSID_BUILD_HOST=${CAPSID_BUILD_HOST}
                -DCAPSID_HOST_TARGET=${CAPSID_HOST_TARGET_PRESENT}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install_tree.cmake"
        )
        set_tests_properties(worker_install_tree PROPERTIES TIMEOUT 300)

        add_test(
            NAME worker_package_contents
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DCAPSID_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-contents-work"
                -DCAPSID_BUILD_HOST=${CAPSID_BUILD_HOST}
                -DCAPSID_HOST_TARGET=${CAPSID_HOST_TARGET_PRESENT}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_package_contents.cmake"
        )
        # The package target rebuilds the whole tree when build products are
        # stale (e.g. a cold container run right after a reconfigure), so
        # budget like the reproducibility gate — not a tight incremental
        # estimate.
        # Packaging can rebuild the binaries under test. Never run it beside
        # tests consuming those binaries, or smoke an archive after setup failed.
        set_tests_properties(worker_package_contents PROPERTIES
            TIMEOUT 1800 RUN_SERIAL TRUE FIXTURES_SETUP capsid_package)

        add_test(NAME worker_package_metadata_controls
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DCAPSID_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DCAPSID_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-metadata-controls"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_package_metadata_controls.cmake")
        set_tests_properties(worker_package_metadata_controls PROPERTIES
            TIMEOUT 120 LABELS "package;contract"
            FIXTURES_REQUIRED capsid_package)

        # §12.4: consume the archive as a customer would — extract into an
        # empty directory, compile C/C++ samples against the packaged
        # headers+library, run worker round trips, drive the packaged Host
        # through the node driver, and scan for build-root paths, secrets
        # and undeclared dynamic dependencies.
        add_test(
            NAME worker_package_smoke
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DCAPSID_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-smoke-work"
                "-DCAPSID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DCAPSID_C_COMPILER=${CMAKE_C_COMPILER}"
                "-DCAPSID_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DCAPSID_SMOKE_SAMPLE_C=${CMAKE_CURRENT_SOURCE_DIR}/tests/package_smoke_sample.c"
                "-DCAPSID_SMOKE_SAMPLE_CC=${CMAKE_CURRENT_SOURCE_DIR}/tests/package_smoke_sample.cc"
                "-DCAPSID_SMOKE_BUILD_TYPE=$<CONFIG>"
                "-DCAPSID_SMOKE_GENERATOR=${CMAKE_GENERATOR}"
                "-DCAPSID_SMOKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                -DCAPSID_SMOKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}
                -DCAPSID_HOST_TARGET=${CAPSID_HOST_TARGET_PRESENT}
                "-DCAPSID_NODE_EXECUTABLE=${CAPSID_NODE_EXECUTABLE}"
                "-DCAPSID_HOST_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/host-single-worker.js"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_smoke.cmake"
        )
        set_tests_properties(worker_package_smoke PROPERTIES
            TIMEOUT 600
            DEPENDS worker_package_contents
            FIXTURES_REQUIRED capsid_package)

        add_test(NAME worker_package_consumer_controls
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DCAPSID_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/pc"
                "-DCAPSID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DCAPSID_C_COMPILER=${CMAKE_C_COMPILER}"
                "-DCAPSID_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DCAPSID_SMOKE_BUILD_TYPE=$<CONFIG>"
                "-DCAPSID_SMOKE_GENERATOR=${CMAKE_GENERATOR}"
                "-DCAPSID_SMOKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_package_consumer_controls.cmake")
        set_tests_properties(worker_package_consumer_controls PROPERTIES
            TIMEOUT 180 FIXTURES_REQUIRED capsid_package)

        # §12.3: reproducibility gate. Two fresh builds from the same inputs
        # must agree on the file name list, the identity records, the SBOM
        # identity fields and every deterministic text file; binary hash
        # differences (toolchain not yet bit-reproducible) are recorded in
        # repro-differences.txt. The gate mirrors this build's feature flags
        # by parsing them from the baseline build-info.txt, and reuses this
        # build's generator, parallelism and toolchain prefix (CI: the
        # pinned OpenSSL 3.5 install).
        if(DEFINED CMAKE_BUILD_PARALLEL_LEVEL AND
           NOT CMAKE_BUILD_PARALLEL_LEVEL STREQUAL "")
            set(CAPSID_REPRO_PARALLEL "${CMAKE_BUILD_PARALLEL_LEVEL}")
        else()
            set(CAPSID_REPRO_PARALLEL 2)
        endif()
        add_test(
            NAME worker_package_reproducibility
            COMMAND "${CMAKE_COMMAND}"
                "-DCAPSID_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DCAPSID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DCAPSID_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-repro-work"
                "-DCAPSID_CMAKE_COMMAND=${CMAKE_COMMAND}"
                "-DCAPSID_REPRO_ESBUILD_EXECUTABLE=${CAPSID_ESBUILD_EXECUTABLE}"
                "-DCAPSID_REPRO_GENERATOR=${CMAKE_GENERATOR}"
                "-DCAPSID_REPRO_PARALLEL=${CAPSID_REPRO_PARALLEL}"
                "-DCAPSID_REPRO_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
                "-DCAPSID_REPRO_OPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}"
                "-DCAPSID_REPRO_BOOST_DIR=${Boost_DIR}"
                "-DCAPSID_REPRO_BOOST_INCLUDE_DIR=${Boost_INCLUDE_DIR}"
                "-DCAPSID_REPRO_BOOST_SYSTEM_LIBRARY=${Boost_SYSTEM_LIBRARY_RELEASE}"
                "-DCAPSID_C_COMPILER=${CMAKE_C_COMPILER}"
                "-DCAPSID_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                -DCAPSID_STRICT_WARNINGS=${CAPSID_STRICT_WARNINGS}
                -DCAPSID_ENABLE_FFI_CAPABILITY=${CAPSID_ENABLE_FFI_CAPABILITY}
                -DCAPSID_ENABLE_RAW_SOCKET_CAPABILITY=${CAPSID_ENABLE_RAW_SOCKET_CAPABILITY}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reproducibility.cmake"
        )
        set_tests_properties(worker_package_reproducibility PROPERTIES
            TIMEOUT 1800
            DEPENDS worker_package_smoke)
        endif()

        add_executable(
            test-sandbox
            tests/test_sandbox.cc
            src/sandbox.cc
        )
        target_include_directories(test-sandbox PRIVATE include src)
        target_link_libraries(test-sandbox PRIVATE capsid_sanitizers)
        add_test(NAME worker_sandbox_enforcement COMMAND test-sandbox)
        set_tests_properties(worker_sandbox_enforcement PROPERTIES
            TIMEOUT 10
            LABELS "sandbox")

        # Binding v1 §7.9: the privileged profile conformance probes. Skip
        # 77 (unprivileged / non-Linux) is a failure in the Hosted
        # Validity workflow.
        set(CAPSID_BINDING_SANDBOX_DIR
            "${CAPSID_SANDBOX_TEST_DATA_DIR}/capsid-binding-sandbox-dir")
        set(CAPSID_BINDING_SANDBOX_ALLOWED_FILE
            "${CAPSID_BINDING_SANDBOX_DIR}/allowed.db")
        set(CAPSID_BINDING_SANDBOX_DENIED_FILE
            "${CAPSID_SANDBOX_TEST_DATA_DIR}/capsid-binding-denied.db")
        file(MAKE_DIRECTORY "${CAPSID_BINDING_SANDBOX_DIR}")
        file(WRITE "${CAPSID_BINDING_SANDBOX_ALLOWED_FILE}" "allowed\n")
        file(WRITE "${CAPSID_BINDING_SANDBOX_DENIED_FILE}" "denied\n")
        add_test(NAME worker_binding_sandbox_write
            COMMAND test-sandbox --binding-write
                "${CAPSID_BINDING_SANDBOX_DIR}")
        set_tests_properties(worker_binding_sandbox_write PROPERTIES
            TIMEOUT 15
            LABELS "sandbox"
            SKIP_RETURN_CODE 77)
        add_test(NAME worker_binding_sandbox_read
            COMMAND test-sandbox --binding-read
                "${CAPSID_BINDING_SANDBOX_ALLOWED_FILE}"
                "${CAPSID_BINDING_SANDBOX_DENIED_FILE}")
        set_tests_properties(worker_binding_sandbox_read PROPERTIES
            TIMEOUT 15
            LABELS "sandbox"
            SKIP_RETURN_CODE 77)
        add_test(NAME worker_binding_sandbox_watch
            COMMAND test-sandbox --binding-watch
                "${CAPSID_BINDING_SANDBOX_ALLOWED_FILE}")
        set_tests_properties(worker_binding_sandbox_watch PROPERTIES
            TIMEOUT 15
            LABELS "sandbox"
            SKIP_RETURN_CODE 77)
        add_test(NAME worker_binding_sandbox_sqlite
            COMMAND test-sandbox --binding-sqlite
                "${CAPSID_BINDING_SANDBOX_DIR}"
                "${CAPSID_BINDING_SANDBOX_ALLOWED_FILE}"
                "${CAPSID_BINDING_SANDBOX_DENIED_FILE}")
        set_tests_properties(worker_binding_sandbox_sqlite PROPERTIES
            TIMEOUT 15
            LABELS "sandbox"
            SKIP_RETURN_CODE 77)
        add_test(NAME worker_binding_sandbox_network
            COMMAND test-sandbox --binding-network)
        set_tests_properties(worker_binding_sandbox_network PROPERTIES
            TIMEOUT 15
            LABELS "sandbox"
            SKIP_RETURN_CODE 77)
        add_test(NAME worker_binding_sandbox_wasi
            COMMAND test-sandbox --binding-wasi
                "${CAPSID_BINDING_SANDBOX_ALLOWED_FILE}")
        set_tests_properties(worker_binding_sandbox_wasi PROPERTIES
            TIMEOUT 15
            LABELS "sandbox"
            SKIP_RETURN_CODE 77)
        add_test(NAME worker_binding_sandbox_union
            COMMAND test-sandbox --binding-union
                "${CAPSID_BINDING_SANDBOX_DIR}"
                "${CAPSID_BINDING_SANDBOX_ALLOWED_FILE}"
                "${CAPSID_BINDING_SANDBOX_DENIED_FILE}")
        set_tests_properties(worker_binding_sandbox_union PROPERTIES
            TIMEOUT 15
            LABELS "sandbox"
            SKIP_RETURN_CODE 77)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_executable(
                test-sandbox-fd-hygiene
                tests/test_sandbox_fd_hygiene.cc
            )
            target_link_libraries(
                test-sandbox-fd-hygiene PRIVATE capsid_runtime
            )
            add_test(NAME worker_binding_sandbox_setup_error
                COMMAND test-sandbox --binding-setup-error)
            set_tests_properties(worker_binding_sandbox_setup_error PROPERTIES
                TIMEOUT 10 LABELS "sandbox" SKIP_RETURN_CODE 77)
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
                NAME worker_process_environment_hygiene
                COMMAND test-sandbox-fd-hygiene
                    $<TARGET_FILE:capsid-worker> --non-strict
            )
            set_tests_properties(worker_process_environment_hygiene PROPERTIES
                TIMEOUT 10 LABELS "host;process")

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

        if(UNIX)
        add_executable(
            test-worker-zero-binding
            tests/test_worker_zero_binding.cc
            src/protocol.cc
        )
        target_include_directories(
            test-worker-zero-binding
            PRIVATE include src "${CAPSID_GENERATED_DIR}")
        target_link_libraries(
            test-worker-zero-binding
            PRIVATE capsid_runtime capsid_sanitizers)
        add_dependencies(
            test-worker-zero-binding
            capsid-worker
            test-p0-fixture
            test-binding-import-fixture
            test-binding-call-fixture
            test-binding-owner-fixture
            test-binding-lifecycle-fixture)
        add_test(
            NAME worker_zero_binding_regression
            COMMAND test-worker-zero-binding
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_P0_FIXTURE}"
                "${CAPSID_BINDING_IMPORT_FIXTURE}"
                "${CAPSID_BINDING_CALL_FIXTURE}"
                "${CAPSID_BINDING_OWNER_FIXTURE}"
                "${CAPSID_BINDING_LIFECYCLE_FIXTURE}"
        )
        set_tests_properties(
            worker_zero_binding_regression
            PROPERTIES TIMEOUT 60
        )
        endif()  # UNIX — zero-binding regression uses POSIX spawn/socket

        if(WIN32)
            add_executable(test-windows-worker-environment
                tests/test_windows_worker_environment.cc)
            target_include_directories(test-windows-worker-environment PRIVATE include src)
            target_link_libraries(test-windows-worker-environment PRIVATE capsid_runtime capsid_sanitizers)
            if(CAPSID_STRICT_WARNINGS)
                target_compile_options(test-windows-worker-environment PRIVATE /W4 /WX)
            endif()
            add_dependencies(test-windows-worker-environment capsid-worker)
            add_test(NAME worker_windows_environment
                COMMAND test-windows-worker-environment $<TARGET_FILE:capsid-worker>)
            set_tests_properties(worker_windows_environment PROPERTIES
                TIMEOUT 30 LABELS "worker;platform;environment")
            add_executable(
                test-worker-binding-windows
                tests/test_worker_binding_windows.cc
            )
            target_include_directories(
                test-worker-binding-windows
                PRIVATE include src "${CAPSID_GENERATED_DIR}")
            target_link_libraries(
                test-worker-binding-windows
                PRIVATE capsid_runtime capsid_sanitizers)
            add_dependencies(
                test-worker-binding-windows
                capsid-worker
                test-binding-call-fixture)
            add_test(
                NAME worker_binding_windows_smoke
                COMMAND test-worker-binding-windows
                    $<TARGET_FILE:capsid-worker>
                    "${CAPSID_BINDING_CALL_FIXTURE}"
            )
            set_tests_properties(
                worker_binding_windows_smoke
                PROPERTIES TIMEOUT 30
            )
        endif()

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
        # Immediate cancellation and cancellation after an observed idle
        # receive timeout each retain their existing bounded stage deadlines.
        set_tests_properties(worker_fetch_cancel_lifecycle PROPERTIES TIMEOUT 30)

        set(CAPSID_QUEUE_SATURATION_FIXTURE
            "${CAPSID_GENERATED_DIR}/test-queue-saturation.js")
        add_custom_command(
            OUTPUT "${CAPSID_QUEUE_SATURATION_FIXTURE}"
            COMMAND "${CAPSID_ESBUILD}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/queue-saturation.js"
                --bundle
                --target=esnext
                --platform=neutral
                --format=esm
                "--outfile=${CAPSID_QUEUE_SATURATION_FIXTURE}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/queue-saturation.js"
            VERBATIM
        )
        add_custom_target(test-queue-saturation-fixture
            DEPENDS "${CAPSID_QUEUE_SATURATION_FIXTURE}")

        get_target_property(CAPSID_TIMEOUT_WRITER_SOURCES capsid-worker SOURCES)
        add_executable(test-worker-timeout-writer-fixture ${CAPSID_TIMEOUT_WRITER_SOURCES}
            tests/worker_timeout_writer_fixture.cc)
        foreach(CAPSID_TIMEOUT_WRITER_PROPERTY IN ITEMS INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS
                CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS INTERPROCEDURAL_OPTIMIZATION)
            get_target_property(CAPSID_TIMEOUT_WRITER_VALUE capsid-worker "${CAPSID_TIMEOUT_WRITER_PROPERTY}")
            if(NOT CAPSID_TIMEOUT_WRITER_VALUE STREQUAL "CAPSID_TIMEOUT_WRITER_VALUE-NOTFOUND")
                set_property(TARGET test-worker-timeout-writer-fixture PROPERTY
                    "${CAPSID_TIMEOUT_WRITER_PROPERTY}" "${CAPSID_TIMEOUT_WRITER_VALUE}")
            endif()
        endforeach()
        target_compile_definitions(test-worker-timeout-writer-fixture PRIVATE CAPSID_WORKER_CUSTOM_ENTRY=1)
        target_link_libraries(test-worker-timeout-writer-fixture PRIVATE tjs capsid_sanitizers)
        add_dependencies(test-worker-timeout-writer-fixture capsid-worker)
        add_executable(
            test-response-queue-saturation
            tests/test_response_queue_saturation.cc
        )
        target_link_libraries(
            test-response-queue-saturation
            PRIVATE capsid_runtime Threads::Threads
        )
        add_dependencies(
            test-response-queue-saturation
            test-queue-saturation-fixture test-worker-timeout-writer-fixture
        )
        add_test(
            NAME worker_response_queue_saturation
            COMMAND test-response-queue-saturation
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_QUEUE_SATURATION_FIXTURE}"
        )
        set_tests_properties(
            worker_response_queue_saturation PROPERTIES TIMEOUT 90
        )
        add_test(NAME worker_response_timeout_queue_pressure
            COMMAND test-response-queue-saturation
                $<TARGET_FILE:test-worker-timeout-writer-fixture>
                "${CAPSID_QUEUE_SATURATION_FIXTURE}" 18)
        set_tests_properties(worker_response_timeout_queue_pressure PROPERTIES
            LABELS "worker;backpressure;lifecycle" TIMEOUT 30)

        add_executable(
            test-direct-fetch-matrix
            tests/test_direct_fetch_matrix.cc
        )
        target_link_libraries(
            test-direct-fetch-matrix
            PRIVATE capsid_runtime Threads::Threads websockets
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
        add_test(
            NAME worker_direct_fetch_bodyless
            COMMAND test-direct-fetch-matrix
                $<TARGET_FILE:capsid-worker>
                "${CAPSID_DIRECT_FETCH_MATRIX_FIXTURE}"
                --bodyless
        )
        set_tests_properties(worker_direct_fetch_bodyless PROPERTIES
            TIMEOUT 40 LABELS "worker;fetch;lifecycle")
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
        file(MAKE_DIRECTORY "${CAPSID_SANDBOX_TEST_DATA_DIR}")
        configure_file(
            "${CAPSID_MBEDTLS_TEST_DATA}/test-ca-sha256.crt"
            "${CAPSID_SANDBOX_TEST_DATA_DIR}/test-ca-sha256.crt" COPYONLY)
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
        find_program(CAPSID_TEST_OPENSSL_EXECUTABLE NAMES openssl)
        if(CAPSID_TEST_OPENSSL_EXECUTABLE)
            add_test(
                NAME worker_direct_fetch_https_tls12_rsa_pss
                COMMAND test-direct-fetch-tls
                    $<TARGET_FILE:capsid-worker>
                    "${CAPSID_DIRECT_FETCH_TLS_FIXTURE}"
                    "${CAPSID_MBEDTLS_TEST_DATA}/server2-sha256.crt"
                    "${CAPSID_MBEDTLS_TEST_DATA}/server2.key"
                    "${CAPSID_MBEDTLS_TEST_DATA}/test-ca-sha256.crt"
                    --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}"
            )
            set_tests_properties(
                worker_direct_fetch_https_tls12_rsa_pss
                PROPERTIES TIMEOUT 30 LABELS "tls;rsa-pss"
            )
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                add_test(
                    NAME worker_strict_sandbox_https_tls12_rsa_pss
                    COMMAND test-direct-fetch-tls
                        $<TARGET_FILE:capsid-worker>
                        "${CAPSID_DIRECT_FETCH_TLS_FIXTURE}"
                        "${CAPSID_MBEDTLS_TEST_DATA}/server2-sha256.crt"
                        "${CAPSID_MBEDTLS_TEST_DATA}/server2.key"
                        "${CAPSID_SANDBOX_TEST_DATA_DIR}/test-ca-sha256.crt"
                        --strict
                        --openssl "${CAPSID_TEST_OPENSSL_EXECUTABLE}"
                )
                set_tests_properties(
                    worker_strict_sandbox_https_tls12_rsa_pss
                    PROPERTIES TIMEOUT 30 LABELS "sandbox;tls;rsa-pss"
                )
            endif()
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(
                NAME worker_strict_sandbox_https_ca
                COMMAND test-direct-fetch-tls
                    $<TARGET_FILE:capsid-worker>
                    "${CAPSID_DIRECT_FETCH_TLS_FIXTURE}"
                    "${CAPSID_MBEDTLS_TEST_DATA}/server2-sha256.crt"
                    "${CAPSID_MBEDTLS_TEST_DATA}/server2.key"
                    "${CAPSID_SANDBOX_TEST_DATA_DIR}/test-ca-sha256.crt"
                    --strict
            )
            set_tests_properties(
                worker_strict_sandbox_https_ca
                PROPERTIES TIMEOUT 30 LABELS "sandbox"
            )
        endif()

        add_executable(test-stubborn-worker tests/stubborn_worker.cc)
        add_executable(test-worker-destroy-status tests/test_worker_destroy_status.cc)
        target_include_directories(test-worker-destroy-status PRIVATE tests)
        target_link_libraries(test-worker-destroy-status PRIVATE capsid_runtime capsid_sanitizers)
        set_target_properties(test-worker-destroy-status PROPERTIES
            CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(test-worker-destroy-status PRIVATE /W4 /WX)
            else()
                target_compile_options(test-worker-destroy-status PRIVATE -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_dependencies(test-worker-destroy-status capsid-worker)
        foreach(mode IN ITEMS null clean terminated)
            add_test(NAME worker_destroy_status_${mode}
                COMMAND test-worker-destroy-status ${mode} $<TARGET_FILE:capsid-worker>)
            set_tests_properties(worker_destroy_status_${mode} PROPERTIES TIMEOUT 15)
        endforeach()
        if(UNIX)
            add_test(NAME worker_destroy_status_reaped
                COMMAND test-worker-destroy-status reaped $<TARGET_FILE:capsid-worker>)
            set_tests_properties(worker_destroy_status_reaped PROPERTIES TIMEOUT 15)
        endif()
        add_executable(test-worker-lifecycle tests/test_worker_lifecycle.cc)
        target_include_directories(test-worker-lifecycle PRIVATE tests)
        target_link_libraries(test-worker-lifecycle PRIVATE capsid_runtime)
        add_test(
            NAME worker_bounded_destroy
            COMMAND test-worker-lifecycle $<TARGET_FILE:test-stubborn-worker>
        )
        set_tests_properties(worker_bounded_destroy PROPERTIES TIMEOUT 5)
        add_executable(
            test-worker-timeout-drain
            tests/test_worker_timeout_drain.cc)
        target_include_directories(test-worker-timeout-drain PRIVATE tests)
        target_link_libraries(
            test-worker-timeout-drain
            PRIVATE capsid_runtime
        )
        add_test(
            NAME worker_timeout_drain
            COMMAND test-worker-timeout-drain
                $<TARGET_FILE:test-stubborn-worker>
        )
        set_tests_properties(worker_timeout_drain PROPERTIES TIMEOUT 20)
        add_executable(test-worker-watchdog-observation tests/test_worker_watchdog_observation.cc)
        target_include_directories(test-worker-watchdog-observation PRIVATE src tests)
        target_link_libraries(test-worker-watchdog-observation PRIVATE capsid_runtime capsid_sanitizers)
        set_target_properties(test-worker-watchdog-observation PROPERTIES
            CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        add_dependencies(test-worker-watchdog-observation test-stubborn-worker)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(test-worker-watchdog-observation PRIVATE /W4 /WX)
            else()
                target_compile_options(test-worker-watchdog-observation PRIVATE -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
        add_test(NAME worker_watchdog_active_deadline
            COMMAND test-worker-watchdog-observation $<TARGET_FILE:test-stubborn-worker> active)
        add_test(NAME worker_watchdog_late_first_observation
            COMMAND test-worker-watchdog-observation $<TARGET_FILE:test-stubborn-worker> late)
        set_tests_properties(worker_watchdog_active_deadline worker_watchdog_late_first_observation
            PROPERTIES TIMEOUT 10 LABELS "worker;abi;timeout;lifecycle")
    endif()
endif()


# The sanitizer policy has its own positive/negative leak controls. Production
# sandbox controls remain registered under the production configuration.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CAPSID_SANDBOX_PROFILE STREQUAL "sanitizer" AND CAPSID_ENABLE_ASAN)
    find_package(Threads REQUIRED)
    if(NOT CAPSID_PYTHON3_EXECUTABLE)
        find_program(CAPSID_PYTHON3_EXECUTABLE NAMES python3 python REQUIRED)
    endif()
    add_executable(test-sanitizer-leak-control
        tests/test_sanitizer_leak_control.cc src/sandbox.cc)
    target_include_directories(test-sanitizer-leak-control PRIVATE include src)
    target_link_libraries(test-sanitizer-leak-control PRIVATE capsid_sanitizers Threads::Threads)
    if(CAPSID_STRICT_WARNINGS)
        target_compile_options(test-sanitizer-leak-control PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    foreach(leak_case IN ITEMS clean leak)
        add_test(NAME "sanitizer_sandbox_${leak_case}"
            COMMAND "${CAPSID_PYTHON3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sanitizer_leak_control.py"
                --binary $<TARGET_FILE:test-sanitizer-leak-control> --case "${leak_case}")
        set_tests_properties("sanitizer_sandbox_${leak_case}" PROPERTIES
            TIMEOUT 40 LABELS "sanitizer;sandbox;memory"
            ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
    endforeach()
endif()

# Compile the production client in a separate test object. Only that object
# redirects native calls; ordinary runtime and test witnesses use real APIs.
if(BUILD_TESTING AND TARGET capsid-worker AND
   CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin|Windows)$")
    add_library(test-worker-spawn-rollback-client OBJECT src/client.cc)
    target_include_directories(test-worker-spawn-rollback-client PRIVATE
        include src tests "${CAPSID_GENERATED_DIR}")
    target_link_libraries(test-worker-spawn-rollback-client PRIVATE capsid_sanitizers)
    target_compile_definitions(test-worker-spawn-rollback-client PRIVATE
        CAPSID_TEST_SPAWN_ROLLBACK_RENAME=1)
    if(CAPSID_ENABLE_ASAN)
        target_compile_definitions(test-worker-spawn-rollback-client PRIVATE CAPSID_ASAN_BUILD=1)
    endif()
    if(CAPSID_ENABLE_TSAN)
        target_compile_definitions(test-worker-spawn-rollback-client PRIVATE CAPSID_TSAN_BUILD=1)
    endif()
    if(MSVC)
        target_compile_options(test-worker-spawn-rollback-client PRIVATE
            "/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/worker_spawn_rollback_hooks.h")
    else()
        target_compile_options(test-worker-spawn-rollback-client PRIVATE
            -include "${CMAKE_CURRENT_SOURCE_DIR}/tests/worker_spawn_rollback_hooks.h")
    endif()
    add_executable(test-worker-spawn-rollback tests/test_worker_spawn_rollback.cc
        $<TARGET_OBJECTS:test-worker-spawn-rollback-client>)
    target_include_directories(test-worker-spawn-rollback PRIVATE include src tests)
    target_link_libraries(test-worker-spawn-rollback PRIVATE capsid_runtime capsid_sanitizers Threads::Threads)
    foreach(rollback_target IN ITEMS test-worker-spawn-rollback-client test-worker-spawn-rollback)
        set_target_properties(${rollback_target} PROPERTIES CXX_STANDARD 11
            CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(${rollback_target} PRIVATE /W4 /WX)
            else()
                target_compile_options(${rollback_target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
                if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                    target_compile_options(${rollback_target} PRIVATE -Wno-maybe-uninitialized)
                endif()
            endif()
        endif()
    endforeach()
    add_dependencies(test-worker-spawn-rollback capsid-worker test-stubborn-worker)
    add_test(NAME worker_spawn_rollback
        COMMAND test-worker-spawn-rollback $<TARGET_FILE:capsid-worker> $<TARGET_FILE:test-stubborn-worker>)
    set_tests_properties(worker_spawn_rollback PROPERTIES
        TIMEOUT 15 LABELS "worker;abi;lifecycle;allocation")
endif()

# These allocation rollback/admission tests replace global new/delete.
# Clang's static TSan C++ runtime defines those symbols strongly. Use its
# shared runtime for these tests to retain the allocator probes and race
# instrumentation. The Host and Worker keep their ordinary TSan link mode.
if(BUILD_TESTING AND CAPSID_ENABLE_TSAN AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-runtime-dir
        RESULT_VARIABLE _capsid_allocation_tsan_result
        OUTPUT_VARIABLE _capsid_allocation_tsan_dir
        ERROR_VARIABLE _capsid_allocation_tsan_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    file(GLOB _capsid_allocation_tsan_shared
        "${_capsid_allocation_tsan_dir}/libclang_rt.tsan*.so")
    if(NOT _capsid_allocation_tsan_result STREQUAL "0" OR
       NOT _capsid_allocation_tsan_shared)
        message(FATAL_ERROR
            "Allocation tests require Clang's shared TSan runtime: "
            "${_capsid_allocation_tsan_error}")
    endif()
    foreach(_capsid_allocation_test IN ITEMS
            test-channel-io test-host-single-worker-lifecycle
            test-host-static-pool-adoption test-host-external-drain
            test-worker-spawn-rollback)
        if(TARGET "${_capsid_allocation_test}")
            target_link_options("${_capsid_allocation_test}" PRIVATE -shared-libsan)
            set_property(TARGET "${_capsid_allocation_test}" APPEND PROPERTY
                BUILD_RPATH "${_capsid_allocation_tsan_dir}")
        endif()
    endforeach()
endif()
