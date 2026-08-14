# Capsid install rules, CPack packaging and package artifacts
# (remediation spec §12).
#
# Included LAST from the top-level CMakeLists.txt. Ordering is the §12.1
# defense: capsid sets its own CPACK_* variables and calls include(CPack)
# after every third-party directory has been processed, then asserts that
# CPACK_PACKAGE_NAME is still "capsid". The libwebsockets overlay patch
# (0011-lws-cpack-top-level.patch) additionally gates the vendored
# include(CPack) on top-level-ness, so the assertion is a tripwire, not
# the only barrier.

# ---- §12.2 install manifest -------------------------------------------------
# Only public artifacts are installed: the worker/compiler/host binaries,
# the runtime library (static), the public headers, support docs, the
# license and the identity/SBOM data. Test fixtures, build directories,
# private headers, keys and vendored sources never leave the build tree.
# Target existence — not just an option flag — decides what may be
# installed: the host is skipped when Boost is missing, and the worker
# pair (worker + its bytecode compiler) is absent in CAPSID_BUILD_WORKER=OFF
# matrices, where a host-only package is still a valid artifact.
set(CAPSID_INSTALL_BINARIES)
foreach(CAPSID_INSTALL_CANDIDATE IN ITEMS
        capsid-worker capsid-bytecode-compile capsid-host)
    if(TARGET ${CAPSID_INSTALL_CANDIDATE})
        list(APPEND CAPSID_INSTALL_BINARIES ${CAPSID_INSTALL_CANDIDATE})
    endif()
endforeach()

# capsid_sanitizers is an INTERFACE library capsid_runtime links PUBLIC;
# it must be exported alongside so a consumer's find_package works without
# rebuilding the graph. All destinations are relative, keeping the export
# relocatable for the package smoke's empty-directory consumption.
install(TARGETS ${CAPSID_INSTALL_BINARIES} capsid_runtime capsid_sanitizers
    EXPORT CapsidTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)
install(EXPORT CapsidTargets
    NAMESPACE Capsid::
    DESTINATION lib/cmake/Capsid
    FILE CapsidTargets.cmake)

install(FILES
    include/capsid/runtime.h
    include/capsid/runtime.hpp
    DESTINATION include/capsid)

install(FILES
    README.md
    docs/architecture.md
    docs/host-integration.md
    docs/capsid-json.md
    docs/host-config.md
    docs/capability-policy.md
    docs/module-permissions.md
    docs/linux-sandbox.md
    docs/conformance.md
    docs/performance-benchmarks.md
    docs/testing.md
    DESTINATION share/doc/capsid)

install(FILES LICENSE DESTINATION share/licenses/capsid)

# Identity artifacts are generated at configure time (spec §12.3): the
# package ships build-info.txt (identity summary + both canonical records)
# and the SBOM. The file manifest and the staged-file hashes inside the
# SBOM are produced at INSTALL time by GeneratePackageArtifacts.cmake,
# because only the installed tree is authoritative for what actually ships.
install(FILES
    "${CAPSID_GENERATED_DIR}/build-info.txt"
    DESTINATION share/capsid)

# The static worker is the deployment form: capsid_runtime and the vendored
# txiki stack (lws, QuickJS, WAMR, mbedtls) are linked in, so the installed
# binaries have no build-tree or package-relative runtime dependencies
# beyond the libc family. INSTALL_RPATH is set explicitly so a future
# switch to a shared capsid_runtime keeps $ORIGIN/../lib working, and the
# package smoke scans the dynamic sections to prove the policy holds.
foreach(CAPSID_RPATH_TARGET IN LISTS CAPSID_INSTALL_BINARIES)
    set_target_properties(${CAPSID_RPATH_TARGET} PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib")
endforeach()

# ---- §12.3 package naming ---------------------------------------------------
# capsid-<version>-<system>-<arch>.tar.gz. Names are lowercased so the
# archive name is stable across host spellings (Linux/Darwin, x86_64/AMD64).
# Computed BEFORE the install(CODE) below: the SBOM name and the basename are
# expanded at configure time, so the package identity must already exist.
string(TOLOWER "${CMAKE_SYSTEM_NAME}" CAPSID_PACKAGE_SYSTEM)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64")
    set(CAPSID_PACKAGE_ARCH "x86_64")
else()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" CAPSID_PACKAGE_ARCH)
endif()
set(CAPSID_PACKAGE_BASENAME
    "capsid-${PROJECT_VERSION}-${CAPSID_PACKAGE_SYSTEM}-${CAPSID_PACKAGE_ARCH}")
set(CAPSID_SBOM_NAME "${CAPSID_PACKAGE_BASENAME}")

# Post-install generation of the file manifest and the SPDX SBOM. Runs for
# both `cmake --install` (worker_install_tree gate) and CPack (the package
# content is the staged install tree, so the artifacts are packaged too).
# CAPSID_BUILD_* (exported from ComputeBuildIdentity via PARENT_SCOPE) and
# CAPSID_BUILD_COMMIT_DATE are expanded at configure time;
# CMAKE_INSTALL_PREFIX and CMAKE_CURRENT_INSTALL_PREFIX are evaluated at
# install time by install(CODE).
# NOTE: NONE of the -D values may carry quote characters in the generated
# script. CMake 4.4's lexer keeps mid-argument quotes as literal characters,
# so a \"-quoted -D value reaches the child cmake with quote characters
# inside the variable; GeneratePackageArtifacts would then emit JSON like
# ""sha256:..."" whose first valid value is the empty string — string(JSON)
# SET silently truncates instead of failing. (Verified: the child receives
# ["sha256:..."] from -DX="sha256:...".) All the values below are quoteless
# barewords by construction.
install(CODE "
    execute_process(
        COMMAND \"${CMAKE_COMMAND}\"
            -DCAPSID_SBOM_NAME=${CAPSID_SBOM_NAME}
            -DCAPSID_SBOM_VERSION=${PROJECT_VERSION}
            -DCAPSID_SBOM_BUILD_ID=${CAPSID_BUILD_BUILD_ID}
            -DCAPSID_SBOM_COMPAT_ID=${CAPSID_BUILD_COMPATIBILITY_ID}
            -DCAPSID_SBOM_COMMIT=${CAPSID_BUILD_CAPSID_COMMIT}
            -DCAPSID_SBOM_COMMIT_DATE=${CAPSID_BUILD_COMMIT_DATE}
            -DCAPSID_SBOM_QUICKJS=${CAPSID_BUILD_QUICKJS_COMMIT}
            -DCAPSID_SBOM_OVERLAY_KEY=${CAPSID_BUILD_TXIKI_OVERLAY_KEY}
            -DCAPSID_SBOM_OVERLAY_MANIFEST=${CAPSID_BUILD_TXIKI_OVERLAY_MANIFEST}
            -DCAPSID_SBOM_PREFIX=\${CMAKE_INSTALL_PREFIX}
            -P \"${CMAKE_CURRENT_SOURCE_DIR}/cmake/GeneratePackageArtifacts.cmake\"
        RESULT_VARIABLE CAPSID_SBOM_GEN_RESULT
        OUTPUT_VARIABLE CAPSID_SBOM_GEN_OUT
        ERROR_VARIABLE CAPSID_SBOM_GEN_ERR)
    if(NOT \"\${CAPSID_SBOM_GEN_RESULT}\" EQUAL 0)
        message(FATAL_ERROR
            \"SBOM/file-manifest generation failed \"
            \"(exit=\${CAPSID_SBOM_GEN_RESULT})\n\"
            \"stdout: \${CAPSID_SBOM_GEN_OUT}\n\"
            \"stderr: \${CAPSID_SBOM_GEN_ERR}\")
    endif()
")

# ---- §12.3 package format (CPack) -------------------------------------------
set(CPACK_PACKAGE_NAME "capsid")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Capsid")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Capsid restricted JavaScript runtime worker + host")
set(CPACK_PACKAGE_FILE_NAME "${CAPSID_PACKAGE_BASENAME}")
set(CPACK_GENERATOR "TGZ")
set(CPACK_THREADS 0)
set(CPACK_PACKAGE_CHECKSUM "SHA256")

# Reproducibility (spec §12.3): member timestamps default to the install
# time; with SOURCE_DATE_EPOCH set (the reproducibility gate exports the
# commit date) CPack's archive generators produce stable timestamps so two
# fresh builds of the same commit can be compared byte-for-byte.
if(DEFINED ENV{SOURCE_DATE_EPOCH})
    set(CPACK_ARCHIVE_MTIME "$ENV{SOURCE_DATE_EPOCH}")
endif()

# §12.1 tripwire: capsid must own the CPack configuration. Any third-party
# include(CPack) that ran before us (unpatched lws, a future dependency)
# would have overwritten CPACK_PACKAGE_NAME; fail the configure instead of
# silently packaging the wrong project.
include(CPack)
if(NOT CPACK_PACKAGE_NAME STREQUAL "capsid")
    message(FATAL_ERROR
        "CPack configuration was taken over by a third party: "
        "CPACK_PACKAGE_NAME=${CPACK_PACKAGE_NAME} (expected capsid)")
endif()
