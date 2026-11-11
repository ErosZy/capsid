# Compare extracted package trees without running a compiler. The build driver
# and the focused artifact controls exercise the same comparison routine.
include_guard(GLOBAL)

function(capsid_read_build_info_header root output_lines)
    file(STRINGS "${root}/share/capsid/build-info.txt" lines)
    list(POP_FRONT lines schema)
    if(NOT schema STREQUAL "capsid-build-info-v1")
        message(FATAL_ERROR "unsupported package build-info schema: ${schema}")
    endif()
    set(header)
    foreach(line IN LISTS lines)
        # The canonical records below this boundary legitimately repeat header
        # keys. Only the header has the exactly-once identity contract.
        if(line MATCHES "^--- ")
            break()
        endif()
        list(APPEND header "${line}")
    endforeach()
    set(${output_lines} "${header}" PARENT_SCOPE)
endfunction()

function(capsid_read_verified_package_manifest root output_names)
    file(STRINGS "${root}/share/capsid/FILE-MANIFEST.txt" lines)
    if(NOT lines)
        message(FATAL_ERROR "package manifest is empty: ${root}")
    endif()
    set(names)
    foreach(line IN LISTS lines)
        string(REPLACE "\t" ";" fields "${line}")
        list(LENGTH fields field_count)
        if(NOT field_count EQUAL 3)
            message(FATAL_ERROR "package manifest must contain name, size and checksum: ${line}")
        endif()
        list(GET fields 0 name)
        list(GET fields 1 size)
        list(GET fields 2 checksum)
        if(IS_ABSOLUTE "${name}" OR name MATCHES "(^|/)\\.\\.(/|$)"
                OR name MATCHES "\\\\")
            message(FATAL_ERROR "package manifest path must stay inside the package: ${name}")
        endif()
        list(FIND names "${name}" duplicate_index)
        if(NOT duplicate_index EQUAL -1)
            message(FATAL_ERROR "duplicate package manifest entry: ${name}")
        endif()
        if(NOT EXISTS "${root}/${name}" OR IS_DIRECTORY "${root}/${name}")
            message(FATAL_ERROR "package manifest entry is not a file: ${name}")
        endif()
        string(LENGTH "${checksum}" checksum_length)
        if(NOT size MATCHES "^[0-9]+$" OR NOT checksum MATCHES "^[0-9a-f]+$"
                OR NOT checksum_length EQUAL 64)
            message(FATAL_ERROR "package manifest size/checksum is malformed: ${name}")
        endif()
        file(SIZE "${root}/${name}" actual_size)
        file(SHA256 "${root}/${name}" actual_checksum)
        if(NOT size EQUAL actual_size)
            message(FATAL_ERROR "package manifest size differs: ${name}")
        endif()
        if(NOT checksum STREQUAL actual_checksum)
            message(FATAL_ERROR "package manifest checksum differs: ${name}")
        endif()
        list(APPEND names "${name}")
    endforeach()
    file(GLOB_RECURSE actual_names LIST_DIRECTORIES FALSE RELATIVE "${root}" "${root}/*")
    list(REMOVE_ITEM actual_names "share/capsid/FILE-MANIFEST.txt" "share/capsid/SBOM.spdx.json")
    list(SORT actual_names)
    list(SORT names)
    if(NOT names STREQUAL actual_names)
        message(FATAL_ERROR "package manifest does not list the complete file inventory: ${root}")
    endif()
    set(${output_names} "${names}" PARENT_SCOPE)
endfunction()

function(capsid_compare_package_trees
        CAPSID_REPRO_BASELINE_ROOT CAPSID_REPRO_ROOT CAPSID_REPRO_DIFFS)
    file(WRITE "${CAPSID_REPRO_DIFFS}" "capsid reproducibility differences "
        "(allowed: toolchain not bit-reproducible; identity must match)\n")
    # ---- validate each manifest, then compare file NAME lists ---------------------
    # FILE-MANIFEST.txt lists every package file except itself and the SBOM
    # (both embed per-build binary hashes by design).
    capsid_read_verified_package_manifest("${CAPSID_REPRO_ROOT}" CAPSID_REPRO_NAMES)
    capsid_read_verified_package_manifest("${CAPSID_REPRO_BASELINE_ROOT}" CAPSID_REPRO_BASELINE_NAMES)
    list(LENGTH CAPSID_REPRO_NAMES repro_name_count)
    list(LENGTH CAPSID_REPRO_BASELINE_NAMES baseline_name_count)
    if(NOT repro_name_count EQUAL baseline_name_count)
        message(FATAL_ERROR "package file counts differ: baseline "
            "${baseline_name_count} vs repro ${repro_name_count}")
    endif()
    list(SORT CAPSID_REPRO_NAMES)
    list(SORT CAPSID_REPRO_BASELINE_NAMES)
    if(NOT CAPSID_REPRO_NAMES STREQUAL CAPSID_REPRO_BASELINE_NAMES)
        message(FATAL_ERROR "package file name lists differ:\nbaseline: "
            "${CAPSID_REPRO_BASELINE_NAMES}\nrepro: ${CAPSID_REPRO_NAMES}")
    endif()

    # ---- compare identity records ------------------------------------------------
    set(CAPSID_REPRO_RECORDS
        version commit commitDate buildId compatibilityId featureFlags
        cmakeBuildType)
    capsid_read_build_info_header("${CAPSID_REPRO_BASELINE_ROOT}" CAPSID_REPRO_BASELINE_INFO_LINES)
    capsid_read_build_info_header("${CAPSID_REPRO_ROOT}" CAPSID_REPRO_INFO_LINES)
    foreach(record IN LISTS CAPSID_REPRO_RECORDS)
        set(baseline_value)
        set(repro_value)
        set(baseline_count 0)
        set(repro_count 0)
        # cmakeBuildType may legitimately be empty; the other records never are.
        set(record_regex "^${record}=(.*)$")
        foreach(line IN LISTS CAPSID_REPRO_BASELINE_INFO_LINES)
            if(line MATCHES "${record_regex}")
                set(baseline_value "${CMAKE_MATCH_1}")
                math(EXPR baseline_count "${baseline_count} + 1")
            endif()
        endforeach()
        foreach(line IN LISTS CAPSID_REPRO_INFO_LINES)
            if(line MATCHES "${record_regex}")
                set(repro_value "${CMAKE_MATCH_1}")
                math(EXPR repro_count "${repro_count} + 1")
            endif()
        endforeach()
        if(NOT baseline_count EQUAL 1 OR NOT repro_count EQUAL 1)
            message(FATAL_ERROR "identity record ${record} must appear exactly once "
                "in each package: baseline ${baseline_count}, repro ${repro_count}")
        endif()
        if(NOT record STREQUAL "cmakeBuildType" AND
                ("${baseline_value}" STREQUAL "" OR "${repro_value}" STREQUAL ""))
            message(FATAL_ERROR "identity record ${record} must be nonempty")
        endif()
        # Quoted comparison: an unquoted empty value would collapse the STREQUAL
        # expression into a truthy variable-name check.
        if(NOT "${baseline_value}" STREQUAL "${repro_value}")
            message(FATAL_ERROR "identity record ${record} differs: baseline "
                "[${baseline_value}] vs repro [${repro_value}]")
        endif()
    endforeach()

    # The whole file must also be byte-identical: this subsumes the field checks
    # and covers the bytecode-compatibility and build-provenance records too.
    file(SHA256 "${CAPSID_REPRO_BASELINE_ROOT}/share/capsid/build-info.txt"
        CAPSID_REPRO_BASELINE_INFO_SHA)
    file(SHA256 "${CAPSID_REPRO_ROOT}/share/capsid/build-info.txt"
        CAPSID_REPRO_INFO_SHA)
    if(NOT CAPSID_REPRO_BASELINE_INFO_SHA STREQUAL CAPSID_REPRO_INFO_SHA)
        message(FATAL_ERROR "build-info.txt differs beyond the identity records:\n"
            "baseline ${CAPSID_REPRO_BASELINE_INFO_SHA}\n"
            "repro    ${CAPSID_REPRO_INFO_SHA}")
    endif()

    # ---- compare SBOM identity fields --------------------------------------------
    # The SBOM itself embeds per-build binary hashes (package verification code),
    # so only its identity fields are byte-compared.
    file(READ "${CAPSID_REPRO_ROOT}/share/capsid/SBOM.spdx.json"
        CAPSID_REPRO_SBOM)
    file(READ "${CAPSID_REPRO_BASELINE_ROOT}/share/capsid/SBOM.spdx.json"
        CAPSID_REPRO_BASELINE_SBOM)
    foreach(sbom_field IN ITEMS name spdxVersion documentNamespace)
        string(JSON baseline_value GET "${CAPSID_REPRO_BASELINE_SBOM}"
            "${sbom_field}")
        string(JSON repro_value GET "${CAPSID_REPRO_SBOM}" "${sbom_field}")
        if(NOT baseline_value STREQUAL repro_value)
            message(FATAL_ERROR "SBOM field ${sbom_field} differs: baseline "
                "[${baseline_value}] vs repro [${repro_value}]")
        endif()
    endforeach()
    string(JSON baseline_created GET "${CAPSID_REPRO_BASELINE_SBOM}"
        creationInfo created)
    string(JSON repro_created GET "${CAPSID_REPRO_SBOM}" creationInfo created)
    if(NOT baseline_created STREQUAL repro_created)
        message(FATAL_ERROR "SBOM creationInfo.created differs: baseline "
            "[${baseline_created}] vs repro [${repro_created}]")
    endif()

    # ---- deterministic text files must hash identically --------------------------
    # Everything except FILE-MANIFEST.txt and SBOM.spdx.json (self-listed binary
    # hash carriers).
    set(CAPSID_REPRO_TEXT_FILES)
    foreach(manifest_name IN LISTS CAPSID_REPRO_NAMES)
        if(manifest_name MATCHES
                "^(include/|share/doc/|share/licenses/|lib/cmake/)"
                OR manifest_name STREQUAL "share/capsid/build-info.txt")
            list(APPEND CAPSID_REPRO_TEXT_FILES "${manifest_name}")
        endif()
    endforeach()
    foreach(text_file IN LISTS CAPSID_REPRO_TEXT_FILES)
        file(SHA256 "${CAPSID_REPRO_ROOT}/${text_file}" repro_sha)
        file(SHA256 "${CAPSID_REPRO_BASELINE_ROOT}/${text_file}" baseline_sha)
        if(NOT repro_sha STREQUAL baseline_sha)
            message(FATAL_ERROR "deterministic file differs: ${text_file}\n"
                "baseline ${baseline_sha}\nrepro ${repro_sha}")
        endif()
    endforeach()

    # ---- record binary differences -----------------------------------------------
    set(CAPSID_REPRO_DIFF_COUNT 0)
    foreach(manifest_name IN LISTS CAPSID_REPRO_NAMES)
        if(manifest_name MATCHES
                "^(include/|share/doc/|share/licenses/|lib/cmake/)"
                OR manifest_name STREQUAL "share/capsid/build-info.txt")
            continue()
        endif()
        file(SHA256 "${CAPSID_REPRO_ROOT}/${manifest_name}" repro_sha)
        file(SHA256 "${CAPSID_REPRO_BASELINE_ROOT}/${manifest_name}"
            baseline_sha)
        if(NOT repro_sha STREQUAL baseline_sha)
            math(EXPR CAPSID_REPRO_DIFF_COUNT "${CAPSID_REPRO_DIFF_COUNT} + 1")
            file(SIZE "${CAPSID_REPRO_ROOT}/${manifest_name}" repro_size)
            file(SIZE "${CAPSID_REPRO_BASELINE_ROOT}/${manifest_name}"
                baseline_size)
            file(APPEND "${CAPSID_REPRO_DIFFS}"
                "${manifest_name}\n  baseline ${baseline_sha} (${baseline_size}B)\n"
                "  repro    ${repro_sha} (${repro_size}B)\n")
        endif()
    endforeach()

    message(STATUS "PASS: reproducibility — ${baseline_name_count} files, "
        "identity and deterministic text identical; "
        "${CAPSID_REPRO_DIFF_COUNT} binary hash differences recorded at "
        "${CAPSID_REPRO_DIFFS}")
endfunction()
