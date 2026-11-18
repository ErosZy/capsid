cmake_minimum_required(VERSION 3.18)
get_filename_component(source "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${source}/cmake/CapsidPackagePlatform.cmake")

if(DEFINED CAPSID_NEGATIVE_ARCH)
    capsid_package_platform(Linux "${CAPSID_NEGATIVE_ARCH}" musl platform arch)
    message(FATAL_ERROR "negative architecture unexpectedly accepted")
elseif(DEFINED CAPSID_NEGATIVE_LIBC)
    capsid_package_platform(Linux aarch64 "${CAPSID_NEGATIVE_LIBC}" platform arch)
    message(FATAL_ERROR "negative libc unexpectedly accepted")
endif()

foreach(row IN ITEMS
        "Linux|x86_64|musl|linux-musl-x86_64|x86_64"
        "Linux|AMD64|musl|linux-musl-x86_64|x86_64"
        "Linux|amd64|gnu|linux-gnu-x86_64|x86_64"
        "Linux|aarch64|musl|linux-musl-arm64|arm64"
        "Linux|ARM64|musl|linux-musl-arm64|arm64"
        "Linux|arm64|gnu|linux-gnu-arm64|arm64"
        "Darwin|arm64||darwin-arm64|arm64"
        "Darwin|x86_64||darwin-x86_64|x86_64"
        "Windows|AMD64||windows-x86_64|x86_64")
    # Keep an explicit placeholder: old CMake policies may drop empty list fields.
    string(REPLACE "||" "|unused|" row "${row}")
    string(REPLACE "|" ";" values "${row}")
    list(GET values 0 system)
    list(GET values 1 processor)
    list(GET values 2 libc)
    list(GET values 3 expected_platform)
    list(GET values 4 expected_arch)
    capsid_package_platform("${system}" "${processor}" "${libc}" platform arch)
    if(NOT platform STREQUAL expected_platform OR NOT arch STREQUAL expected_arch)
        message(FATAL_ERROR "package target ${row} produced ${platform}/${arch}")
    endif()
endforeach()

foreach(bad_arch IN ITEMS i686 x86 armv7l riscv64)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DCAPSID_NEGATIVE_ARCH=${bad_arch}"
        -P "${CMAKE_CURRENT_LIST_FILE}" RESULT_VARIABLE result ERROR_VARIABLE error)
    if(result EQUAL 0 OR NOT error MATCHES "unsupported Linux package architecture")
        message(FATAL_ERROR "architecture negative control failed: ${bad_arch}: ${error}")
    endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -DCAPSID_NEGATIVE_LIBC=unknown
    -P "${CMAKE_CURRENT_LIST_FILE}" RESULT_VARIABLE result ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT error MATCHES "unsupported Linux package libc")
    message(FATAL_ERROR "libc negative control failed: ${error}")
endif()
message(STATUS "package platform naming: 9 positive / 5 negative controls passed")
