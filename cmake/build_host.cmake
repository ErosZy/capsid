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
    find_package(OpenSSL 3.5 REQUIRED COMPONENTS Crypto)

    if(NOT TARGET capsid_jansson)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/vendor/jansson")
    endif()

    add_library(capsid_host_core STATIC
        src/host/active_state.cc
        src/host/artifact_safe_read.cc
        src/host/bytecode_attestation.cc
        src/host/config.cc
        src/host/generation_identity.cc
        src/host/managed_host.cc
        src/host/policy_compiler.cc
        src/host/request_normalization.cc
        src/host/secret_file_provider.cc
        src/host/secret_snapshot.cc
        src/host/service_lifecycle.cc
        src/host/worker_recovery.cc
    )
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
    find_package(Boost 1.74 QUIET COMPONENTS system)
    if(Boost_FOUND)
        # M1A design gate (design review §4.3): only the WorkerEventSource
        # adapter may call capsid_worker_fd(); every other Host module
        # observes the worker IPC through readable/writable semantics.
        # Enforce the boundary at configure time.
        file(GLOB CAPSID_HOST_ALL_SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/src/host/*.cc")
        foreach(CAPSID_HOST_AUDIT_SOURCE IN LISTS CAPSID_HOST_ALL_SOURCES)
            if(NOT CAPSID_HOST_AUDIT_SOURCE
                   MATCHES "worker_event_source\\.cc$")
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
            src/host/single_worker_server.cc
            src/host/worker_event_source.cc)
        target_include_directories(capsid-host PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(capsid-host PRIVATE
            capsid_runtime
            capsid_host_core
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
                # TSan does not support std::atomic_thread_fence, which
                # Boost.Asio uses for its fenced blocks; demote only that
                # warning class so the TSan instrumentation itself stays on.
                if(CAPSID_ENABLE_TSAN AND
                   CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                    target_compile_options(capsid-host PRIVATE
                        -Wno-error=tsan)
                endif()
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
    else()
        message(STATUS "capsid-host skipped: system Boost not found")
    endif()
endif()
