cmake_minimum_required(VERSION 3.21)

# Hash a deliberately broad source/configuration/tool closure for experimental
# semantics. Runtime libraries on the consuming host are diagnostic provenance;
# they must not change the identity of an already built compiler.
file(GLOB_RECURSE inputs FOLLOW_SYMLINKS LIST_DIRECTORIES false
    "${SOURCE_DIR}/src/*" "${SOURCE_DIR}/include/*"
    "${SOURCE_DIR}/stdlib/*" "${SOURCE_DIR}/third_party/*"
    "${SOURCE_DIR}/cmake/*" "${BINARY_DIR}/_deps/*-src/*")
foreach(directory IN LISTS EXTERNAL_INCLUDES)
    file(GLOB_RECURSE headers FOLLOW_SYMLINKS LIST_DIRECTORIES false "${directory}/*")
    list(APPEND inputs ${headers})
endforeach()
file(GLOB generated "${BINARY_DIR}/*.pb.h" "${BINARY_DIR}/*.pb.cc")
list(APPEND inputs "${BINARY_DIR}/generated/sun_abi_support.h" ${generated} "${SOURCE_DIR}/CMakeLists.txt"
     "${BINARY_DIR}/CMakeCache.txt" "${COMPILER}" ${TOOLS})
# Inspect dependency tables without executing the compiler or linker.
set(loader_overrides LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT)
set(system_dependencies)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    find_program(dependency_inspector NAMES objdump REQUIRED)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "linux+elf")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    find_program(dependency_inspector NAMES otool REQUIRED)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "macos+macho")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "otool")
    list(APPEND loader_overrides DYLD_LIBRARY_PATH DYLD_FRAMEWORK_PATH
        DYLD_FALLBACK_LIBRARY_PATH DYLD_FALLBACK_FRAMEWORK_PATH
        DYLD_INSERT_LIBRARIES DYLD_ROOT_PATH DYLD_SHARED_CACHE_DIR)
    # System dylibs live in Apple's shared cache, not individual disk files.
    # Use the OS build identity for that runtime; hash other dylibs recursively.
    list(APPEND inputs "/System/Library/CoreServices/SystemVersion.plist")
    set(system_dependencies "^/usr/lib/" "^/System/Library/")
else()
    message(FATAL_ERROR "Unsupported implementation fingerprint host")
endif()
foreach(variable IN LISTS loader_overrides)
    if(DEFINED ENV{${variable}} AND NOT "$ENV{${variable}}" STREQUAL "")
        message(FATAL_ERROR "Cannot establish tool runtime coverage with ${variable} set")
    endif()
endforeach()
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${dependency_inspector}")
set(tool_roots "${COMPILER}" ${TOOLS} "${dependency_inspector}")
list(REMOVE_DUPLICATES tool_roots)
foreach(tool IN LISTS tool_roots)
    if(NOT EXISTS "${tool}" OR IS_DIRECTORY "${tool}")
        message(FATAL_ERROR "Cannot fingerprint implementation tool: ${tool}")
    endif()
endforeach()
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${tool_roots}
    PRE_EXCLUDE_REGEXES ${system_dependencies}
    RESOLVED_DEPENDENCIES_VAR tool_libraries
    UNRESOLVED_DEPENDENCIES_VAR unresolved_libraries
    CONFLICTING_DEPENDENCIES_PREFIX conflicting_libraries)
if(unresolved_libraries OR conflicting_libraries_FILENAMES)
    message(FATAL_ERROR "Incomplete tool runtime dependencies: ${unresolved_libraries}; conflicting: ${conflicting_libraries_FILENAMES}")
endif()
list(APPEND inputs "${dependency_inspector}" ${tool_libraries})
list(REMOVE_DUPLICATES inputs)
list(SORT inputs)
set(digests "sun.experimental.implementation.v1")
foreach(input IN LISTS inputs)
    if(IS_DIRECTORY "${input}")
        continue()
    elseif(IS_SYMLINK "${input}" AND NOT EXISTS "${input}")
        message(FATAL_ERROR "Cannot fingerprint broken implementation symlink: ${input}")
    elseif(input STREQUAL "${BINARY_DIR}/CMakeCache.txt")
        # Cache comments and workspace locations are not implementation options.
        # Preserve every other value, including option and search-path order.
        file(STRINGS "${input}" cache_lines)
        set(configuration "")
        foreach(line IN LISTS cache_lines)
            if(line MATCHES "^(#|//|SUN_RELEASE_(ORDER|SERIES):|SUN_VERSION_NAME:)" OR line STREQUAL "")
                continue()
            endif()
            string(REPLACE "${BINARY_DIR}" "@build@" line "${line}")
            string(REPLACE "${SOURCE_DIR}" "@source@" line "${line}")
            string(APPEND configuration "${line}\n")
        endforeach()
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
