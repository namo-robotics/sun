# Exercise compiler release names with a fake Git executable, without changing Git state.
file(MAKE_DIRECTORY "${SOURCE_DIR}/tmp/version-name-test")
set(work "${SOURCE_DIR}/tmp/version-name-test")
set(mock "${work}/git")
file(WRITE "${mock}" [=[#!/bin/sh
case "$*" in
  *--version*) echo "git version 2.40.0" ;;
  *rev-parse*) echo abcdef012345 ;;
  *status*) printf '%s' "$MOCK_DIRTY" ;;
  *tag*) printf '%s' "$MOCK_TAGS" ;;
esac
]=])
file(CHMOD "${mock}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

# Generate one header and check its literal version-name definition.
function(check_name expected)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${SOURCE_DIR}" "-DOUTPUT_FILE=${work}/version.h"
        "-DGIT_EXECUTABLE=${mock}" ${ARGN}
        -P "${SOURCE_DIR}/cmake/GitVersion.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Version generation failed: ${output}${error}")
    endif()
    file(READ "${work}/version.h" header)
    string(FIND "${header}" "#define SUN_VERSION_NAME \"${expected}\"" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Expected '${expected}' in ${header}")
    endif()
endfunction()

set(ENV{MOCK_DIRTY} "")
set(ENV{MOCK_TAGS} "")
check_name("dev")
set(ENV{MOCK_TAGS} "0")
check_name("0")
set(ENV{MOCK_TAGS} "v1.0.0")
check_name("v1.0.0")
set(ENV{MOCK_TAGS} "unrelated\nv2.0.0\nv1.0.0\n1.2.3")
check_name("1.2.3")
set(ENV{MOCK_DIRTY} " M source.cpp")
check_name("dev")
check_name("custom label" "-DSUN_VERSION_NAME=custom label")
check_name("" "-DSUN_VERSION_NAME=")
check_name("dev" "-DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE")
check_name("quote\\\"slash\\\\name" "-DSUN_VERSION_NAME=quote\"slash\\name")

# Distribution scripts use the same tag and dirty-source rules.
set(ENV{PATH} "${work}:$ENV{PATH}")
unset(ENV{SUN_VERSION_NAME})
function(check_bundle_name expected)
    execute_process(COMMAND bash "${SOURCE_DIR}/scripts/release-version-name.sh"
        OUTPUT_VARIABLE name OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result)
    if(NOT result EQUAL 0 OR NOT name STREQUAL expected)
        message(FATAL_ERROR "Expected bundle name '${expected}', got '${name}'")
    endif()
endfunction()
set(ENV{MOCK_DIRTY} "")
set(ENV{MOCK_TAGS} "unrelated\nv2.0.0\nv1.0.0\n1.2.3")
check_bundle_name("1.2.3")
set(ENV{MOCK_DIRTY} " M source.cpp")
check_bundle_name("dev")
set(ENV{SUN_VERSION_NAME} "custom label")
check_bundle_name("custom label")
set(ENV{SUN_VERSION_NAME} "")
check_bundle_name("")
file(REMOVE_RECURSE "${work}")

