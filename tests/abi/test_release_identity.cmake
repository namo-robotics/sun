# Exercise release configuration and its exclusion from experimental semantics.
set(fixture "${SOURCE_DIR}/tmp/abi_release_identity")
file(MAKE_DIRECTORY "${fixture}/source" "${fixture}/build/generated")

# Run the production provenance validator in a separate process for each case.
function(check_release order series accepted)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSUN_RELEASE_ORDER=${order}"
        "-DSUN_RELEASE_SERIES=${series}" -P "${SOURCE_DIR}/cmake/abi/ReleaseIdentity.cmake"
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    if((accepted AND NOT result EQUAL 0) OR (NOT accepted AND result EQUAL 0))
        message(FATAL_ERROR "Unexpected validation result for release '${series}:${order}'")
    endif()
endfunction()
check_release("0" "" TRUE)
check_release("1" "sun.official-1" TRUE)
check_release("18446744073709551615" "official" TRUE)
check_release("18446744073709551616" "official" FALSE)
check_release("01" "official" FALSE)
check_release("-1" "official" FALSE)
check_release("1" "" FALSE)
check_release("0" "official" FALSE)
check_release("1" "unsafe\"series" FALSE)

file(WRITE "${fixture}/source/CMakeLists.txt" "# Fixture implementation\n")
file(WRITE "${fixture}/compiler" "compiler fixture")
file(WRITE "${fixture}/build/generated/sun_abi_support.h" "embedded support fixture")
# Run the real fingerprint script with a small complete fixture input closure.
function(fingerprint order series implementation output)
    file(WRITE "${fixture}/build/CMakeCache.txt"
        "SUN_RELEASE_ORDER:STRING=${order}\nSUN_RELEASE_SERIES:STRING=${series}\nIMPLEMENTATION:STRING=${implementation}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${fixture}/source"
        "-DBINARY_DIR=${fixture}/build" "-DCOMPILER=${fixture}/compiler"
        -P "${SOURCE_DIR}/cmake/abi/CompatibilityFingerprint.cmake"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Fingerprint generation failed")
    endif()
    file(READ "${fixture}/build/generated/sun_compatibility.h" generated)
    set(${output} "${generated}" PARENT_SCOPE)
endfunction()
fingerprint("0" "" "one" development)
fingerprint("17" "official" "one" release)
fingerprint("18" "next-series" "one" next_release)
fingerprint("18" "next-series" "two" changed)
if(NOT development STREQUAL release OR NOT release STREQUAL next_release)
    message(FATAL_ERROR "Diagnostic release metadata changed compatibility")
endif()
if(next_release STREQUAL changed)
    message(FATAL_ERROR "Implementation configuration was omitted from compatibility")
endif()
file(WRITE "${fixture}/build/generated/sun_abi_support.h" "changed embedded support")
fingerprint("18" "next-series" "two" changed_support)
if(changed STREQUAL changed_support)
    message(FATAL_ERROR "Embedded compiler support was omitted from compatibility")
endif()
file(REMOVE_RECURSE "${fixture}")
