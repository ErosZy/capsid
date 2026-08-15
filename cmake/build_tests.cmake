if(BUILD_TESTING)
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
                        host_managed_executable_active_state_validation_fail_closed
                        host_managed_executable_enforces_global_worker_capacity
                        host_managed_executable_enforces_queue_maximums
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
                        host_managed_executable_crash_loop_quarantines
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
                        PROPERTIES TIMEOUT 40 SKIP_RETURN_CODE 77)
                endforeach()
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
                structured_log_app_lane_is_fifo)
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
                ledger_no_surge_refuses)
            add_test(
                NAME "host_worker_capacity_ledger_${CAPSID_LEDGER_TEST_ID}"
                COMMAND test-host-worker-capacity-ledger
                    "${CAPSID_LEDGER_TEST_ID}")
            set_tests_properties(
                "host_worker_capacity_ledger_${CAPSID_LEDGER_TEST_ID}"
                PROPERTIES TIMEOUT 10)
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
            # Multi-shard scenarios require SO_REUSEPORT, which Windows
            # does not provide; only the single-shard scenarios run there
            # (see docs/windows.md).
            if(NOT WIN32)
            add_test(
                NAME host_static_pool_server_shared_port_lifecycle
                COMMAND test-host-static-pool-server lifecycle
                    $<TARGET_FILE:capsid-worker>)
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
                host_static_pool_server_shared_port_lifecycle
                host_static_pool_server_drain_inflight_completes
                host_static_pool_server_drain_deadline_forces
                host_static_pool_server_drain_idle_exits PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)
            endif()
            add_test(
                NAME host_static_pool_server_atomic_start_failure
                COMMAND test-host-static-pool-server atomic-failure
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
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(
                NAME host_admission_worker_death_returns_503
                COMMAND test-host-admission worker-death-503
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_admission_worker_death_returns_503 PROPERTIES
                TIMEOUT 30 LABELS "host;integration;m2")
            endif()
            # pool-forwards runs a multi-shard pool (SO_REUSEPORT is
            # unavailable on Windows; see docs/windows.md).
            if(NOT WIN32)
            add_test(
                NAME host_admission_pool_forwards_options
                COMMAND test-host-admission pool-forwards-admission
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_admission_pool_forwards_options PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)
            endif()
            set_tests_properties(
                host_admission_inflight_full_rejects
                host_admission_queue_full_rejects
                host_admission_queue_timeout_returns_504 PROPERTIES
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
                NAME host_fast_response_untouched
                COMMAND test-host-write-timeout fast-response-untouched
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_write_timeout_cancels
                host_worker_deadline_independent
                host_fast_response_untouched PROPERTIES
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
            endif()

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
            # The pool variant needs a multi-shard pool (SO_REUSEPORT is
            # unavailable on Windows; see docs/windows.md).
            if(NOT WIN32)
            add_test(
                NAME host_concurrent_pool_wait
                COMMAND test-host-concurrent-wait pool
                    $<TARGET_FILE:capsid-worker>)
            set_tests_properties(
                host_concurrent_pool_wait PROPERTIES
                LABELS "host;integration;m2"
                TIMEOUT 30)
            endif()

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
            add_test(
                NAME host_sanitizer_instrumentation
                COMMAND "${CMAKE_COMMAND}"
                    "-DCAPSID_COMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json"
                    "-DCAPSID_EXPECT_ASAN=${CAPSID_ENABLE_ASAN}"
                    "-DCAPSID_EXPECT_UBSAN=${CAPSID_ENABLE_UBSAN}"
                    "-DCAPSID_EXPECT_TSAN=${CAPSID_ENABLE_TSAN}"
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
    if(TARGET capsid-worker)
        add_test(
            NAME abi_guard_oom_countdown
            COMMAND test-abi-guard $<TARGET_FILE:capsid-worker>)
    else()
        add_test(NAME abi_guard_oom_countdown COMMAND test-abi-guard)
    endif()
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
            # claims in docs/conformance.md.
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
        target_include_directories(
            test-host-worker-executor PRIVATE
            include src "${CAPSID_GENERATED_DIR}")
        target_link_libraries(test-host-worker-executor PRIVATE
            capsid_runtime
            capsid_host_core
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
            endif()
        endif()
        add_dependencies(test-host-worker-executor capsid-worker)
        add_test(
            NAME host_worker_executor_contract
            COMMAND test-host-worker-executor $<TARGET_FILE:capsid-worker>)
        set_tests_properties(host_worker_executor_contract PROPERTIES
            TIMEOUT 40)

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
        set_tests_properties(host_generation_pool_contract PROPERTIES
            TIMEOUT 90)
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
        set_tests_properties(worker_package_contents PROPERTIES TIMEOUT 1800)

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
                -DCAPSID_SMOKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}
                -DCAPSID_HOST_TARGET=${CAPSID_HOST_TARGET_PRESENT}
                "-DCAPSID_NODE_EXECUTABLE=${CAPSID_NODE_EXECUTABLE}"
                "-DCAPSID_HOST_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/host-single-worker.js"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_smoke.cmake"
        )
        set_tests_properties(worker_package_smoke PROPERTIES
            TIMEOUT 600
            DEPENDS worker_package_contents)

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
                "-DCAPSID_REPRO_GENERATOR=${CMAKE_GENERATOR}"
                "-DCAPSID_REPRO_PARALLEL=${CAPSID_REPRO_PARALLEL}"
                "-DCAPSID_REPRO_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
                "-DCAPSID_REPRO_OPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}"
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
            test-queue-saturation-fixture
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
                        "${CAPSID_MBEDTLS_TEST_DATA}/test-ca-sha256.crt"
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
    endif()
endif()
