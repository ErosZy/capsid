# Build identity matrix (WP-00 / PR-01 RED gate for P0-7).
#
# The bytecode-compatibility record is generated at configure time into
# <build>/generated/build-identity-record.txt. Every controlled build
# difference must change the record (and therefore the compatibility ID),
# and every repeated identical configure must produce the identical record.
#
# Each matrix entry uses a FRESH configure directory; reusing an old build
# directory must never be accepted as evidence.
#
# Current failure (pre-WP-07): CAPSID_BUILD_COMPILE_FLAGS is assembled as
# a CMake list and passed to a one-value argument, so the record only
# keeps "build_type=... lto=..." and drops asan/ubsan/mimalloc; ASAN and
# UBSAN configures then produce byte-identical records.
#
# Usage (script mode):
#   cmake -DCAPSID_SOURCE_DIR=... -DCAPSID_CMAKE_COMMAND=...
#        [-DCAPSID_MATRIX_VARIANTS="plain;asan;ubsan;mimalloc;lto-off"]
#        [-DCAPSID_MATRIX_WORK_DIR=...]
#        -P tests/test_build_identity_matrix.cmake

if(NOT CAPSID_SOURCE_DIR OR NOT CAPSID_CMAKE_COMMAND)
    message(FATAL_ERROR
        "CAPSID_SOURCE_DIR and CAPSID_CMAKE_COMMAND are required")
endif()

if(NOT CAPSID_MATRIX_VARIANTS)
    set(CAPSID_MATRIX_VARIANTS "plain;asan;ubsan;mimalloc;lto-off")
endif()

if(NOT CAPSID_MATRIX_WORK_DIR)
    set(CAPSID_MATRIX_WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/identity-matrix")
endif()

# Every variant must be a fresh configure. The work root is wiped first so
# a reused directory can never leak into the comparison.
file(REMOVE_RECURSE "${CAPSID_MATRIX_WORK_DIR}")
file(MAKE_DIRECTORY "${CAPSID_MATRIX_WORK_DIR}")

function(capsid_matrix_configure variant build_dir)
    if("${variant}" STREQUAL "plain")
        set(extra_flags)
    elseif("${variant}" STREQUAL "asan")
        set(extra_flags "-DCAPSID_ENABLE_ASAN=ON")
    elseif("${variant}" STREQUAL "ubsan")
        set(extra_flags "-DCAPSID_ENABLE_UBSAN=ON")
    elseif("${variant}" STREQUAL "mimalloc")
        set(extra_flags "-DCAPSID_USE_MIMALLOC=ON")
    elseif("${variant}" STREQUAL "lto-off")
        set(extra_flags "-DCAPSID_ENABLE_LTO=OFF")
    else()
        message(FATAL_ERROR "unknown matrix variant: ${variant}")
    endif()
    execute_process(
        COMMAND "${CAPSID_CMAKE_COMMAND}"
            -S "${CAPSID_SOURCE_DIR}"
            -B "${build_dir}"
            -DCAPSID_BUILD_WORKER=OFF
            -DBUILD_TESTING=OFF
            -DCMAKE_BUILD_TYPE=Release
            ${extra_flags}
        RESULT_VARIABLE configure_result
        OUTPUT_VARIABLE configure_output
        ERROR_VARIABLE configure_error)
    if(NOT configure_result EQUAL 0)
        message(FATAL_ERROR
            "variant ${variant} configure failed:\n${configure_output}"
            "\n${configure_error}")
    endif()
endfunction()

function(capsid_matrix_record variant build_dir out_record out_digest)
    set(record_file "${build_dir}/generated/build-identity-record.txt")
    if(NOT EXISTS "${record_file}")
        message(FATAL_ERROR
            "variant ${variant} produced no identity record: ${record_file}")
    endif()
    file(READ "${record_file}" record)
    file(SHA256 "${record_file}" digest)
    if(NOT record MATCHES
       "^schema=capsid-bytecode-compatibility-v1\n")
        message(FATAL_ERROR
            "variant ${variant} record has the wrong schema header")
    endif()
    # The compile-flags line must carry every key that changes bytecode
    # compatibility. The pre-fix implementation truncates at the first
    # CMake list separator and drops asan/ubsan/mimalloc.
    if(NOT record MATCHES
       "bytecodeCompileFlags=build_type=[^ ]* lto=(ON|OFF) asan=(ON|OFF) ubsan=(ON|OFF) mimalloc=(ON|OFF)\n")
        message(FATAL_ERROR
            "variant ${variant} compile flags are incomplete; record:\n"
            "${record}")
    endif()
    set(${out_record} "${record}" PARENT_SCOPE)
    set(${out_digest} "${digest}" PARENT_SCOPE)
endfunction()

set(CAPSID_MATRIX_RECORDS)
set(CAPSID_MATRIX_DIGESTS)
set(CAPSID_MATRIX_DIRS)
foreach(variant IN LISTS CAPSID_MATRIX_VARIANTS)
    set(build_dir
        "${CAPSID_MATRIX_WORK_DIR}/configure-${variant}")
    capsid_matrix_configure("${variant}" "${build_dir}")
    capsid_matrix_record("${variant}" "${build_dir}"
        record digest)
    list(APPEND CAPSID_MATRIX_RECORDS "${record}")
    list(APPEND CAPSID_MATRIX_DIGESTS "${digest}")
    list(APPEND CAPSID_MATRIX_DIRS "${build_dir}")
    string(LENGTH "${record}" record_length)
    if(record_length EQUAL 0)
        message(FATAL_ERROR
            "variant ${variant} record must not be empty")
    endif()
endforeach()

# Every controlled difference must change the identity.
list(LENGTH CAPSID_MATRIX_RECORDS variant_count)
if(variant_count LESS 2)
    message(FATAL_ERROR
        "identity matrix needs at least two variants")
endif()
set(CAPSID_MATRIX_INDEX 0)
while(CAPSID_MATRIX_INDEX LESS variant_count)
    list(GET CAPSID_MATRIX_DIGESTS ${CAPSID_MATRIX_INDEX} digest)
    set(CAPSID_MATRIX_OTHER 0)
    while(CAPSID_MATRIX_OTHER LESS variant_count)
        if(NOT CAPSID_MATRIX_OTHER EQUAL CAPSID_MATRIX_INDEX)
            list(GET CAPSID_MATRIX_DIGESTS ${CAPSID_MATRIX_OTHER} other_digest)
            if(digest STREQUAL other_digest)
                list(GET CAPSID_MATRIX_VARIANTS ${CAPSID_MATRIX_INDEX} variant)
                list(GET CAPSID_MATRIX_VARIANTS ${CAPSID_MATRIX_OTHER} other)
                message(FATAL_ERROR
                    "identity collision: ${variant} and ${other} both "
                    "produce ${digest}")
            endif()
        endif()
        math(EXPR CAPSID_MATRIX_OTHER "${CAPSID_MATRIX_OTHER} + 1")
    endwhile()
    math(EXPR CAPSID_MATRIX_INDEX "${CAPSID_MATRIX_INDEX} + 1")
endwhile()

# Identical configure twice must produce the identical record.
set(plain_index -1)
set(CAPSID_MATRIX_INDEX 0)
foreach(variant IN LISTS CAPSID_MATRIX_VARIANTS)
    if(variant STREQUAL "plain")
        set(plain_index ${CAPSID_MATRIX_INDEX})
    endif()
    math(EXPR CAPSID_MATRIX_INDEX "${CAPSID_MATRIX_INDEX} + 1")
endforeach()
if(plain_index LESS 0)
    message(FATAL_ERROR
        "identity matrix must include the plain variant for the "
        "repeatability check")
endif()
list(GET CAPSID_MATRIX_DIGESTS ${plain_index} plain_digest)
set(repeat_dir "${CAPSID_MATRIX_WORK_DIR}/configure-plain-repeat")
capsid_matrix_configure("plain" "${repeat_dir}")
capsid_matrix_record("plain" "${repeat_dir}" repeat_record repeat_digest)
if(NOT repeat_digest STREQUAL plain_digest)
    message(FATAL_ERROR
        "identical plain configure produced a different identity: "
        "${repeat_digest} vs ${plain_digest}")
endif()
list(APPEND CAPSID_MATRIX_DIRS "${repeat_dir}")

file(REMOVE_RECURSE ${CAPSID_MATRIX_DIRS})
file(REMOVE_RECURSE "${CAPSID_MATRIX_WORK_DIR}")
message(STATUS "PASS: ${variant_count} identity variants all distinct and "
        "reproducible")
