# Ninja compares /showIncludes output byte for byte. CMake stores the
# measured prefix as UTF-8. Its AUTO decoder follows the input code page,
# which can differ from the output code page used by cl.exe. Decode the raw
# compiler output with the actual output code page before generating Ninja.
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
    return()
endif()

set(_capsid_prefix_dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/capsid-msvc-prefix")
file(MAKE_DIRECTORY "${_capsid_prefix_dir}")
file(WRITE "${_capsid_prefix_dir}/decode.c" [=[
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t **argv) {
    FILE *input = NULL;
    long length;
    char *raw, *utf8;
    wchar_t *wide;
    int wide_size, utf8_size;
    UINT code_page = GetConsoleOutputCP();
    if (argc != 2 || _wfopen_s(&input, argv[1], L"rb") != 0) return 1;
    if (fseek(input, 0, SEEK_END) != 0) return 2;
    length = ftell(input);
    if (length <= 0 || length > 1048576 || fseek(input, 0, SEEK_SET) != 0) return 3;
    raw = (char *)malloc((size_t)length);
    if (!raw || fread(raw, 1, (size_t)length, input) != (size_t)length) return 4;
    fclose(input);
    if (code_page == 0) code_page = GetACP();
    wide_size = MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, raw, (int)length, NULL, 0);
    if (wide_size <= 0) return 5;
    wide = (wchar_t *)malloc((size_t)wide_size * sizeof(wchar_t));
    if (!wide || MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, raw, (int)length, wide, wide_size) != wide_size) return 6;
    utf8_size = WideCharToMultiByte(CP_UTF8, 0, wide, wide_size, NULL, 0, NULL, NULL);
    if (utf8_size <= 0) return 7;
    utf8 = (char *)malloc((size_t)utf8_size);
    if (!utf8 || WideCharToMultiByte(CP_UTF8, 0, wide, wide_size, utf8, utf8_size, NULL, NULL) != utf8_size) return 8;
    if (fwrite(utf8, 1, (size_t)utf8_size, stdout) != (size_t)utf8_size) return 9;
    free(utf8);
    free(wide);
    free(raw);
    return 0;
}
]=])
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" /nologo /MT /W4 /WX /TC
        "${_capsid_prefix_dir}/decode.c" "/Fo${_capsid_prefix_dir}/decode.obj"
        "/Fe${_capsid_prefix_dir}/decode.exe" /link kernel32.lib
    WORKING_DIRECTORY "${_capsid_prefix_dir}"
    RESULT_VARIABLE _capsid_decode_build_result
    OUTPUT_VARIABLE _capsid_decode_build_stdout
    ERROR_VARIABLE _capsid_decode_build_stderr
    ENCODING NONE
)
if(NOT _capsid_decode_build_result EQUAL 0)
    message(FATAL_ERROR "MSVC dependency decoder build failed: ${_capsid_decode_build_stdout}${_capsid_decode_build_stderr}")
endif()
file(WRITE "${_capsid_prefix_dir}/capsid-msvc-prefix-marker.h" "/* dependency prefix probe */\n")
file(WRITE "${_capsid_prefix_dir}/probe.c" "#include \"capsid-msvc-prefix-marker.h\"\n")
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" /nologo /showIncludes /c /TC
        "${_capsid_prefix_dir}/probe.c" "/Fo${_capsid_prefix_dir}/probe.obj"
    RESULT_VARIABLE _capsid_prefix_result
    OUTPUT_VARIABLE _capsid_prefix_stdout
    ERROR_VARIABLE _capsid_prefix_stderr
    ENCODING NONE
)
if(NOT _capsid_prefix_result EQUAL 0)
    message(FATAL_ERROR "MSVC dependency prefix probe failed: ${_capsid_prefix_stderr}")
endif()
file(WRITE "${_capsid_prefix_dir}/includes.bin" "${_capsid_prefix_stdout}")
execute_process(
    COMMAND "${_capsid_prefix_dir}/decode.exe" "${_capsid_prefix_dir}/includes.bin"
    RESULT_VARIABLE _capsid_decode_result
    OUTPUT_VARIABLE _capsid_prefix_decoded
    ERROR_VARIABLE _capsid_decode_stderr
    ENCODING UTF-8
)
if(NOT _capsid_decode_result EQUAL 0)
    message(FATAL_ERROR "MSVC dependency prefix decoding failed (${_capsid_decode_result}): ${_capsid_decode_stderr}")
endif()
string(REGEX MATCH "(^|[\r\n])([^\r\n]*)[A-Za-z]:[/\\][^\r\n]*capsid-msvc-prefix-marker[.]h"
    _capsid_prefix_match "${_capsid_prefix_decoded}")
if(NOT _capsid_prefix_match OR CMAKE_MATCH_2 STREQUAL "")
    message(FATAL_ERROR "MSVC /showIncludes did not identify the dependency prefix probe header")
endif()
set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${CMAKE_C_CL_SHOWINCLUDES_PREFIX}")
set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_C_CL_SHOWINCLUDES_PREFIX}")

# Fixing the prefix only affects new compilations. Ninja still considers old
# objects with an empty dependency record valid after a header changes. Retire
# those objects once per compiler/prefix revision so an existing build also
# acquires real dependencies. Keep the dependency database and other products.
function(_capsid_migrate_msvc_dependencies)
    string(SHA256 _key
        "2|${CMAKE_C_COMPILER}|${CMAKE_C_COMPILER_VERSION}|${CMAKE_CXX_COMPILER}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_C_CL_SHOWINCLUDES_PREFIX}")
    set(_stamp "${_capsid_prefix_dir}/dependency-migration.txt")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _previous)
        if(_previous STREQUAL _key)
            return()
        endif()
    endif()
    set(_objects)
    if(EXISTS "${CMAKE_BINARY_DIR}/.ninja_deps")
        execute_process(COMMAND "${CMAKE_MAKE_PROGRAM}" -C "${CMAKE_BINARY_DIR}" -t deps
            RESULT_VARIABLE _result OUTPUT_VARIABLE _deps ERROR_VARIABLE _error ENCODING NONE)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "Cannot inspect existing MSVC header dependencies: ${_error}")
        endif()
        string(REPLACE "\r\n" "\n" _deps "${_deps}")
        string(REGEX MATCHALL "(^|\n)[^\n]+[.]obj: #deps 0, [^\n]*" _records "${_deps}")
        get_filename_component(_build_real "${CMAKE_BINARY_DIR}" REALPATH)
        foreach(_record IN LISTS _records)
            string(REGEX REPLACE "^\n?(.+[.]obj): #deps 0, [^\n]*$" "\\1" _object "${_record}")
            get_filename_component(_absolute "${_object}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
            if(EXISTS "${_absolute}")
                get_filename_component(_real "${_absolute}" REALPATH)
                file(RELATIVE_PATH _relative "${_build_real}" "${_real}")
                if(IS_ABSOLUTE "${_relative}" OR _relative MATCHES "^[.][.](/|$)")
                    message(FATAL_ERROR "Refusing to invalidate an MSVC object outside the build directory: ${_real}")
                endif()
                list(APPEND _objects "${_real}")
            endif()
        endforeach()
    endif()
    # Validate all paths before removing any individual object.
    foreach(_object IN LISTS _objects)
        file(REMOVE "${_object}")
    endforeach()
    list(LENGTH _objects _count)
    file(WRITE "${_capsid_prefix_dir}/dependency-migration-objects.txt" "${_objects}\n")
    file(WRITE "${_stamp}" "${_key}")
    if(_count GREATER 0)
        message(STATUS "Rebuilding ${_count} MSVC objects with missing header dependencies")
    endif()
endfunction()
_capsid_migrate_msvc_dependencies()
unset(_capsid_prefix_dir)
unset(_capsid_prefix_result)
unset(_capsid_prefix_stdout)
unset(_capsid_prefix_stderr)
unset(_capsid_prefix_match)
unset(_capsid_prefix_decoded)
unset(_capsid_decode_build_result)
unset(_capsid_decode_build_stdout)
unset(_capsid_decode_build_stderr)
unset(_capsid_decode_result)
unset(_capsid_decode_stderr)
