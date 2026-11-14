target_sources(sun_tests PRIVATE
    test_errors.cpp
    test_results.cpp
    test_exception_ownership.cpp
    test_arithmetic_errors.cpp
)

if(TARGET sun_exception_runtime)
    add_executable(sun_source_exception_tests test_source_exceptions.cpp)
    target_link_libraries(sun_source_exception_tests PRIVATE sun_lib GTest::gtest_main)
    add_test(NAME Errors_SourceExceptions.language
        COMMAND sun_source_exception_tests
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME Errors_SourceExceptions.native_and_formatting
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_source_exceptions.py
            $<TARGET_FILE:sun> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/source_exceptions.sun
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    find_program(SUN_EXCEPTION_CLANG NAMES clang-20 clang HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
    # Ask the backend for its IR triple; Darwin's driver triple can differ.
    execute_process(COMMAND ${SUN_EXCEPTION_CLANG} -S -emit-llvm -x c /dev/null -o -
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
        COMMAND ${SUN_EXCEPTION_CLANG} -x ir -c -fPIC -O2
            ${SUN_EXCEPTION_FIXTURE} -o ${SUN_EXCEPTION_OBJECT}
        DEPENDS ${SUN_EXCEPTION_FIXTURE}
        COMMENT "Compiling native Sun exception runtime fixtures" VERBATIM)
    add_custom_target(sun_exception_fixture DEPENDS ${SUN_EXCEPTION_OBJECT})
    add_executable(sun_exception_tests test_native_exceptions.cpp ${SUN_EXCEPTION_OBJECT})
    add_dependencies(sun_exception_tests sun_exception_fixture)
    target_link_libraries(sun_exception_tests PRIVATE sun_exception_runtime sun_jit_support GTest::gtest_main Threads::Threads)
    target_compile_definitions(sun_exception_tests PRIVATE SUN_EXCEPTION_FIXTURE="${SUN_EXCEPTION_FIXTURE}")
    add_test(NAME Errors_NativeExceptions.transport
        COMMAND sun_exception_tests
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        include(CheckSymbolExists)
        check_symbol_exists(__GLIBC__ "features.h" SUN_EXCEPTION_GLIBC)
        if(APPLE OR SUN_EXCEPTION_GLIBC)
            add_executable(sun_exception_allocation_probe
                exception_allocation_probe.cpp ${SUN_EXCEPTION_OBJECT})
            add_dependencies(sun_exception_allocation_probe sun_exception_fixture)
            target_link_libraries(sun_exception_allocation_probe PRIVATE sun_exception_runtime sun_jit_support Threads::Threads ${CMAKE_DL_LIBS})
            target_compile_options(sun_exception_allocation_probe PRIVATE -fno-builtin)
            target_compile_definitions(sun_exception_allocation_probe PRIVATE SUN_EXCEPTION_FIXTURE="${SUN_EXCEPTION_FIXTURE}")
            add_test(NAME Errors_NativeExceptions.allocation_free_first_throw
                COMMAND sun_exception_allocation_probe
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
            add_test(NAME Errors_NativeExceptions.allocation_free_first_jit_throw
                COMMAND sun_exception_allocation_probe --jit
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
            add_test(NAME Errors_NativeExceptions.allocation_free_jit_reload
                COMMAND sun_exception_allocation_probe --jit-reload
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
            add_test(NAME Errors_NativeExceptions.allocation_free_jit_threads
                COMMAND sun_exception_allocation_probe --jit-threads
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        endif()
    endif()
endif()
