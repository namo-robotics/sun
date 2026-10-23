# Verify that a downloaded bundle survives host runtime changes without rebuilding.
set(fixture "${SOURCE_DIR}/tmp/abi_runtime_provenance")
file(REMOVE_RECURSE "${fixture}")
file(MAKE_DIRECTORY "${fixture}/runtime")
file(WRITE "${fixture}/sun-config.json" "{\"root\": true}\n")
file(WRITE "${fixture}/consumer.sun" "manifest { libraries: [\"${STDLIB}\"] }\n/** Exercises an imported bundle contract. */\npublic module runtime_consumer {\n/** Returns a fixed result. */\npublic function value() i32 { return 42; }\n}\n")
set(compile "${SUN}" --emit-moon --force-rebuild -o "${fixture}/consumer.moon"
    "${fixture}/consumer.sun")

# Compile against the original stdlib with a controlled loader environment.
function(check_runtime expected_warning)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_PRELOAD
        --unset=LD_LIBRARY_PATH ${ARGN} ${compile}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Runtime changed bundle compatibility: ${output}${error}")
    endif()
    string(FIND "${error}" "different runtime libraries" warning)
    if(expected_warning AND warning EQUAL -1)
        message(FATAL_ERROR "Runtime drift did not produce a warning: ${error}")
    elseif(NOT expected_warning AND NOT warning EQUAL -1)
        message(FATAL_ERROR "Unrelated runtime produced a warning: ${error}")
    endif()
endfunction()

check_runtime(FALSE)
check_runtime(FALSE "LD_PRELOAD=${UNRELATED_LIBRARY}")
# Appending bytes changes provenance without changing the library's ELF ABI.
# Supply the SONAME used by the compiler, independently of its versioned filename.
find_program(objdump NAMES objdump REQUIRED)
execute_process(COMMAND "${objdump}" -p "${RUNTIME_LIBRARY}"
    OUTPUT_VARIABLE runtime_headers COMMAND_ERROR_IS_FATAL ANY)
string(REGEX MATCH "SONAME[ \t]+([^ \t\r\n]+)" soname_match "${runtime_headers}")
set(runtime_soname "${CMAKE_MATCH_1}")
if(NOT runtime_soname)
    message(FATAL_ERROR "The runtime fixture needs a shared library SONAME")
endif()
file(COPY_FILE "${RUNTIME_LIBRARY}" "${fixture}/runtime/${runtime_soname}")
file(APPEND "${fixture}/runtime/${runtime_soname}" "runtime provenance fixture")
check_runtime(TRUE "LD_LIBRARY_PATH=${fixture}/runtime")
file(REMOVE_RECURSE "${fixture}")
