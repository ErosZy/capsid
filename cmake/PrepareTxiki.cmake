if(NOT DEFINED CAPSID_VENDOR_SOURCE OR NOT DEFINED CAPSID_VENDOR_OVERLAY)
    message(FATAL_ERROR "CAPSID_VENDOR_SOURCE and CAPSID_VENDOR_OVERLAY are required")
endif()

file(REMOVE_RECURSE "${CAPSID_VENDOR_OVERLAY}")
file(MAKE_DIRECTORY "${CAPSID_VENDOR_OVERLAY}")
file(COPY
    "${CAPSID_VENDOR_SOURCE}/CMakeLists.txt"
    "${CAPSID_VENDOR_SOURCE}/LICENSE"
    DESTINATION "${CAPSID_VENDOR_OVERLAY}"
)
file(COPY "${CAPSID_VENDOR_SOURCE}/src" DESTINATION "${CAPSID_VENDOR_OVERLAY}")
file(COPY "${CAPSID_VENDOR_SOURCE}/tests/fixtures" DESTINATION "${CAPSID_VENDOR_OVERLAY}/tests")
# Dependencies are mostly immutable and can stay symlinked. libwebsockets,
# WAMR, quickjs, and mbedtls are patched by the restricted runtime overlay,
# however, so they need copy-on-write storage. Patching through a symlink
# would either fail or silently dirty the vendor submodule.
file(MAKE_DIRECTORY "${CAPSID_VENDOR_OVERLAY}/deps")
file(GLOB CAPSID_VENDOR_DEPS
    RELATIVE "${CAPSID_VENDOR_SOURCE}/deps"
    "${CAPSID_VENDOR_SOURCE}/deps/*")
foreach(CAPSID_VENDOR_DEP IN LISTS CAPSID_VENDOR_DEPS)
    if(NOT CAPSID_VENDOR_DEP STREQUAL "libwebsockets"
       AND NOT CAPSID_VENDOR_DEP STREQUAL "wamr"
       AND NOT CAPSID_VENDOR_DEP STREQUAL "quickjs"
       AND NOT CAPSID_VENDOR_DEP STREQUAL "mbedtls")
        if(WIN32)
            # Windows without Developer Mode cannot create symlinks, and
            # CMake's COPY_ON_ERROR fallback does not populate directories;
            # copy the dependency tree outright (content-identical, and the
            # overlay key hashes files, not link topology).
            file(COPY
                "${CAPSID_VENDOR_SOURCE}/deps/${CAPSID_VENDOR_DEP}"
                DESTINATION "${CAPSID_VENDOR_OVERLAY}/deps"
            )
        else()
            file(CREATE_LINK
                "${CAPSID_VENDOR_SOURCE}/deps/${CAPSID_VENDOR_DEP}"
                "${CAPSID_VENDOR_OVERLAY}/deps/${CAPSID_VENDOR_DEP}"
                SYMBOLIC
            )
        endif()
    endif()
endforeach()

# WAMR, quickjs, and mbedtls are copied as a unit because patches may span
# their loaders, runtimes, and public headers over time. Keeping the whole
# dependency private to the overlay also makes vendor-clean checks
# unambiguous.
file(COPY
    "${CAPSID_VENDOR_SOURCE}/deps/wamr"
    "${CAPSID_VENDOR_SOURCE}/deps/quickjs"
    "${CAPSID_VENDOR_SOURCE}/deps/mbedtls"
    DESTINATION "${CAPSID_VENDOR_OVERLAY}/deps"
)

set(CAPSID_LWS_SOURCE "${CAPSID_VENDOR_SOURCE}/deps/libwebsockets")
set(CAPSID_LWS_OVERLAY "${CAPSID_VENDOR_OVERLAY}/deps/libwebsockets")
file(MAKE_DIRECTORY "${CAPSID_LWS_OVERLAY}")
file(GLOB CAPSID_LWS_ENTRIES
    RELATIVE "${CAPSID_LWS_SOURCE}"
    "${CAPSID_LWS_SOURCE}/*")
foreach(CAPSID_LWS_ENTRY IN LISTS CAPSID_LWS_ENTRIES)
    # CMakeLists.txt is also copied: 0011-lws-cpack-top-level.patch gates
    # libwebsockets' include(CPack) on top-level-ness, and a subproject
    # CPack would silently take over the parent `package` target (remediation
    # spec §12.1). Patching through a symlink would dirty the vendor
    # submodule, so the build file must be copy-on-write like include/ and
    # lib/.
    if(CAPSID_LWS_ENTRY STREQUAL "include" OR
       CAPSID_LWS_ENTRY STREQUAL "lib" OR
       CAPSID_LWS_ENTRY STREQUAL "CMakeLists.txt")
        file(COPY
            "${CAPSID_LWS_SOURCE}/${CAPSID_LWS_ENTRY}"
            DESTINATION "${CAPSID_LWS_OVERLAY}"
        )
    elseif(WIN32)
        # See the dependency loop above: no Developer Mode -> no symlinks,
        # and COPY_ON_ERROR does not populate directories, so copy.
        file(COPY
            "${CAPSID_LWS_SOURCE}/${CAPSID_LWS_ENTRY}"
            DESTINATION "${CAPSID_LWS_OVERLAY}"
        )
    else()
        file(CREATE_LINK
            "${CAPSID_LWS_SOURCE}/${CAPSID_LWS_ENTRY}"
            "${CAPSID_LWS_OVERLAY}/${CAPSID_LWS_ENTRY}"
            SYMBOLIC
        )
    endif()
endforeach()
execute_process(
    COMMAND git -C "${CAPSID_LWS_SOURCE}" rev-parse --absolute-git-dir
    OUTPUT_VARIABLE CAPSID_LWS_GIT_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE CAPSID_LWS_GIT_RESULT
)
if(CAPSID_LWS_GIT_RESULT EQUAL 0 AND CAPSID_LWS_GIT_DIR)
    # The .git link keeps git tooling working inside the overlay. It is an
    # optimization, not a build input: when the platform cannot create
    # symlinks (Windows without Developer Mode), the overlay simply runs
    # without it.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E create_symlink
            "${CAPSID_LWS_GIT_DIR}"
            "${CAPSID_LWS_OVERLAY}/.git"
        RESULT_VARIABLE CAPSID_LWS_GIT_LINK_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT CAPSID_LWS_GIT_LINK_RESULT EQUAL 0)
        file(REMOVE "${CAPSID_LWS_OVERLAY}/.git")
    endif()
endif()

# The patch stack is applied with git apply rather than PATH's patch:
# patch binaries differ per machine (GNU patch from git-bash, Strawberry
# Perl's patch — which asserts on mixed line endings), while git is
# already a hard build requirement. --ignore-whitespace lets the context
# match across CRLF/LF checkout conventions.
find_program(CAPSID_GIT_EXECUTABLE git REQUIRED)
file(GLOB CAPSID_PATCHES "${CMAKE_CURRENT_LIST_DIR}/../patches/txiki/*.patch")
list(SORT CAPSID_PATCHES)
foreach(CAPSID_PATCH IN LISTS CAPSID_PATCHES)
    # WORKING_DIRECTORY alone (no `git -C`): mingw git mis-resolves
    # drive-lettered -C paths and then fails deps-relative hunks with
    # "No such file or directory".
    execute_process(
        COMMAND "${CAPSID_GIT_EXECUTABLE}"
            apply -p1 --ignore-whitespace "${CAPSID_PATCH}"
        WORKING_DIRECTORY "${CAPSID_VENDOR_OVERLAY}"
        RESULT_VARIABLE CAPSID_PATCH_RESULT
        OUTPUT_VARIABLE CAPSID_PATCH_OUTPUT
        ERROR_VARIABLE CAPSID_PATCH_ERROR
    )
    if(NOT CAPSID_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply ${CAPSID_PATCH}\n${CAPSID_PATCH_OUTPUT}\n${CAPSID_PATCH_ERROR}")
    endif()
endforeach()
