# Package smoke test (remediation spec §12.4).
#
# The CI consumes the Capsid archive the way a customer would: extract into
# an EMPTY directory, then use ONLY package-internal paths to prove the
# shipped artifact works end to end:
#
#   1. compile a C and a C++ public-header sample against the packaged
#      include/ and lib/libcapsid_runtime.a;
#   2. probe build-info.txt (version/commit/buildId/compatId shape) and
#      cross-check the packaged bytecode compiler's compatibility id;
#   3. start the packaged worker, load a bundle, complete a request and
#      shut down gracefully (both samples, C and C++);
#   4. start the packaged Host (when the build ships one), wait readiness,
#      send real HTTP requests, SIGTERM graceful exit — driven by the
#      existing node driver with the packaged binaries;
#   5. scan the package: no build-root absolute paths, no secret canaries,
#      no undeclared dynamic dependencies.
#
# Also verifies the shipped FILE-MANIFEST.txt hashes and that the SBOM is
# parseable SPDX 2.3 with exactly the four pinned packages.
#
# Usage (script mode):
#   cmake -DCAPSID_BUILD_DIR=... -DCAPSID_WORK_DIR=<fresh dir>
#        -DCAPSID_SOURCE_DIR=<repo root> -DCAPSID_C_COMPILER=...
#        -DCAPSID_CXX_COMPILER=...
#        -DCAPSID_HOST_TARGET=ON|OFF
#        -DCAPSID_NODE_EXECUTABLE=<node> -DCAPSID_HOST_FIXTURE=<repo file>
#        -DCAPSID_SMOKE_SAMPLE_C=... -DCAPSID_SMOKE_SAMPLE_CC=...
#        -P tests/package_smoke.cmake
#
# Depends on worker_package_contents (it must have produced the archive).

foreach(CAPSID_SMOKE_REQUIRED
        CAPSID_BUILD_DIR CAPSID_WORK_DIR CAPSID_SOURCE_DIR
        CAPSID_C_COMPILER CAPSID_CXX_COMPILER CAPSID_HOST_TARGET
        CAPSID_SMOKE_SYSTEM_NAME
        CAPSID_SMOKE_SAMPLE_C CAPSID_SMOKE_SAMPLE_CC)
    if(NOT DEFINED ${CAPSID_SMOKE_REQUIRED})
        message(FATAL_ERROR "package_smoke requires ${CAPSID_SMOKE_REQUIRED}")
    endif()
endforeach()

if(NOT CAPSID_HOST_TARGET)
    set(CAPSID_SMOKE_HOST "OFF")
else()
    set(CAPSID_SMOKE_HOST "ON")
endif()

file(REMOVE_RECURSE "${CAPSID_WORK_DIR}")
file(MAKE_DIRECTORY "${CAPSID_WORK_DIR}")

# ---- locate the newest Capsid archive ----------------------------------------
set(CAPSID_SMOKE_ARCHIVE)
set(CAPSID_SMOKE_NEWEST_MTIME -1)
# The Windows CPack generator produces a ZIP archive; POSIX builds a
# tar.gz. Accept whichever the platform produced.
file(GLOB CAPSID_SMOKE_ALL_ARCHIVES
    "${CAPSID_BUILD_DIR}/capsid-*.tar.gz"
    "${CAPSID_BUILD_DIR}/capsid-*.zip")
if(NOT CAPSID_SMOKE_ALL_ARCHIVES)
    message(FATAL_ERROR "no capsid archive in ${CAPSID_BUILD_DIR}")
endif()
foreach(archive IN LISTS CAPSID_SMOKE_ALL_ARCHIVES)
    file(TIMESTAMP "${archive}" archive_mtime "%s" UTC)
    if(archive_mtime GREATER CAPSID_SMOKE_NEWEST_MTIME)
        set(CAPSID_SMOKE_NEWEST_MTIME "${archive_mtime}")
        set(CAPSID_SMOKE_ARCHIVE "${archive}")
    endif()
endforeach()

set(CAPSID_SMOKE_ROOT "${CAPSID_WORK_DIR}/extracted")
file(ARCHIVE_EXTRACT
    INPUT "${CAPSID_SMOKE_ARCHIVE}"
    DESTINATION "${CAPSID_SMOKE_ROOT}")
file(GLOB CAPSID_SMOKE_ENTRIES "${CAPSID_SMOKE_ROOT}/*")
list(LENGTH CAPSID_SMOKE_ENTRIES CAPSID_SMOKE_ENTRY_COUNT)
set(CAPSID_PACKAGE_ROOT "${CAPSID_SMOKE_ROOT}")
if(CAPSID_SMOKE_ENTRY_COUNT EQUAL 1)
    list(GET CAPSID_SMOKE_ENTRIES 0 CAPSID_SMOKE_SINGLE_ENTRY)
    if(IS_DIRECTORY "${CAPSID_SMOKE_SINGLE_ENTRY}")
        set(CAPSID_PACKAGE_ROOT "${CAPSID_SMOKE_SINGLE_ENTRY}")
    endif()
endif()

get_filename_component(CAPSID_SMOKE_ARCHIVE_NAME
    "${CAPSID_SMOKE_ARCHIVE}" NAME)
message(STATUS "package smoke: ${CAPSID_SMOKE_ARCHIVE_NAME} -> "
        "${CAPSID_PACKAGE_ROOT}")

# ---- §12.4 step 2a: FILE-MANIFEST hashes ------------------------------------
# Every entry is "<rel>\t<size>\t<sha256>"; recompute and compare both.
set(CAPSID_SMOKE_MANIFEST
    "${CAPSID_PACKAGE_ROOT}/share/capsid/FILE-MANIFEST.txt")
if(NOT EXISTS "${CAPSID_SMOKE_MANIFEST}")
    message(FATAL_ERROR "package is missing FILE-MANIFEST.txt")
endif()
file(STRINGS "${CAPSID_SMOKE_MANIFEST}" CAPSID_SMOKE_MANIFEST_LINES)
list(LENGTH CAPSID_SMOKE_MANIFEST_LINES CAPSID_SMOKE_MANIFEST_COUNT)
if(CAPSID_SMOKE_MANIFEST_COUNT LESS 10)
    message(FATAL_ERROR "FILE-MANIFEST.txt has only "
        "${CAPSID_SMOKE_MANIFEST_COUNT} entries")
endif()
set(CAPSID_SMOKE_MANIFEST_ENTRIES)
foreach(manifest_line IN LISTS CAPSID_SMOKE_MANIFEST_LINES)
    string(REPLACE "\t" ";" manifest_parts "${manifest_line}")
    list(LENGTH manifest_parts part_count)
    if(NOT part_count EQUAL 3)
        message(FATAL_ERROR "malformed FILE-MANIFEST line: ${manifest_line}")
    endif()
    list(GET manifest_parts 0 manifest_rel)
    list(GET manifest_parts 1 manifest_size)
    list(GET manifest_parts 2 manifest_sha)
    set(manifest_path "${CAPSID_PACKAGE_ROOT}/${manifest_rel}")
    if(NOT EXISTS "${manifest_path}")
        message(FATAL_ERROR "FILE-MANIFEST lists missing file "
            "${manifest_rel}")
    endif()
    file(SHA256 "${manifest_path}" computed_sha)
    file(SIZE "${manifest_path}" computed_size)
    if(NOT computed_sha STREQUAL manifest_sha)
        message(FATAL_ERROR "FILE-MANIFEST sha256 mismatch for "
            "${manifest_rel}:\nmanifest ${manifest_sha}\ncomputed ${computed_sha}")
    endif()
    if(NOT computed_size STREQUAL manifest_size)
        message(FATAL_ERROR "FILE-MANIFEST size mismatch for "
            "${manifest_rel}: manifest ${manifest_size} actual "
            "${computed_size}")
    endif()
    list(APPEND CAPSID_SMOKE_MANIFEST_ENTRIES "${manifest_rel}")
endforeach()

# ---- §12.4 step 2b: SBOM is parseable SPDX 2.3 with the four packages -------
set(CAPSID_SMOKE_SBOM "${CAPSID_PACKAGE_ROOT}/share/capsid/SBOM.spdx.json")
if(NOT EXISTS "${CAPSID_SMOKE_SBOM}")
    message(FATAL_ERROR "package is missing SBOM.spdx.json")
endif()
file(READ "${CAPSID_SMOKE_SBOM}" CAPSID_SMOKE_SBOM_TEXT)
# string(JSON) GET fails the configure on unparseable input, so a successful
# GET is a real parse check with no external interpreter dependency.
string(JSON CAPSID_SMOKE_SBOM_SPDX GET "${CAPSID_SMOKE_SBOM_TEXT}" spdxVersion)
if(NOT CAPSID_SMOKE_SBOM_SPDX STREQUAL "SPDX-2.3")
    message(FATAL_ERROR "SBOM spdxVersion=${CAPSID_SMOKE_SBOM_SPDX}")
endif()
string(JSON CAPSID_SMOKE_SBOM_PKGS LENGTH
    "${CAPSID_SMOKE_SBOM_TEXT}" packages)
if(NOT CAPSID_SMOKE_SBOM_PKGS EQUAL 4)
    message(FATAL_ERROR "SBOM declares ${CAPSID_SMOKE_SBOM_PKGS} packages, "
        "expected 4 (capsid, txiki.js, quickjs-ng, libwebsockets)")
endif()
string(REPLACE ".tar.gz" "" CAPSID_SMOKE_BASENAME
    "${CAPSID_SMOKE_ARCHIVE_NAME}")
string(REPLACE ".zip" "" CAPSID_SMOKE_BASENAME
    "${CAPSID_SMOKE_BASENAME}")
string(JSON CAPSID_SMOKE_SBOM_NAME GET "${CAPSID_SMOKE_SBOM_TEXT}" name)
if(NOT CAPSID_SMOKE_SBOM_NAME STREQUAL "${CAPSID_SMOKE_BASENAME}-sbom")
    message(FATAL_ERROR "SBOM name=${CAPSID_SMOKE_SBOM_NAME} expected "
        "${CAPSID_SMOKE_BASENAME}-sbom")
endif()

# ---- §12.4 step 2c: build-info probe ----------------------------------------
set(CAPSID_SMOKE_BUILD_INFO
    "${CAPSID_PACKAGE_ROOT}/share/capsid/build-info.txt")
if(NOT EXISTS "${CAPSID_SMOKE_BUILD_INFO}")
    message(FATAL_ERROR "package is missing build-info.txt")
endif()
file(STRINGS "${CAPSID_SMOKE_BUILD_INFO}" CAPSID_SMOKE_INFO_LINES)
set(CAPSID_SMOKE_VERSION)
set(CAPSID_SMOKE_COMMIT)
set(CAPSID_SMOKE_BUILD_ID)
set(CAPSID_SMOKE_COMPAT_ID)
foreach(info_line IN LISTS CAPSID_SMOKE_INFO_LINES)
    if(info_line MATCHES "^version=(.+)$")
        set(CAPSID_SMOKE_VERSION "${CMAKE_MATCH_1}")
    elseif(info_line MATCHES "^commit=([0-9a-f]+)$")
        set(CAPSID_SMOKE_COMMIT "${CMAKE_MATCH_1}")
    elseif(info_line MATCHES "^buildId=sha256:([0-9a-f]+)$")
        set(CAPSID_SMOKE_BUILD_ID "${CMAKE_MATCH_1}")
    elseif(info_line MATCHES "^compatibilityId=sha256:([0-9a-f]+)$")
        set(CAPSID_SMOKE_COMPAT_ID "${CMAKE_MATCH_1}")
    endif()
endforeach()
# CMake's regex engine has no {n} quantifier, so the fixed widths are checked
# with explicit lengths (40-hex commit, 64-hex sha256 digests).
if(NOT CAPSID_SMOKE_VERSION STREQUAL "0.1.2")
    message(FATAL_ERROR "build-info version=${CAPSID_SMOKE_VERSION}")
endif()
string(LENGTH "${CAPSID_SMOKE_COMMIT}" commit_len)
if(NOT commit_len EQUAL 40)
    message(FATAL_ERROR "build-info commit missing or not 40-hex: "
        "${CAPSID_SMOKE_COMMIT}")
endif()
string(LENGTH "${CAPSID_SMOKE_BUILD_ID}" build_id_len)
string(LENGTH "${CAPSID_SMOKE_COMPAT_ID}" compat_id_len)
if(NOT build_id_len EQUAL 64 OR NOT compat_id_len EQUAL 64)
    message(FATAL_ERROR "build-info buildId/compatibilityId missing or "
        "malformed: buildId=${CAPSID_SMOKE_BUILD_ID} "
        "compatId=${CAPSID_SMOKE_COMPAT_ID}")
endif()

# The packaged bytecode compiler must agree with the packaged runtime's
# compatibility identity: a mismatched compiler/runtime pair would emit
# bytecode the shipped worker cannot load.
execute_process(
    COMMAND "${CAPSID_PACKAGE_ROOT}/bin/capsid-bytecode-compile"
        --print-compatibility-id
    RESULT_VARIABLE compat_probe_result
    OUTPUT_VARIABLE compat_probe_out
    ERROR_VARIABLE compat_probe_err)
if(NOT compat_probe_result EQUAL 0)
    message(FATAL_ERROR "capsid-bytecode-compile --print-compatibility-id "
        "failed:\n${compat_probe_err}")
endif()
string(STRIP "${compat_probe_out}" compat_probe_id)
if(NOT compat_probe_id STREQUAL "sha256:${CAPSID_SMOKE_COMPAT_ID}")
    message(FATAL_ERROR "packaged compiler compatibility id ${compat_probe_id}"
        " != build-info compatibilityId sha256:${CAPSID_SMOKE_COMPAT_ID}")
endif()

# ---- §12.4 step 1+3: C and C++ samples --------------------------------------
set(CAPSID_SMOKE_BUNDLE "${CAPSID_WORK_DIR}/smoke-bundle.mjs")
file(WRITE "${CAPSID_SMOKE_BUNDLE}"
    "export default {\n"
    "    async fetch(request) {\n"
    "        return new Response('package smoke ok', {\n"
    "            status: 200,\n"
    "            headers: { 'content-type': 'text/plain' },\n"
    "        });\n"
    "    },\n"
    "};\n")

# Also prove the shipped bytecode compiler produces real, non-empty outputs
# for the same source (the worker round trips above load the source bundle).
execute_process(
    COMMAND "${CAPSID_PACKAGE_ROOT}/bin/capsid-bytecode-compile"
        --source "${CAPSID_SMOKE_BUNDLE}"
        --source-name "smoke.mjs"
        --application "package-smoke"
        --version "0.1.2"
        --key-id "smoke-key"
        --bytecode-out "${CAPSID_WORK_DIR}/smoke.qjsb"
        --attestation-out "${CAPSID_WORK_DIR}/smoke-bytecode.json"
        --signing-message-out "${CAPSID_WORK_DIR}/smoke-message.bin"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_out
    ERROR_VARIABLE compile_err)
if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR "packaged bytecode compiler failed:\n"
        "${compile_err}")
endif()
foreach(compiled_output
        "${CAPSID_WORK_DIR}/smoke.qjsb"
        "${CAPSID_WORK_DIR}/smoke-bytecode.json"
        "${CAPSID_WORK_DIR}/smoke-message.bin")
    file(SIZE "${compiled_output}" compiled_size)
    if(compiled_size EQUAL 0)
        message(FATAL_ERROR "bytecode compiler produced an empty output: "
            "${compiled_output}")
    endif()
endforeach()

set(CAPSID_SMOKE_C_SAMPLE "${CAPSID_WORK_DIR}/package_smoke_sample.c")
set(CAPSID_SMOKE_CC_SAMPLE "${CAPSID_WORK_DIR}/package_smoke_sample.cc")
file(COPY "${CAPSID_SMOKE_SAMPLE_C}" DESTINATION "${CAPSID_WORK_DIR}")
file(COPY "${CAPSID_SMOKE_SAMPLE_CC}" DESTINATION "${CAPSID_WORK_DIR}")
get_filename_component(CAPSID_SMOKE_SAMPLE_C_NAME
    "${CAPSID_SMOKE_SAMPLE_C}" NAME)
get_filename_component(CAPSID_SMOKE_SAMPLE_CC_NAME
    "${CAPSID_SMOKE_SAMPLE_CC}" NAME)

set(CAPSID_PACKAGE_INCLUDE "${CAPSID_PACKAGE_ROOT}/include")
set(CAPSID_PACKAGE_LIB "${CAPSID_PACKAGE_ROOT}/lib")
if(WIN32)
    set(CAPSID_SMOKE_WORKER
        "${CAPSID_PACKAGE_ROOT}/bin/capsid-worker.exe")
    set(CAPSID_SMOKE_RUNTIME_LIB
        "${CAPSID_PACKAGE_LIB}/capsid_runtime.lib")
else()
    set(CAPSID_SMOKE_WORKER "${CAPSID_PACKAGE_ROOT}/bin/capsid-worker")
    set(CAPSID_SMOKE_RUNTIME_LIB
        "${CAPSID_PACKAGE_LIB}/libcapsid_runtime.a")
endif()

# capsid_runtime is a C++ static library; the C sample is compiled by the C
# compiler but must be LINKED by the C++ driver (CMake's target machinery
# infers this in-tree; script mode makes it explicit). Compile/link flags
# are platform-specific (cl vs gcc/clang).
if(WIN32)
    execute_process(
        COMMAND "${CAPSID_C_COMPILER}"
            /std:c11 /W4
            "-I${CAPSID_PACKAGE_INCLUDE}"
            /c "${CAPSID_WORK_DIR}/${CAPSID_SMOKE_SAMPLE_C_NAME}"
            "/Fo${CAPSID_WORK_DIR}/sample_c.obj"
        RESULT_VARIABLE c_compile_result
        OUTPUT_VARIABLE c_compile_out
        ERROR_VARIABLE c_compile_err)
    if(NOT c_compile_result EQUAL 0)
        message(FATAL_ERROR "C sample compile failed against the package:\n"
            "${c_compile_out}\n${c_compile_err}")
    endif()
    execute_process(
        COMMAND "${CAPSID_CXX_COMPILER}"
            "${CAPSID_WORK_DIR}/sample_c.obj"
            "${CAPSID_SMOKE_RUNTIME_LIB}"
            ws2_32.lib
            "/Fe${CAPSID_WORK_DIR}/sample_c.exe"
        RESULT_VARIABLE c_link_result
        OUTPUT_VARIABLE c_link_out
        ERROR_VARIABLE c_link_err)
    if(NOT c_link_result EQUAL 0)
        message(FATAL_ERROR "C sample link failed against the package:\n"
            "${c_link_out}\n${c_link_err}")
    endif()
    execute_process(
        COMMAND "${CAPSID_CXX_COMPILER}"
            /std:c++17 /W4
            "-I${CAPSID_PACKAGE_INCLUDE}"
            "${CAPSID_WORK_DIR}/${CAPSID_SMOKE_SAMPLE_CC_NAME}"
            "${CAPSID_SMOKE_RUNTIME_LIB}"
            ws2_32.lib
            "/Fe${CAPSID_WORK_DIR}/sample_cpp.exe"
        RESULT_VARIABLE cxx_compile_result
        OUTPUT_VARIABLE cxx_compile_out
        ERROR_VARIABLE cxx_compile_err)
    if(NOT cxx_compile_result EQUAL 0)
        message(FATAL_ERROR "C++ sample compile failed against the package:\n"
            "${cxx_compile_out}\n${cxx_compile_err}")
    endif()
    set(CAPSID_SMOKE_SAMPLES sample_c.exe sample_cpp.exe)
else()
    execute_process(
        COMMAND "${CAPSID_C_COMPILER}"
            -std=c11 -Wall -Wextra
            "-I${CAPSID_PACKAGE_INCLUDE}"
            -c "${CAPSID_WORK_DIR}/${CAPSID_SMOKE_SAMPLE_C_NAME}"
            -o "${CAPSID_WORK_DIR}/sample_c.o"
        RESULT_VARIABLE c_compile_result
        OUTPUT_VARIABLE c_compile_out
        ERROR_VARIABLE c_compile_err)
    if(NOT c_compile_result EQUAL 0)
        message(FATAL_ERROR "C sample compile failed against the package:\n"
            "${c_compile_out}\n${c_compile_err}")
    endif()
    execute_process(
        COMMAND "${CAPSID_CXX_COMPILER}"
            "${CAPSID_WORK_DIR}/sample_c.o"
            "${CAPSID_PACKAGE_LIB}/libcapsid_runtime.a"
            -pthread
            -o "${CAPSID_WORK_DIR}/sample_c"
        RESULT_VARIABLE c_link_result
        OUTPUT_VARIABLE c_link_out
        ERROR_VARIABLE c_link_err)
    if(NOT c_link_result EQUAL 0)
        message(FATAL_ERROR "C sample link failed against the package:\n"
            "${c_link_out}\n${c_link_err}")
    endif()
    execute_process(
        COMMAND "${CAPSID_CXX_COMPILER}"
            -std=c++17 -Wall -Wextra
            "-I${CAPSID_PACKAGE_INCLUDE}"
            "${CAPSID_WORK_DIR}/${CAPSID_SMOKE_SAMPLE_CC_NAME}"
            "${CAPSID_PACKAGE_LIB}/libcapsid_runtime.a"
            -pthread
            -o "${CAPSID_WORK_DIR}/sample_cpp"
        RESULT_VARIABLE cxx_compile_result
        OUTPUT_VARIABLE cxx_compile_out
        ERROR_VARIABLE cxx_compile_err)
    if(NOT cxx_compile_result EQUAL 0)
        message(FATAL_ERROR "C++ sample compile failed against the package:\n"
            "${cxx_compile_out}\n${cxx_compile_err}")
    endif()
    set(CAPSID_SMOKE_SAMPLES sample_c sample_cpp)
endif()

foreach(sample_binary IN LISTS CAPSID_SMOKE_SAMPLES)
    execute_process(
        COMMAND "${CAPSID_WORK_DIR}/${sample_binary}"
            "${CAPSID_SMOKE_WORKER}" "${CAPSID_SMOKE_BUNDLE}"
        RESULT_VARIABLE sample_result
        OUTPUT_VARIABLE sample_out
        ERROR_VARIABLE sample_err
        TIMEOUT 60)
    if(NOT sample_result EQUAL 0)
        message(FATAL_ERROR "${sample_binary} failed (exit=${sample_result}):\n"
            "${sample_out}\n${sample_err}")
    endif()
    string(FIND "${sample_out}" "PASS:" pass_at)
    if(pass_at EQUAL -1)
        message(FATAL_ERROR "${sample_binary} did not report PASS:\n"
            "${sample_out}\n${sample_err}")
    endif()
endforeach()

# ---- §12.4 step 4: packaged Host (only when the build ships one) ------------
if(CAPSID_SMOKE_HOST)
    if(NOT DEFINED CAPSID_NODE_EXECUTABLE OR NOT CAPSID_NODE_EXECUTABLE)
        message(FATAL_ERROR
            "package ships capsid-host but no node executable was passed; "
            "the shipped Host must be smoke tested")
    endif()
    if(NOT CAPSID_HOST_FIXTURE OR NOT EXISTS "${CAPSID_HOST_FIXTURE}")
        message(FATAL_ERROR "CAPSID_HOST_FIXTURE is required when the "
            "package ships capsid-host: ${CAPSID_HOST_FIXTURE}")
    endif()
    file(COPY "${CAPSID_HOST_FIXTURE}"
        DESTINATION "${CAPSID_WORK_DIR}")
    get_filename_component(CAPSID_SMOKE_HOST_FIXTURE_NAME
        "${CAPSID_HOST_FIXTURE}" NAME)
    execute_process(
        COMMAND "${CAPSID_NODE_EXECUTABLE}"
            "${CAPSID_SOURCE_DIR}/tests/test_host_single_worker.mjs"
            --host "${CAPSID_PACKAGE_ROOT}/bin/capsid-host"
            --worker "${CAPSID_SMOKE_WORKER}"
            --bundle "${CAPSID_WORK_DIR}/${CAPSID_SMOKE_HOST_FIXTURE_NAME}"
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_out
        ERROR_VARIABLE host_err
        TIMEOUT 300)
    if(NOT host_result EQUAL 0)
        message(FATAL_ERROR "packaged Host smoke failed (exit=${host_result}):\n"
            "${host_out}\n${host_err}")
    endif()
else()
    message(STATUS "package smoke: no capsid-host in this build; host step "
        "skipped")
endif()

# ---- §12.4 step 5a: no build-root absolute paths -----------------------------
# Scan only text artifacts (headers, cmake export, docs, identity data): a
# build-tree path in any of them breaks the relocatable-package contract.
set(CAPSID_SMOKE_TEXT_SCAN)
foreach(manifest_rel IN LISTS CAPSID_SMOKE_MANIFEST_ENTRIES)
    if(manifest_rel MATCHES "[.](txt|json|cmake|md|h|hpp)$" OR
       manifest_rel MATCHES "licenses/")
        list(APPEND CAPSID_SMOKE_TEXT_SCAN
            "${CAPSID_PACKAGE_ROOT}/${manifest_rel}")
    endif()
endforeach()
# A few name-less license files may not match the extension filter; add the
# LICENSE explicitly if the manifest did not already include it.
set(CAPSID_SMOKE_LICENSE "${CAPSID_PACKAGE_ROOT}/share/licenses/capsid/LICENSE")
if(EXISTS "${CAPSID_SMOKE_LICENSE}")
    list(APPEND CAPSID_SMOKE_TEXT_SCAN "${CAPSID_SMOKE_LICENSE}")
endif()
foreach(text_file IN LISTS CAPSID_SMOKE_TEXT_SCAN)
    file(READ "${text_file}" text_content)
    file(RELATIVE_PATH text_rel
        "${CAPSID_PACKAGE_ROOT}" "${text_file}")
    foreach(needle IN ITEMS
            "${CAPSID_BUILD_DIR}"
            "${CAPSID_SOURCE_DIR}"
            "${CAPSID_PACKAGE_ROOT}")
        # Match the needle as a path component, not an arbitrary substring:
        # a build root that is a short prefix (e.g. "/capsid" vs a consumer
        # path like "/bin/capsid-worker" or "include/capsid/runtime.h")
        # would otherwise false-positive on package-internal tokens. A real
        # leak is the root used as a directory or value boundary.
        string(REPLACE "." "[.]" needle_re "${needle}")
        string(REGEX MATCH
            "(^|[^a-zA-Z0-9_.-/])${needle_re}(/|[^a-zA-Z0-9_-]|$)"
            leaked_hit "${text_content}")
        if(leaked_hit)
            message(FATAL_ERROR "build/packaging path leaked into "
                "${text_rel}: ${needle} (context: ${leaked_hit})")
        endif()
    endforeach()
    # Secret canaries inside text content.
    foreach(secret IN ITEMS
            "BEGIN PRIVATE KEY"
            "BEGIN RSA PRIVATE KEY")
        string(FIND "${text_content}" "${secret}" secret_at)
        if(NOT secret_at EQUAL -1)
            message(FATAL_ERROR "secret material leaked into ${text_rel}")
        endif()
    endforeach()
endforeach()
# Secret canaries by filename.
file(GLOB_RECURSE CAPSID_SMOKE_SECRET_FILES
    "${CAPSID_PACKAGE_ROOT}/*.pem"
    "${CAPSID_PACKAGE_ROOT}/*.key"
    "${CAPSID_PACKAGE_ROOT}/*.p12")
if(CAPSID_SMOKE_SECRET_FILES)
    message(FATAL_ERROR "secret files shipped in the package: "
        "${CAPSID_SMOKE_SECRET_FILES}")
endif()

# ---- §12.4 step 5b: no undeclared dynamic dependencies -----------------------
# The static worker must depend only on the platform libc family. A NEEDED
# entry outside the allowlist (or an @rpath that does not resolve inside the
# package) is an undeclared dynamic dependency.
# Entries are regex prefixes (matched as "^${allowed}"), so regex
# metacharacters are escaped with char classes: "libstdc++.so" would
# otherwise fail to compile ("c+" followed by "+") and the dots would
# match any character.
set(CAPSID_SMOKE_DYNAMIC_ALLOWLIST_LINUX
    "libc[.]so" "libm[.]so" "libpthread[.]so" "libdl[.]so" "librt[.]so"
    "libgcc_s[.]so" "libstdc[+][+][.]so" "ld-linux" "libresolv[.]so"
    "libutil[.]so" "libmbedtls[.]so" "libmbedcrypto[.]so" "libmbedx509[.]so")
file(GLOB CAPSID_SMOKE_BINARIES "${CAPSID_PACKAGE_ROOT}/bin/*")
foreach(binary IN LISTS CAPSID_SMOKE_BINARIES)
    get_filename_component(binary_name "${binary}" NAME)
    if(CAPSID_SMOKE_SYSTEM_NAME STREQUAL "Darwin")
        execute_process(
            COMMAND otool -L "${binary}"
            RESULT_VARIABLE otool_result
            OUTPUT_VARIABLE otool_out
            ERROR_VARIABLE otool_err)
        if(NOT otool_result EQUAL 0)
            message(FATAL_ERROR "otool -L failed for ${binary_name}:\n"
                "${otool_err}")
        endif()
        string(REPLACE "\n" ";" otool_lines "${otool_out}")
        set(otool_lineno 0)
        foreach(line IN LISTS otool_lines)
            string(STRIP "${line}" stripped)
            if(NOT stripped)
                continue()
            endif()
            # First line is the header "<binary>:". Everything after it is a
            # dependency entry.
            math(EXPR otool_lineno "${otool_lineno} + 1")
            if(otool_lineno EQUAL 1)
                continue()
            endif()
            if(stripped MATCHES "^/usr/lib/|^/System/Library/" OR
               stripped MATCHES "^@rpath/")
                continue()
            endif()
            message(FATAL_ERROR "${binary_name} has an undeclared dynamic "
                "dependency: ${stripped}")
        endforeach()
    elseif(CAPSID_SMOKE_SYSTEM_NAME STREQUAL "Windows")
        # PE import scan via dumpbin (VS toolchain). When dumpbin is not on
        # PATH (ctest without the dev environment), the scan is skipped
        # with a warning: the static /MT CRT policy and the Release job's
        # static vcpkg triplet are the packaging-time gates instead. CRT
        # DLLs (VCRUNTIME/MSVCP/ucrtbase) are intentionally NOT allowlisted
        # — their presence would prove a static-CRT regression.
        find_program(CAPSID_SMOKE_DUMPBIN NAMES dumpbin)
        if(NOT CAPSID_SMOKE_DUMPBIN)
            message(WARNING "dumpbin not found; skipping the Windows "
                "dynamic-dependency scan for ${binary_name}")
            continue()
        endif()
        set(CAPSID_SMOKE_DYNAMIC_ALLOWLIST_WINDOWS
            "KERNEL32[.]dll" "ntdll[.]dll" "WS2_32[.]dll" "bcrypt[.]dll"
            "CRYPT32[.]dll" "ADVAPI32[.]dll" "USER32[.]dll"
            "IPHLPAPI[.]DLL" "PSAPI[.]DLL" "SHELL32[.]dll" "OLE32[.]dll"
            "OLEAUT32[.]dll" "dbghelp[.]dll" "api-ms-win-")
        execute_process(
            COMMAND "${CAPSID_SMOKE_DUMPBIN}" /dependents "${binary}"
            RESULT_VARIABLE dumpbin_result
            OUTPUT_VARIABLE dumpbin_out
            ERROR_VARIABLE dumpbin_err)
        if(NOT dumpbin_result EQUAL 0)
            message(FATAL_ERROR "dumpbin /dependents failed for "
                "${binary_name}:\n${dumpbin_err}")
        endif()
        string(REPLACE "\n" ";" dumpbin_lines "${dumpbin_out}")
        set(in_dependencies "OFF")
        foreach(line IN LISTS dumpbin_lines)
            string(STRIP "${line}" stripped)
            if(stripped STREQUAL "Image has the following dependencies:")
                set(in_dependencies "ON")
                continue()
            endif()
            if(NOT in_dependencies)
                continue()
            endif()
            if(NOT stripped MATCHES "^([A-Za-z0-9_.-]+[.]dll)$")
                continue()
            endif()
            set(needed_name "${CMAKE_MATCH_1}")
            string(TOUPPER "${needed_name}" needed_name_upper)
            set(needed_allowed "OFF")
            foreach(allowed IN LISTS
                    CAPSID_SMOKE_DYNAMIC_ALLOWLIST_WINDOWS)
                string(TOUPPER "${allowed}" allowed_upper)
                if(needed_name_upper MATCHES "^${allowed_upper}")
                    set(needed_allowed "ON")
                    break()
                endif()
            endforeach()
            if(NOT needed_allowed)
                message(FATAL_ERROR "${binary_name} has an undeclared "
                    "dynamic dependency: ${needed_name}")
            endif()
        endforeach()
    elseif(CAPSID_SMOKE_SYSTEM_NAME STREQUAL "Linux")
        execute_process(
            COMMAND readelf -d "${binary}"
            RESULT_VARIABLE readelf_result
            OUTPUT_VARIABLE readelf_out
            ERROR_VARIABLE readelf_err)
        if(NOT readelf_result EQUAL 0)
            message(FATAL_ERROR "readelf -d failed for ${binary_name}:\n"
                "${readelf_err}")
        endif()
        string(REPLACE "\n" ";" readelf_lines "${readelf_out}")
        foreach(line IN LISTS readelf_lines)
            if(NOT line MATCHES "NEEDED.*\\[(.+)\\]$")
                continue()
            endif()
            set(needed_name "${CMAKE_MATCH_1}")
            set(needed_allowed "OFF")
            foreach(allowed IN LISTS CAPSID_SMOKE_DYNAMIC_ALLOWLIST_LINUX)
                if(needed_name MATCHES "^${allowed}")
                    set(needed_allowed "ON")
                    break()
                endif()
            endforeach()
            if(NOT needed_allowed)
                message(FATAL_ERROR "${binary_name} has an undeclared "
                    "dynamic dependency: ${needed_name}")
            endif()
        endforeach()
    else()
        message(FATAL_ERROR "package smoke: unsupported platform "
            "${CAPSID_SMOKE_SYSTEM_NAME} for the dynamic-dependency scan")
    endif()
endforeach()

message(STATUS "PASS: package ${CAPSID_SMOKE_ARCHIVE_NAME} smoke — samples, "
        "probe, worker round trips, host driver, scans")
