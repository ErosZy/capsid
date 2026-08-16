# First-party Capsid Host targets. Only active with CAPSID_BUILD_HOST=ON.
#
# capsid_host_core is a C++20 internal library. Both capsid_host_core and
# capsid_jansson link capsid_sanitizers, the same way capsid_runtime does:
# for static libraries the sanitizer flags must be applied at compile time
# to the library objects themselves, not only to test executables, or the
# sanitizer coverage of the parsing core is fake (see
# cmake/TestHostSanitizerInstrumentation.cmake). The Host data plane will
# add its own TSan job later (see the Host design review §14).

if(CAPSID_BUILD_HOST)
    # OpenSSL 3.5 LTS is the Host's digest/signature provider. On Apple
    # machines the Homebrew keg may live outside CMake's default search
    # paths; hint it before find_package.
    if(APPLE)
        if(EXISTS "/usr/local/opt/openssl@3.5")
            list(APPEND CMAKE_PREFIX_PATH "/usr/local/opt/openssl@3.5")
        elseif(EXISTS "/opt/homebrew/opt/openssl@3.5")
            list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/openssl@3.5")
        endif()
    endif()
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

    if(NOT TARGET capsid_jansson)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/vendor/jansson")
    endif()

    add_library(capsid_host_core STATIC
        src/host/active_state.cc
        src/host/artifact_safe_read.cc
        src/host/binding_compile.cc
        src/host/binding_registry.cc
        src/host/bytecode_attestation.cc
        src/host/config.cc
        src/host/generation_identity.cc
        src/host/generation_pool.cc
        src/host/local_capsid_policy.cc
        src/host/metrics.cc
        src/host/policy_compiler.cc
        src/host/process_snapshot.cc
        src/host/request_normalization.cc
        src/host/secret_snapshot.cc
        src/host/service_lifecycle.cc
        src/host/static_pool.cc
        src/host/structured_log.cc
        src/host/static_pool_server.cc
        src/host/trusted_key_store.cc
        src/host/worker_capacity_ledger.cc
        src/host/worker_event_source.cc
        src/host/worker_executor.cc
        src/host/worker_recovery.cc
        src/host/worker_supervisor.cc
    )
    if(NOT WIN32)
        # The managed coordinator is POSIX-only (dirfd-relative openat/
        # mkdirat state walks, uid-based verification, UDS admin plane);
        # Windows builds ship the single-worker and static-pool data
        # planes only (see docs/windows.md).
        target_sources(capsid_host_core PRIVATE
            src/host/admin_api.cc
            src/host/host_config_model.cc
            src/host/managed_admin_backend.cc
            src/host/managed_host.cc
            src/host/managed_registry.cc
            src/host/secret_file_provider.cc
        )
    endif()
    # managed_host.cc and the Admin adapters call the worker API; the
    # dependency is PUBLIC so every consumer of the core links the worker
    # runtime, including adapters whose own test code never touches
    # capsid_worker_* directly.
    target_link_libraries(capsid_host_core PUBLIC capsid_runtime)
    set_target_properties(capsid_host_core PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_link_libraries(capsid_jansson PRIVATE capsid_sanitizers)
    target_link_libraries(capsid_host_core PRIVATE
        capsid_jansson
        OpenSSL::Crypto
        capsid_sanitizers
    )
    target_include_directories(capsid_host_core PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    # managed_host.cc constructs the worker configuration itself, so it must
    # see the sanitizer identity instead of relying on capsid_runtime's
    # PRIVATE definitions. Both sanitizers reserve a vast shadow address
    # range and therefore cannot run a worker under finite RLIMIT_AS.
    if(CAPSID_ENABLE_ASAN)
        target_compile_definitions(capsid_host_core PRIVATE
            CAPSID_ASAN_BUILD=1)
    endif()
    if(CAPSID_ENABLE_TSAN)
        target_compile_definitions(capsid_host_core PRIVATE
            CAPSID_TSAN_BUILD=1)
    endif()
    if(CAPSID_STRICT_WARNINGS)
        if(MSVC)
            target_compile_options(capsid_host_core PRIVATE /W4 /WX)
        else()
            target_compile_options(capsid_host_core PRIVATE
                -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()

    # M1A single-worker Host data plane. System Boost.Asio/Beast only: no
    # FetchContent, no configure-time downloads, no hand-written HTTP parser.
    # Platforms without system Boost configure fine but get no capsid-host;
    # the frozen integration test registration only references the target
    # when CAPSID_BUILD_WORKER is enabled, which requires the Linux toolchain.
    # A fully static package (-static in CMAKE_EXE_LINKER_FLAGS) cannot link
    # Boost's shared library; FindBoost must be told to prefer the archive
    # before it resolves the target (Alpine's boost-dev ships both, and the
    # default picks libboost_system.so.1.84.0).
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND
       CMAKE_EXE_LINKER_FLAGS MATCHES "(^| )-static( |$)")
        set(Boost_USE_STATIC_LIBS ON)
    endif()
    find_package(Boost 1.74 QUIET COMPONENTS system)
    if(NOT Boost_FOUND)
        # Boost >= 1.87 ships Boost.System as header-only and no longer
        # provides the `system` component; the config package still works
        # without components. Recover Boost_FOUND and synthesize the
        # Boost::system target from header-only Boost::headers so every
        # link site keeps a single target name on both eras.
        find_package(Boost 1.74 QUIET)
        if(Boost_FOUND AND NOT TARGET Boost::system)
            add_library(Boost::system INTERFACE IMPORTED)
            set_target_properties(Boost::system PROPERTIES
                INTERFACE_LINK_LIBRARIES Boost::headers)
        endif()
    endif()
    if(Boost_FOUND AND TARGET Boost::system)
        # CMake 4.1 removed the FindBoost module; the config-package
        # targets (vcpkg in particular) do not always carry the include
        # directory. Attach it explicitly when missing so the data-plane
        # sources find <boost/asio.hpp>.
        get_target_property(CAPSID_BOOST_INCLUDE_DIRS Boost::system
            INTERFACE_INCLUDE_DIRECTORIES)
        if(NOT CAPSID_BOOST_INCLUDE_DIRS)
            find_path(CAPSID_BOOST_SYSTEM_INCLUDE_DIR
                NAMES boost/version.hpp REQUIRED)
            set_target_properties(Boost::system PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES
                "${CAPSID_BOOST_SYSTEM_INCLUDE_DIR}")
        endif()
    endif()
    if(Boost_FOUND)
        # M1A design gate (design review §4.3): only the WorkerEventSource
        # adapter may call capsid_worker_fd(); every other Host module
        # observes the worker IPC through readable/writable semantics.
        # M2 item 5a adds the worker supervisor: it polls the current
        # worker's IPC fd to observe EXIT and drive replacement/quarantine
        # (design §10.5) — a second legitimate owner, in the managed layer
        # only, never touching the data plane.
        # Enforce the boundary at configure time.
        file(GLOB CAPSID_HOST_ALL_SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/src/host/*.cc")
        foreach(CAPSID_HOST_AUDIT_SOURCE IN LISTS CAPSID_HOST_ALL_SOURCES)
            if(NOT CAPSID_HOST_AUDIT_SOURCE
                   MATCHES "worker_event_source\\.cc$"
                   AND NOT CAPSID_HOST_AUDIT_SOURCE
                   MATCHES "worker_supervisor\\.cc$")
                file(READ "${CAPSID_HOST_AUDIT_SOURCE}"
                    CAPSID_HOST_AUDIT_TEXT)
                if(CAPSID_HOST_AUDIT_TEXT MATCHES "capsid_worker_fd")
                    message(FATAL_ERROR
                        "only WorkerEventSource may call capsid_worker_fd() "
                        "(found in ${CAPSID_HOST_AUDIT_SOURCE})")
                endif()
            endif()
        endforeach()

        add_executable(capsid-host
            src/host/main.cc
            src/host/single_worker_server.cc)
        target_include_directories(capsid-host PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CAPSID_GENERATED_DIR}")
        target_link_libraries(capsid-host PRIVATE
            capsid_runtime
            capsid_host_core
            capsid_jansson
            OpenSSL::Crypto
            Boost::system
            capsid_sanitizers)
        set_target_properties(capsid-host PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        if(CAPSID_STRICT_WARNINGS)
            if(MSVC)
                target_compile_options(capsid-host PRIVATE /W4 /WX)
            else()
                target_compile_options(capsid-host PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
                # GCC 13 reports a spurious -Wmaybe-uninitialized inside
                # Boost.Beast's basic_parser (system headers) only under
                # sanitizer instrumentation; suppress just this warning class
                # for the target, keeping every other warning fatal.
                if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                    target_compile_options(capsid-host PRIVATE
                        -Wno-maybe-uninitialized)
                endif()
                # -Wno-error=tsan for GNU TSan builds now lives on the
                # capsid_sanitizers interface (CMakeLists.txt), the single
                # source of truth every sanitizer-linked target inherits.
            endif()
        endif()
        if(CAPSID_GENERATE_LINK_MAP)
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
               CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
                target_link_options(
                    capsid-host PRIVATE
                    "LINKER:-Map,${CAPSID_GENERATED_DIR}/capsid-host.map")
            endif()
        endif()
        # The Admin HTTP transport uses Boost.Beast as the HTTP/1 framing
        # authority; it joins the host core only when Boost is available,
        # so a Boost-less platform still builds the pure Admin dispatcher.
        # The long-lived Admin service depends on the accepted-connection
        # transport, so it joins in the same gate. (Managed-only sources;
        # excluded on Windows with the rest of the coordinator.)
        if(NOT WIN32)
            target_sources(capsid_host_core PRIVATE
                src/host/admin_http.cc
                src/host/admin_service.cc)
        endif()
        # The WP-05 data plane joins the same gate: the Managed listener
        # is Boost.Asio. (RoutingTable publishes through the C++11 shared_ptr
        # atomic free functions, so it no longer needs C++20
        # std::atomic<shared_ptr> — Apple libc++ was the blocker, PR-10.)
        # A Boost-less platform (e.g. the macOS portable-unit gate, spec
        # §9.6-10) builds the pure coordinator surface and skips the data
        # plane; the data-plane tests are already registered under UNIX AND
        # Boost_FOUND.
        if(NOT WIN32)
            target_sources(capsid_host_core PRIVATE
                src/host/managed_listener.cc
                src/host/routing_snapshot.cc)
        endif()
        target_link_libraries(capsid_host_core PRIVATE Boost::system)
    else()
        message(STATUS "capsid-host skipped: system Boost not found")
    endif()
endif()
