# System c-ares only, like Boost/OpenSSL: no configure-time downloads.
# 1.27 is available in Ubuntu 24.04. Do not silently fall back to the
# blocking system resolver: that would restore unbounded DNS shutdown.
find_package(c-ares 1.27 CONFIG QUIET)
if(TARGET c-ares::cares_static)
    set(CAPSID_CARES_TARGET c-ares::cares_static)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND
   CMAKE_EXE_LINKER_FLAGS MATCHES "(^| )-static( |$)")
    find_path(CAPSID_CARES_INCLUDE_DIR NAMES ares.h REQUIRED)
    find_library(CAPSID_CARES_LIBRARY NAMES libcares.a libcares_static.a REQUIRED)
    set(CAPSID_CARES_USE_MANUAL ON)
elseif(TARGET c-ares::cares)
    set(CAPSID_CARES_TARGET c-ares::cares)
else()
    find_path(CAPSID_CARES_INCLUDE_DIR NAMES ares.h REQUIRED)
    find_library(CAPSID_CARES_LIBRARY NAMES cares cares_static REQUIRED)
    set(CAPSID_CARES_USE_MANUAL ON)
endif()
if(CAPSID_CARES_USE_MANUAL)
    file(STRINGS "${CAPSID_CARES_INCLUDE_DIR}/ares_version.h"
        CAPSID_CARES_VERSION_LINE REGEX "^#define ARES_VERSION_STR ")
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+"
        CAPSID_CARES_VERSION "${CAPSID_CARES_VERSION_LINE}")
    if(NOT CAPSID_CARES_VERSION OR CAPSID_CARES_VERSION VERSION_LESS 1.27)
        message(FATAL_ERROR "Host networking requires system c-ares >= 1.27")
    endif()
    add_library(capsid_cares UNKNOWN IMPORTED)
    set_target_properties(capsid_cares PROPERTIES
        IMPORTED_LOCATION "${CAPSID_CARES_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CAPSID_CARES_INCLUDE_DIR}")
    set(CAPSID_CARES_TARGET capsid_cares)
endif()
target_link_libraries(capsid_host_core PRIVATE ${CAPSID_CARES_TARGET})
get_target_property(CAPSID_CARES_PROVIDER_INCLUDES ${CAPSID_CARES_TARGET}
    INTERFACE_INCLUDE_DIRECTORIES)
if(NOT CAPSID_CARES_PROVIDER_INCLUDES)
    set(CAPSID_CARES_PROVIDER_INCLUDES "${c-ares_INCLUDE_DIR}")
endif()
# Another dependency (e.g. Boost in a conda prefix) can put a different
# ares.h ahead of implicit /usr/include. Bind the private wrapper to this
# provider's header, whose quoted sibling includes also stay in its prefix.
unset(CAPSID_CARES_PROVIDER_DIR CACHE)
find_path(CAPSID_CARES_PROVIDER_DIR NAMES ares.h
    PATHS ${CAPSID_CARES_PROVIDER_INCLUDES} NO_DEFAULT_PATH REQUIRED)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/host_dns_provider.h.in"
    "${CAPSID_GENERATED_DIR}/host_dns_provider.h" @ONLY)
target_include_directories(capsid_host_core PRIVATE "${CAPSID_GENERATED_DIR}")
target_sources(capsid_host_core PRIVATE src/host/async_resolver.cc)
