if(NOT DEFINED CAPSID_WORKER OR NOT DEFINED CAPSID_RUNTIME_ARCHIVE)
    message(FATAL_ERROR "CAPSID_WORKER and CAPSID_RUNTIME_ARCHIVE are required")
endif()

get_filename_component(CAPSID_WORKER_NAME "${CAPSID_WORKER}" NAME)
get_filename_component(CAPSID_RUNTIME_NAME "${CAPSID_RUNTIME_ARCHIVE}" NAME)

foreach(CAPSID_ARTIFACT_NAME IN ITEMS
        "${CAPSID_WORKER_NAME}"
        "${CAPSID_RUNTIME_NAME}")
    string(TOLOWER "${CAPSID_ARTIFACT_NAME}" CAPSID_ARTIFACT_NAME_LOWER)
    string(CONCAT CAPSID_LEGACY_PRODUCT_NAME "win" "ter")
    if(CAPSID_ARTIFACT_NAME_LOWER MATCHES "${CAPSID_LEGACY_PRODUCT_NAME}")
        message(FATAL_ERROR
            "public artifact leaks the legacy product name: "
            "${CAPSID_ARTIFACT_NAME}")
    endif()
    if(NOT CAPSID_ARTIFACT_NAME_LOWER MATCHES "capsid")
        message(FATAL_ERROR
            "public artifact does not use the Capsid name: "
            "${CAPSID_ARTIFACT_NAME}")
    endif()
endforeach()
