target_sources(sun_tests PRIVATE
    test_errors.cpp
    test_results.cpp
    test_exception_ownership.cpp
    test_arithmetic_errors.cpp
)

if(TARGET sun_exception_runtime)
    target_sources(sun_tests PRIVATE
        test_source_exceptions.cpp test_exception_traces.cpp
        exception_cli_fixture.cpp test_trace_reporting.cpp test_trace_depth.cpp
        test_exception_compilation.cpp test_exception_writer.cpp
        test_native_exceptions.cpp exception_jit_fixture.cpp)
    target_compile_definitions(sun_tests PRIVATE
        SUN_TEST_COMPILER="$<TARGET_FILE:sun>" SUN_TEST_ROOT="${CMAKE_SOURCE_DIR}")
    add_dependencies(sun_tests sun)
    target_link_libraries(sun_tests PRIVATE Threads::Threads)
    find_program(SUN_EXCEPTION_CLANG NAMES clang-20 clang HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
    # Ask the backend for its IR triple; Darwin's driver triple can differ.
    execute_process(COMMAND ${SUN_EXCEPTION_CLANG} -Werror -S -emit-llvm -x c /dev/null -o -
        OUTPUT_VARIABLE SUN_EXCEPTION_EMPTY_IR
        COMMAND_ERROR_IS_FATAL ANY)
    string(REGEX MATCH "target triple = \"([^\"]+)\"" SUN_EXCEPTION_TRIPLE_LINE
        "${SUN_EXCEPTION_EMPTY_IR}")
    if(NOT SUN_EXCEPTION_TRIPLE_LINE)
        message(FATAL_ERROR "Cannot determine the exception fixture target triple")
    endif()
    set(SUN_EXCEPTION_TARGET "${CMAKE_MATCH_1}")
    set(SUN_EXCEPTION_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/exceptions.ll)
    configure_file(fixtures/exceptions.ll.in ${SUN_EXCEPTION_FIXTURE} @ONLY)
    set(SUN_EXCEPTION_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/exceptions_fixture.o)
    add_custom_command(OUTPUT ${SUN_EXCEPTION_OBJECT}
        COMMAND ${SUN_EXCEPTION_CLANG} -Werror -x ir -c -fPIC -O2
            ${SUN_EXCEPTION_FIXTURE} -o ${SUN_EXCEPTION_OBJECT}
        DEPENDS ${SUN_EXCEPTION_FIXTURE}
        COMMENT "Compiling native Sun exception runtime fixtures" VERBATIM)
    add_custom_target(sun_exception_fixture DEPENDS ${SUN_EXCEPTION_OBJECT})
    target_sources(sun_tests PRIVATE ${SUN_EXCEPTION_OBJECT})
    add_dependencies(sun_tests sun_exception_fixture)
    target_compile_definitions(sun_tests PRIVATE SUN_EXCEPTION_FIXTURE="${SUN_EXCEPTION_FIXTURE}")
    if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        include(CheckSymbolExists)
        check_symbol_exists(__GLIBC__ "features.h" SUN_EXCEPTION_GLIBC)
        if(APPLE OR SUN_EXCEPTION_GLIBC)
            set(SUN_EXCEPTION_ALLOCATION_SUPPORT
                exception_allocation_support.cpp exception_capture_probe.cpp allocation_audit.cpp)
            add_executable(sun_exception_allocation_probe
                exception_allocation_probe.cpp ${SUN_EXCEPTION_ALLOCATION_SUPPORT}
                ../../src/driver/jit/exception_storage.cpp ${SUN_EXCEPTION_OBJECT})
            add_dependencies(sun_exception_allocation_probe sun_exception_fixture)
            target_link_libraries(sun_exception_allocation_probe PRIVATE sun_exception_runtime Threads::Threads ${CMAKE_DL_LIBS})
            target_compile_options(sun_exception_allocation_probe PRIVATE -fno-builtin)
            add_executable(sun_exception_jit_allocation_probe
                exception_jit_allocation_probe.cpp exception_jit_fixture.cpp
                ${SUN_EXCEPTION_ALLOCATION_SUPPORT} ${SUN_EXCEPTION_OBJECT})
            add_dependencies(sun_exception_jit_allocation_probe sun_exception_fixture)
            target_link_libraries(sun_exception_jit_allocation_probe PRIVATE sun_exception_runtime sun_jit_support Threads::Threads ${CMAKE_DL_LIBS})
            target_compile_options(sun_exception_jit_allocation_probe PRIVATE -fno-builtin)
            target_compile_definitions(sun_exception_jit_allocation_probe PRIVATE SUN_EXCEPTION_FIXTURE="${SUN_EXCEPTION_FIXTURE}")
            target_sources(sun_tests PRIVATE test_exception_platform.cpp)
            add_dependencies(sun_tests
                sun_exception_allocation_probe sun_exception_jit_allocation_probe)
            target_compile_definitions(sun_tests PRIVATE
                SUN_TEST_CLANG="${SUN_EXCEPTION_CLANG}"
                SUN_TEST_NATIVE_PROBE="$<TARGET_FILE:sun_exception_allocation_probe>"
                SUN_TEST_JIT_PROBE="$<TARGET_FILE:sun_exception_jit_allocation_probe>")
        endif()
    endif()
endif()
