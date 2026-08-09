# Package artifact generation (remediation spec §12.2/§12.3).
#
# Runs at INSTALL time (via install(CODE) in CapsidPackage.cmake) against
# the completed install prefix, so the manifest and SBOM describe exactly
# what ships. Produces:
#
#   <prefix>/share/capsid/FILE-MANIFEST.txt   every file, name <tab> size
#                                             <tab> sha256
#   <prefix>/share/capsid/SBOM.spdx.json      SPDX 2.3 document: the capsid
#                                             package (build_id etc.), the
#                                             vendored dependency packages
#                                             pinned by overlay commit, and
#                                             every shipped file with its
#                                             SHA-256
#
# The two output files exclude themselves (and each other) from the walk.
# 'created' in the SBOM is the capsid commit date, which is deterministic
# per commit — two builds of the same commit produce byte-identical SBOMs.
#
# Usage (script mode, invoked by install(CODE)):
#   cmake -DCAPSID_SBOM_NAME=... -DCAPSID_SBOM_VERSION=...
#         -DCAPSID_SBOM_BUILD_ID=... -DCAPSID_SBOM_COMPAT_ID=...
#         -DCAPSID_SBOM_COMMIT=... -DCAPSID_SBOM_COMMIT_DATE=...
#         -DCAPSID_SBOM_QUICKJS=... -DCAPSID_SBOM_OVERLAY_KEY=...
#         -DCAPSID_SBOM_OVERLAY_MANIFEST=...
#         -DCAPSID_SBOM_PREFIX=<install prefix>
#         -P cmake/GeneratePackageArtifacts.cmake
#
# CMake 4.4 string(JSON) semantics used below (verified against 4.4.0):
#   * there is no document-init mode; SET with json-string "{}" creates the
#     document and each subsequent SET carries the accumulated JSON back in
#   * the SET <value> must itself be valid JSON text — a bare string like
#     SPDX-2.3 is a parse error, so every scalar is passed JSON-escaped
#     ("\"${VAR}\""); arrays and objects pass through as JSON
#   * a multi-argument set() forms a semicolon list; joining JSON fragments
#     must therefore happen inside ONE quoted argument.

foreach(CAPSID_GPA_REQUIRED
        CAPSID_SBOM_NAME CAPSID_SBOM_VERSION CAPSID_SBOM_BUILD_ID
        CAPSID_SBOM_COMPAT_ID CAPSID_SBOM_COMMIT CAPSID_SBOM_COMMIT_DATE
        CAPSID_SBOM_QUICKJS CAPSID_SBOM_OVERLAY_KEY
        CAPSID_SBOM_OVERLAY_MANIFEST CAPSID_SBOM_PREFIX)
    if(NOT DEFINED ${CAPSID_GPA_REQUIRED})
        message(FATAL_ERROR "GeneratePackageArtifacts requires "
            "${CAPSID_GPA_REQUIRED}")
    endif()
    # Fail closed against the install(CODE) quoting hazard: a quoted -D value
    # arrives with literal quote characters, and string(JSON) SET truncates
    # double-quoted JSON to its first valid value instead of erroring — an
    # empty documentNamespace/created would silently ship. Any quote, space
    # or control character in a required value is a hard failure.
    if("${${CAPSID_GPA_REQUIRED}}" MATCHES "[\"\n\r\t ]")
        message(FATAL_ERROR "GeneratePackageArtifacts: ${CAPSID_GPA_REQUIRED} "
            "contains quoting/whitespace (install(CODE) quoting violated): "
            "[${${CAPSID_GPA_REQUIRED}}]")
    endif()
endforeach()

set(CAPSID_GPA_OUT_DIR "${CAPSID_SBOM_PREFIX}/share/capsid")
set(CAPSID_GPA_MANIFEST "${CAPSID_GPA_OUT_DIR}/FILE-MANIFEST.txt")
set(CAPSID_GPA_SBOM "${CAPSID_GPA_OUT_DIR}/SBOM.spdx.json")

# ---- walk the installed tree ------------------------------------------------
set(CAPSID_GPA_ENTRIES)
set(CAPSID_GPA_MANIFEST_LINES)
file(GLOB_RECURSE CAPSID_GPA_ALL_FILES
    "${CAPSID_SBOM_PREFIX}/*")
list(SORT CAPSID_GPA_ALL_FILES)
foreach(CAPSID_GPA_FILE IN LISTS CAPSID_GPA_ALL_FILES)
    get_filename_component(CAPSID_GPA_BASENAME "${CAPSID_GPA_FILE}" NAME)
    if(CAPSID_GPA_BASENAME STREQUAL "FILE-MANIFEST.txt" OR
       CAPSID_GPA_BASENAME STREQUAL "SBOM.spdx.json")
        continue()
    endif()
    file(SHA256 "${CAPSID_GPA_FILE}" CAPSID_GPA_SHA)
    file(SIZE "${CAPSID_GPA_FILE}" CAPSID_GPA_SIZE)
    file(RELATIVE_PATH CAPSID_GPA_REL
        "${CAPSID_SBOM_PREFIX}" "${CAPSID_GPA_FILE}")
    string(APPEND CAPSID_GPA_MANIFEST_LINES
        "${CAPSID_GPA_REL}\t${CAPSID_GPA_SIZE}\t${CAPSID_GPA_SHA}\n")
    list(APPEND CAPSID_GPA_ENTRIES
        "${CAPSID_GPA_REL}|${CAPSID_GPA_SIZE}|${CAPSID_GPA_SHA}")
endforeach()
file(WRITE "${CAPSID_GPA_MANIFEST}" "${CAPSID_GPA_MANIFEST_LINES}")

# ---- SPDX 2.3 document ------------------------------------------------------
# Packages: capsid (the artifact) and the vendored dependencies pinned by
# the overlay — each is identifiable by its locked commit. Files: every
# shipped file with its SHA-256. Relationships connect document → package →
# files and package → dependency packages.
string(JSON CAPSID_GPA_SBOM_JSON SET "{}"
    "spdxVersion" "\"SPDX-2.3\"")
string(JSON CAPSID_GPA_SBOM_JSON SET "${CAPSID_GPA_SBOM_JSON}"
    "dataLicense" "\"CC0-1.0\"")
string(JSON CAPSID_GPA_SBOM_JSON SET "${CAPSID_GPA_SBOM_JSON}"
    "SPDXID" "\"SPDXRef-DOCUMENT\"")
string(JSON CAPSID_GPA_SBOM_JSON SET "${CAPSID_GPA_SBOM_JSON}"
    "name" "\"${CAPSID_SBOM_NAME}-sbom\"")
string(JSON CAPSID_GPA_SBOM_JSON SET "${CAPSID_GPA_SBOM_JSON}"
    "documentNamespace"
    "\"https://capsid.dev/sbom/${CAPSID_SBOM_BUILD_ID}\"")
string(JSON CAPSID_GPA_SBOM_JSON SET "${CAPSID_GPA_SBOM_JSON}"
    "documentDescribes" ["SPDXRef-Package-Capsid"])
# Dotted member names are NOT path navigation in string(JSON) — they create
# flat keys — so nested objects are built bottom-up and inserted as values.
string(JSON CAPSID_GPA_CREATION SET "{}"
    "created" "\"${CAPSID_SBOM_COMMIT_DATE}\"")
string(JSON CAPSID_GPA_CREATION SET "${CAPSID_GPA_CREATION}"
    "creators" ["Tool: capsid-build"])
string(JSON CAPSID_GPA_SBOM_JSON SET "${CAPSID_GPA_SBOM_JSON}"
    "creationInfo" "${CAPSID_GPA_CREATION}")

# capsid package: build_id and compatibility_id are the identity anchors
# the CI evidence index records (spec §12.5).
string(JSON CAPSID_GPA_PKG_CAPSID SET "{}"
    "SPDXID" "\"SPDXRef-Package-Capsid\"")
string(JSON CAPSID_GPA_PKG_CAPSID SET "${CAPSID_GPA_PKG_CAPSID}"
    "name" "\"capsid\"")
string(JSON CAPSID_GPA_PKG_CAPSID SET "${CAPSID_GPA_PKG_CAPSID}"
    "versionInfo" "\"${CAPSID_SBOM_VERSION}\"")
string(JSON CAPSID_GPA_PKG_CAPSID SET "${CAPSID_GPA_PKG_CAPSID}"
    "licenseConcluded" "\"MIT\"")
string(JSON CAPSID_GPA_PKG_CAPSID SET "${CAPSID_GPA_PKG_CAPSID}"
    "downloadLocation" "\"NOASSERTION\"")
string(JSON CAPSID_GPA_PKG_CAPSID SET "${CAPSID_GPA_PKG_CAPSID}"
    "copyrightText" "\"NOASSERTION\"")
string(JSON CAPSID_GPA_PKG_CAPSID SET "${CAPSID_GPA_PKG_CAPSID}"
    "builtDate" "\"${CAPSID_SBOM_COMMIT_DATE}\"")
string(JSON CAPSID_GPA_PKG_CAPSID SET "${CAPSID_GPA_PKG_CAPSID}"
    "comment"
    "\"build_id=${CAPSID_SBOM_BUILD_ID} compatibility_id=${CAPSID_SBOM_COMPAT_ID}\"")

# Vendored dependency packages pinned by the overlay. Commit hashes are the
# txiki.js overlay values from the locked manifest (vendor + quickjs), the
# overlay key/manifest identify the patched set.
string(JSON CAPSID_GPA_PKG_TXIKI SET "{}"
    "SPDXID" "\"SPDXRef-Package-Txiki\"")
string(JSON CAPSID_GPA_PKG_TXIKI SET "${CAPSID_GPA_PKG_TXIKI}"
    "name" "\"txiki.js\"")
string(JSON CAPSID_GPA_PKG_TXIKI SET "${CAPSID_GPA_PKG_TXIKI}"
    "versionInfo" "\"v26.6.0\"")
string(JSON CAPSID_GPA_PKG_TXIKI SET "${CAPSID_GPA_PKG_TXIKI}"
    "licenseConcluded" "\"MIT\"")
string(JSON CAPSID_GPA_PKG_TXIKI SET "${CAPSID_GPA_PKG_TXIKI}"
    "downloadLocation" "\"NOASSERTION\"")
string(JSON CAPSID_GPA_PKG_TXIKI SET "${CAPSID_GPA_PKG_TXIKI}"
    "copyrightText" "\"NOASSERTION\"")
string(JSON CAPSID_GPA_PKG_TXIKI SET "${CAPSID_GPA_PKG_TXIKI}"
    "comment"
    "\"overlay_key=${CAPSID_SBOM_OVERLAY_KEY} overlay_manifest=${CAPSID_SBOM_OVERLAY_MANIFEST}\"")

string(JSON CAPSID_GPA_PKG_QUICKJS SET "{}"
    "SPDXID" "\"SPDXRef-Package-QuickJS\"")
string(JSON CAPSID_GPA_PKG_QUICKJS SET "${CAPSID_GPA_PKG_QUICKJS}"
    "name" "\"quickjs-ng\"")
string(JSON CAPSID_GPA_PKG_QUICKJS SET "${CAPSID_GPA_PKG_QUICKJS}"
    "versionInfo" "\"${CAPSID_SBOM_QUICKJS}\"")
string(JSON CAPSID_GPA_PKG_QUICKJS SET "${CAPSID_GPA_PKG_QUICKJS}"
    "licenseConcluded" "\"MIT\"")
string(JSON CAPSID_GPA_PKG_QUICKJS SET "${CAPSID_GPA_PKG_QUICKJS}"
    "downloadLocation" "\"NOASSERTION\"")
string(JSON CAPSID_GPA_PKG_QUICKJS SET "${CAPSID_GPA_PKG_QUICKJS}"
    "copyrightText" "\"NOASSERTION\"")
string(JSON CAPSID_GPA_PKG_QUICKJS SET "${CAPSID_GPA_PKG_QUICKJS}"
    "comment" "\"locked quickjs commit inside the txiki.js overlay\"")

string(JSON CAPSID_GPA_PKG_LWS SET "{}"
    "SPDXID" "\"SPDXRef-Package-LibWebSockets\"")
string(JSON CAPSID_GPA_PKG_LWS SET "${CAPSID_GPA_PKG_LWS}"
    "name" "\"libwebsockets\"")
string(JSON CAPSID_GPA_PKG_LWS SET "${CAPSID_GPA_PKG_LWS}"
    "versionInfo" "\"4.5.99\"")
string(JSON CAPSID_GPA_PKG_LWS SET "${CAPSID_GPA_PKG_LWS}"
    "licenseConcluded" "\"MIT\"")
string(JSON CAPSID_GPA_PKG_LWS SET "${CAPSID_GPA_PKG_LWS}"
    "downloadLocation" "\"NOASSERTION\"")
string(JSON CAPSID_GPA_PKG_LWS SET "${CAPSID_GPA_PKG_LWS}"
    "copyrightText" "\"NOASSERTION\"")
string(JSON CAPSID_GPA_PKG_LWS SET "${CAPSID_GPA_PKG_LWS}"
    "comment" "\"vendored under the txiki.js overlay (0011-lws-cpack-top-level)\"")

# string(CONCAT) only: a multi-argument set() would form a semicolon list
# and corrupt the JSON array at install time.
string(CONCAT CAPSID_GPA_PACKAGES
    "${CAPSID_GPA_PKG_CAPSID},${CAPSID_GPA_PKG_TXIKI},"
    "${CAPSID_GPA_PKG_QUICKJS},${CAPSID_GPA_PKG_LWS}")
string(JSON CAPSID_GPA_SBOM_JSON SET "${CAPSID_GPA_SBOM_JSON}"
    "packages" "[${CAPSID_GPA_PACKAGES}]")

# Files with hashes.
set(CAPSID_GPA_FILE_OBJS)
set(CAPSID_GPA_RELATIONSHIPS)
list(LENGTH CAPSID_GPA_ENTRIES CAPSID_GPA_ENTRY_COUNT)
set(CAPSID_GPA_INDEX 0)
foreach(CAPSID_GPA_ENTRY IN LISTS CAPSID_GPA_ENTRIES)
    string(REPLACE "|" ";" CAPSID_GPA_PARTS "${CAPSID_GPA_ENTRY}")
    list(GET CAPSID_GPA_PARTS 0 CAPSID_GPA_REL)
    list(GET CAPSID_GPA_PARTS 1 CAPSID_GPA_SIZE)
    list(GET CAPSID_GPA_PARTS 2 CAPSID_GPA_SHA)
    set(CAPSID_GPA_FILE_ID "SPDXRef-File-${CAPSID_GPA_INDEX}")
    string(JSON CAPSID_GPA_FILE_OBJ SET "{}"
        "SPDXID" "\"${CAPSID_GPA_FILE_ID}\"")
    string(JSON CAPSID_GPA_FILE_OBJ SET "${CAPSID_GPA_FILE_OBJ}"
        "fileName" "\"/${CAPSID_GPA_REL}\"")
    string(JSON CAPSID_GPA_FILE_OBJ SET "${CAPSID_GPA_FILE_OBJ}"
        "licenseConcluded" "\"MIT\"")
    string(JSON CAPSID_GPA_FILE_OBJ SET "${CAPSID_GPA_FILE_OBJ}"
        "copyrightText" "\"NOASSERTION\"")
    string(JSON CAPSID_GPA_CHECKSUM SET "{}"
        "algorithm" "\"SHA256\"")
    string(JSON CAPSID_GPA_CHECKSUM SET "${CAPSID_GPA_CHECKSUM}"
        "checksumValue" "\"${CAPSID_GPA_SHA}\"")
    string(JSON CAPSID_GPA_FILE_OBJ SET "${CAPSID_GPA_FILE_OBJ}"
        "checksums" "[${CAPSID_GPA_CHECKSUM}]")
    if(CAPSID_GPA_FILE_OBJS)
        set(CAPSID_GPA_FILE_OBJS
            "${CAPSID_GPA_FILE_OBJS},${CAPSID_GPA_FILE_OBJ}")
    else()
        set(CAPSID_GPA_FILE_OBJS "${CAPSID_GPA_FILE_OBJ}")
    endif()
    if(CAPSID_GPA_RELATIONSHIPS)
        string(APPEND CAPSID_GPA_RELATIONSHIPS ",")
    endif()
    string(APPEND CAPSID_GPA_RELATIONSHIPS
        "{\"spdxElementId\":\"SPDXRef-Package-Capsid\","
        "\"relatedSpdxElement\":\"${CAPSID_GPA_FILE_ID}\","
        "\"relationshipType\":\"CONTAINS\"}")
    math(EXPR CAPSID_GPA_INDEX "${CAPSID_GPA_INDEX} + 1")
endforeach()
string(JSON CAPSID_GPA_SBOM_JSON SET "${CAPSID_GPA_SBOM_JSON}"
    "files" "[${CAPSID_GPA_FILE_OBJS}]")

# Relationships: document → capsid package; capsid → dependency packages.
# string(CONCAT) again — the fragments must not become a semicolon list.
string(CONCAT CAPSID_GPA_DOC_RELS
    "{\"spdxElementId\":\"SPDXRef-DOCUMENT\","
    "\"relatedSpdxElement\":\"SPDXRef-Package-Capsid\","
    "\"relationshipType\":\"DESCRIBES\"},"
    "{\"spdxElementId\":\"SPDXRef-Package-Capsid\","
    "\"relatedSpdxElement\":\"SPDXRef-Package-Txiki\","
    "\"relationshipType\":\"DEPENDS_ON\"},"
    "{\"spdxElementId\":\"SPDXRef-Package-Capsid\","
    "\"relatedSpdxElement\":\"SPDXRef-Package-QuickJS\","
    "\"relationshipType\":\"DEPENDS_ON\"},"
    "{\"spdxElementId\":\"SPDXRef-Package-Capsid\","
    "\"relatedSpdxElement\":\"SPDXRef-Package-LibWebSockets\","
    "\"relationshipType\":\"DEPENDS_ON\"}")
set(CAPSID_GPA_ALL_RELS "${CAPSID_GPA_DOC_RELS}")
if(CAPSID_GPA_RELATIONSHIPS)
    string(APPEND CAPSID_GPA_ALL_RELS ",${CAPSID_GPA_RELATIONSHIPS}")
endif()
string(JSON CAPSID_GPA_SBOM_JSON SET "${CAPSID_GPA_SBOM_JSON}"
    "relationships" "[${CAPSID_GPA_ALL_RELS}]")

file(WRITE "${CAPSID_GPA_SBOM}" "${CAPSID_GPA_SBOM_JSON}\n")
