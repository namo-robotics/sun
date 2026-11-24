# Exercise independent compiler processes sharing the configured validation cache.
set(fixture "${SOURCE_DIR}/tmp/validation_cache_processes")
file(REMOVE_RECURSE "${fixture}")
file(MAKE_DIRECTORY "${fixture}")
file(WRITE "${fixture}/sun-config.json" "{\"root\":true}\n")
file(WRITE "${fixture}/library.sun" "manifest {}\n/** Supplies a cache fixture. */\npublic module cache_fixture {\n/** Returns a constant. */\npublic function value() i32 { return 7; }\n}\n")
set(compile "${SUN}" --emit-moon -o "${fixture}/library.moon" "${fixture}/library.sun")
# Cold compilation creates the artifact; the next invocation validates its bytes.
foreach(pass RANGE 1 3)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env
        --unset=SUN_DISABLE_VALIDATION_CACHE
        "SUN_VALIDATION_CACHE=${fixture}/cache" SUN_LOG_LEVEL=debug ${compile}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cache process ${pass} failed: ${output}${errors}")
    endif()
    if(pass EQUAL 2 AND NOT "${output}${errors}" MATCHES "Validation cache miss")
        message(FATAL_ERROR "Cold output validation did not miss: ${output}${errors}")
    endif()
    if(pass EQUAL 3 AND NOT "${output}${errors}" MATCHES "Validation cache hit")
        message(FATAL_ERROR "Fresh process did not reuse receipt: ${output}${errors}")
    endif()
endforeach()
# Explicitly disabled caching still validates and leaves the artifact up to date.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    "SUN_VALIDATION_CACHE=${fixture}/cache" SUN_DISABLE_VALIDATION_CACHE=1
    SUN_LOG_LEVEL=debug ${compile}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0 OR NOT "${output}${errors}" MATCHES "Validation cache miss")
    message(FATAL_ERROR "Disabled cache did not validate: ${output}${errors}")
endif()
file(REMOVE_RECURSE "${fixture}")
