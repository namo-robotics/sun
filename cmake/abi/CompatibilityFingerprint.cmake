# Hash a deliberately broad source/configuration/tool closure for experimental
# semantics. Shared libraries are additionally hashed by the running consumer.
file(GLOB_RECURSE inputs LIST_DIRECTORIES false
    "${SOURCE_DIR}/src/*" "${SOURCE_DIR}/include/*"
    "${SOURCE_DIR}/stdlib/*" "${SOURCE_DIR}/third_party/*"
    "${SOURCE_DIR}/cmake/*" "${BINARY_DIR}/_deps/*-src/*")
foreach(directory IN LISTS EXTERNAL_INCLUDES)
    file(GLOB_RECURSE headers LIST_DIRECTORIES false "${directory}/*")
    list(APPEND inputs ${headers})
endforeach()
file(GLOB generated "${BINARY_DIR}/*.pb.h" "${BINARY_DIR}/*.pb.cc")
list(APPEND inputs ${generated} "${SOURCE_DIR}/CMakeLists.txt"
     "${BINARY_DIR}/CMakeCache.txt" "${COMPILER}")
list(REMOVE_DUPLICATES inputs)
list(SORT inputs)
set(digests "sun.experimental.implementation.v1")
foreach(input IN LISTS inputs)
    if(IS_DIRECTORY "${input}")
        continue()
    elseif(IS_SYMLINK "${input}" AND NOT EXISTS "${input}")
        file(READ_SYMLINK "${input}" target)
        string(APPEND digests "missing:${target}")
    elseif(input STREQUAL "${BINARY_DIR}/CMakeCache.txt")
        # Only diagnostic release values are omitted; all implementation options remain.
        file(READ "${input}" configuration)
        string(REGEX REPLACE "SUN_RELEASE_(ORDER|SERIES):STRING=[^\n]*" "" configuration "${configuration}")
        string(SHA256 digest "${configuration}")
        string(APPEND digests "${digest}")
    elseif(EXISTS "${input}")
        file(SHA256 "${input}" digest)
        string(APPEND digests "${digest}")
    else()
        message(FATAL_ERROR "Cannot fingerprint implementation input: ${input}")
    endif()
endforeach()
string(SHA256 fingerprint "${digests}")
set(output "${BINARY_DIR}/generated/sun_compatibility.h")
set(content "// Generated experimental implementation identity.
#pragma once
#define SUN_COMPATIBILITY_IMPLEMENTATION \"${fingerprint}\"
")
set(previous "")
if(EXISTS "${output}")
    file(READ "${output}" previous)
endif()
if(NOT previous STREQUAL content)
    file(WRITE "${output}" "${content}")
endif()
