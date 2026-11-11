cmake_minimum_required(VERSION 3.20)

foreach(required CAPSID_SOURCE_DIR CAPSID_WORK_DIR CAPSID_TEST_GENERATOR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "package freshness controls require ${required}")
    endif()
endforeach()

# Synthetic archives exercise the actual contents gate and package build
# target. They are presence/freshness controls, not executable distribution
# artifacts or application smoke evidence. Every project is owned by this run.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
get_filename_component(work_root "${CAPSID_WORK_DIR}" ABSOLUTE)
set(run "${work_root}/run-${run_id}")
file(MAKE_DIRECTORY "${run}")
set(gate "${CAPSID_SOURCE_DIR}/tests/test_package_contents.cmake")
set(basename capsid-freshness-control)
set(failures)

set(project [=[
cmake_minimum_required(VERSION 3.20)
project(PackageFreshnessControl NONE)
add_custom_target(package
    COMMAND "${CMAKE_COMMAND}" "-DSEED=${CAPSID_CONTROL_SEED}"
        "-DBAD_SEED=${CAPSID_CONTROL_BAD_SEED}"
        "-DBUILD=${CMAKE_BINARY_DIR}" "-DEXT=${CAPSID_CONTROL_EXT}"
        "-DMODE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/mode.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/emit.cmake"
    VERBATIM)
]=])
set(emitter [=[
file(READ "${MODE_FILE}" mode)
set(current "${BUILD}/capsid-freshness-control.${EXT}")
if(mode STREQUAL "no-op" OR mode STREQUAL "no-op-stale")
    return()
elseif(mode STREQUAL "target-failure")
    message(FATAL_ERROR "intentional package target failure")
elseif(mode STREQUAL "foreign")
    configure_file("${SEED}" "${BUILD}/third-party-fixture.${EXT}" COPYONLY)
elseif(mode STREQUAL "wrong-name")
    configure_file("${SEED}" "${BUILD}/capsid-wrong-name.${EXT}" COPYONLY)
elseif(mode STREQUAL "missing-manifest")
    configure_file("${BAD_SEED}" "${current}" COPYONLY)
else()
    # COPYONLY, like copy_if_different, leaves an identical existing archive's
    # bytes and mtime unchanged. A successful repeat must not depend on mtime.
    configure_file("${SEED}" "${current}" COPYONLY)
    if(mode STREQUAL "with-foreign")
        configure_file("${SEED}" "${BUILD}/third-party-fixture.${EXT}" COPYONLY)
    elseif(mode STREQUAL "multiple")
        configure_file("${SEED}" "${BUILD}/capsid-extra.${EXT}" COPYONLY)
    endif()
endif()
]=])

foreach(extension zip tar.gz)
    set(area "${run}/${extension}")
    set(source "${area}/source")
    set(build "${area}/build")
    set(payload "${area}/payload/${basename}")
    file(MAKE_DIRECTORY "${source}")
    foreach(entry bin/capsid-worker bin/capsid-worker.exe
            bin/capsid-bytecode-compile bin/capsid-bytecode-compile.exe
            bin/capsid-host bin/capsid-host.exe
            lib/libcapsid_runtime.a lib/capsid_runtime.lib
            include/capsid/runtime.h include/capsid/runtime.hpp
            lib/cmake/Capsid/CapsidTargets.cmake
            lib/cmake/Capsid/CapsidConfig.cmake
            lib/cmake/Capsid/CapsidConfigVersion.cmake
            share/licenses/capsid/LICENSE
            share/licenses/capsid/web-streams-polyfill/LICENSE share/doc/capsid/README.md
            share/capsid/build-info.txt share/capsid/SBOM.spdx.json)
        get_filename_component(parent "${payload}/${entry}" DIRECTORY)
        file(MAKE_DIRECTORY "${parent}")
        file(WRITE "${payload}/${entry}" "synthetic freshness fixture: ${entry}\n")
    endforeach()
    set(seed "${area}/${basename}.${extension}")
    set(bad_seed "${area}/missing-manifest.${extension}")
    if(extension STREQUAL "zip")
        set(tar_flags cf)
        set(tar_format --format=zip)
    else()
        set(tar_flags czf)
        set(tar_format --format=gnutar)
    endif()
    foreach(kind valid missing-manifest)
        set(output "${seed}")
        if(kind STREQUAL "missing-manifest")
            file(REMOVE "${payload}/include/capsid/runtime.h")
            set(output "${bad_seed}")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E tar "${tar_flags}"
            "${output}" "${tar_format}" "${basename}"
            WORKING_DIRECTORY "${area}/payload"
            RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
        if(NOT result STREQUAL "0")
            message(FATAL_ERROR "control archive setup failed: ${out}${err}")
        endif()
    endforeach()
    file(SHA256 "${seed}" seed_hash)
    file(WRITE "${source}/CMakeLists.txt" "${project}")
    file(WRITE "${source}/emit.cmake" "${emitter}")
    set(configure "${CMAKE_COMMAND}" -S "${source}" -B "${build}"
        -G "${CAPSID_TEST_GENERATOR}"
        "-DCAPSID_CONTROL_SEED=${seed}" "-DCAPSID_CONTROL_BAD_SEED=${bad_seed}"
        "-DCAPSID_CONTROL_EXT=${extension}")
    if(CAPSID_TEST_MAKE_PROGRAM)
        list(APPEND configure "-DCMAKE_MAKE_PROGRAM=${CAPSID_TEST_MAKE_PROGRAM}")
    endif()
    execute_process(COMMAND ${configure} RESULT_VARIABLE result
        OUTPUT_VARIABLE out ERROR_VARIABLE err)
    file(WRITE "${area}/configure.log" "${out}${err}")
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "control project setup failed; see ${area}/configure.log")
    endif()
    # The identity stub belongs to the fixture, not CPack. Write it only after
    # configuring the custom target: CMake reserves "package" when a CPack
    # configuration is already present during target generation.
    file(WRITE "${build}/CPackConfig.cmake"
        "set(CPACK_PACKAGE_FILE_NAME \"${basename}\")\n")

    foreach(case fresh identical stale-foreign no-op no-op-stale foreign
            with-foreign wrong-name multiple target-failure missing-manifest)
        # Only remove files inside this run's private build, never a product
        # build's outputs. Resolve the target before each filesystem mutation.
        get_filename_component(real_run "${run}" REALPATH)
        get_filename_component(real_build "${build}" REALPATH)
        file(RELATIVE_PATH relative "${real_run}" "${real_build}")
        if(IS_ABSOLUTE "${relative}" OR relative MATCHES "^\.\.(/|$)" OR relative STREQUAL "")
            message(FATAL_ERROR "private control build escaped its run directory")
        endif()
        file(GLOB old "${build}/*.zip" "${build}/*.tar.gz")
        if(old)
            file(REMOVE ${old})
        endif()
        set(previous)
        if(case STREQUAL "identical" OR case STREQUAL "stale-foreign"
                OR case STREQUAL "no-op-stale" OR case STREQUAL "target-failure")
            file(COPY "${seed}" DESTINATION "${build}")
            list(APPEND previous "${basename}.${extension}")
        endif()
        if(case STREQUAL "stale-foreign")
            configure_file("${seed}" "${build}/third-party-old.${extension}" COPYONLY)
            list(APPEND previous "third-party-old.${extension}")
        endif()
        file(WRITE "${source}/mode.txt" "${case}")
        set(case_work "${build}/contents-${case}")
        set(expected "")
        if(case STREQUAL "no-op" OR case STREQUAL "no-op-stale")
            set(expected "package target produced no new Capsid archive")
        elseif(case STREQUAL "foreign" OR case STREQUAL "with-foreign")
            set(expected "package target produced a third-party archive")
        elseif(case STREQUAL "wrong-name")
            set(expected "unexpected Capsid archive")
        elseif(case STREQUAL "multiple")
            set(expected "expected exactly one new Capsid archive")
        elseif(case STREQUAL "target-failure")
            set(expected "package target failed")
        elseif(case STREQUAL "missing-manifest")
            set(expected "is missing")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" "-DCAPSID_BUILD_DIR=${build}"
            "-DCAPSID_WORK_DIR=${case_work}" -DCAPSID_HOST_TARGET=ON -P "${gate}"
            RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
        file(WRITE "${area}/${case}.log" "${out}${err}")
        set(ok TRUE)
        if(expected)
            string(FIND "${out}${err}" "${expected}" found)
            if(result STREQUAL "0" OR found EQUAL -1)
                set(ok FALSE)
            endif()
        elseif(NOT result STREQUAL "0")
            set(ok FALSE)
        endif()
        if(case STREQUAL "missing-manifest" AND NOT "${out}${err}" MATCHES "include/capsid/runtime.h")
            set(ok FALSE)
        endif()
        # The previous inputs must remain available after success or failure;
        # accepting their bytes at the output location is not fresh production.
        foreach(name IN LISTS previous)
            file(GLOB_RECURSE preserved "${case_work}/run-*/previous-archives/${name}")
            list(LENGTH preserved preserved_count)
            if(NOT preserved_count EQUAL 1)
                set(ok FALSE)
            else()
                list(GET preserved 0 preserved_file)
                file(SHA256 "${preserved_file}" preserved_hash)
                if(NOT preserved_hash STREQUAL seed_hash)
                    set(ok FALSE)
                endif()
            endif()
        endforeach()
        if(ok)
            message(STATUS "PASS: package freshness control ${extension}/${case}")
        else()
            list(APPEND failures "${extension}/${case}")
            message(STATUS "FAIL: package freshness control ${extension}/${case}; see ${area}/${case}.log")
        endif()
        if(case STREQUAL "fresh" AND NOT ok)
            message(FATAL_ERROR "positive baseline failed; no fault control can be credited")
        endif()
    endforeach()

    # Exercise the path checks with private paths. The old gate may erase its
    # input here; the marker checks require rejection before any mutation.
    foreach(case outside-build same-as-build)
        set(case_work "${area}/outside")
        if(case STREQUAL "same-as-build")
            set(case_work "${build}")
        endif()
        file(MAKE_DIRECTORY "${case_work}")
        set(marker "${case_work}/contents-guard-marker.txt")
        file(WRITE "${marker}" "preserve this private control input\n")
        execute_process(COMMAND "${CMAKE_COMMAND}" "-DCAPSID_BUILD_DIR=${build}"
            "-DCAPSID_WORK_DIR=${case_work}" -DCAPSID_HOST_TARGET=ON -P "${gate}"
            RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
        file(WRITE "${area}/${case}.log" "${out}${err}")
        string(FIND "${out}${err}" "must be a strict descendant" found)
        set(ok TRUE)
        if(result STREQUAL "0" OR found EQUAL -1 OR NOT EXISTS "${marker}")
            set(ok FALSE)
        else()
            file(READ "${marker}" marker_text)
            if(NOT marker_text STREQUAL "preserve this private control input\n")
                set(ok FALSE)
            endif()
        endif()
        if(ok)
            message(STATUS "PASS: package freshness control ${extension}/${case}")
        else()
            list(APPEND failures "${extension}/${case}")
            message(STATUS "FAIL: package freshness control ${extension}/${case}; see ${area}/${case}.log")
        endif()
    endforeach()
endforeach()
if(failures)
    message(FATAL_ERROR "package freshness controls failed: ${failures}")
endif()
message(STATUS "PASS: all 26 package freshness controls; evidence ${run}")
