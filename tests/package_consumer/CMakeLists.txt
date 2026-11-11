cmake_minimum_required(VERSION 3.18)
project(CapsidPackageConsumer LANGUAGES C CXX)

# This is an independent project: no add_subdirectory, source headers,
# hand-written sanitizer switches, or manually selected runtime library.
find_package(Capsid ${CAPSID_EXPECTED_VERSION} EXACT CONFIG REQUIRED
    PATHS "${CMAKE_PREFIX_PATH}" NO_DEFAULT_PATH)
add_executable(sample_c package_smoke_sample.c)
add_executable(sample_cpp package_smoke_sample.cc)
target_compile_features(sample_c PRIVATE c_std_11)
target_compile_features(sample_cpp PRIVATE cxx_std_17)
set(samples sample_c sample_cpp)
if(WIN32 AND Capsid_RUNTIME_DLLS)
    add_executable(runtime_origin runtime_origin.cc)
    target_compile_features(runtime_origin PRIVATE cxx_std_17)
    list(APPEND samples runtime_origin)
endif()
foreach(sample IN LISTS samples)
    target_link_libraries(${sample} PRIVATE Capsid::capsid_runtime)
    if(MSVC)
        target_compile_options(${sample} PRIVATE /W4 /WX /utf-8)
        # The sample deliberately uses portable ISO C stdio calls.
        target_compile_definitions(${sample} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${sample} PRIVATE -Wall -Wextra -Werror)
    endif()
    if(Capsid_RUNTIME_DLLS)
        add_custom_command(TARGET ${sample} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${Capsid_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${sample}>"
            VERBATIM)
    endif()
endforeach()
