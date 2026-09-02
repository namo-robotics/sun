# Ubuntu's prebuilt libgtest.a carries GCC .sframe sections that lld 20
# cannot link; build GoogleTest from the distro-provided sources with our
# own toolchain instead (defines the same GTest::gtest_main targets).
if(EXISTS /usr/src/googletest/CMakeLists.txt)
    add_subdirectory(/usr/src/googletest ${CMAKE_BINARY_DIR}/_googletest EXCLUDE_FROM_ALL)
else()
    find_package(GTest REQUIRED)
endif()

# Optional: for Windows, if using a manually built GTest
# set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# (Only needed if mixing CRT types; usually not required with package managers)

# One binary for every suite. Sources come from the group subdirectories
# below; nothing is listed here. Suite names mirror the directory path
# (e.g. tests/functions/generic/test_constraints.cpp ->
# Functions_Generic_Constraints), so --gtest_filter="Functions_*" selects a
# whole group. The stdlib's own tests are Sun code (stdlib/*_tests.sun) and
# run as the stdlib_sun_tests entry below, not through this binary.
add_executable(sun_tests)

# Must come after add_executable: target_sources() needs the target to exist.
# Order among the groups themselves does not matter.
add_subdirectory(builtins)
add_subdirectory(classes)
add_subdirectory(control_flow)
add_subdirectory(end_to_end)
add_subdirectory(enums)
add_subdirectory(errors)
add_subdirectory(ffi)
add_subdirectory(functions)
add_subdirectory(interfaces)
add_subdirectory(lambdas)
add_subdirectory(memory_safety)
add_subdirectory(modules)
add_subdirectory(operators)
add_subdirectory(tooling)

target_link_libraries(sun_tests
    PRIVATE
        sun_lib          # Your code
        GTest::gtest_main  # Provides main() and nice output
)

target_precompile_headers(sun_tests PRIVATE
    <gtest/gtest.h>
    ${CMAKE_SOURCE_DIR}/include/ast.h
    ${CMAKE_SOURCE_DIR}/include/parsing/parser.h
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(sun_tests PRIVATE -Xclang -fno-pch-timestamp)
endif()

# Ensure stdlib.moon is built before tests run
if(NOT SKIP_STDLIB)
    add_dependencies(sun_tests stdlib_moon)

    # The stdlib's own Sun test suite (stdlib/*_tests.sun), AOT-compiled to
    # build/stdlib_tests_test by the root CMakeLists. One ctest entry runs the
    # whole suite sequentially; individual failures print as FAIL lines, and
    # `build/stdlib_tests_test --test-filter <name>` reruns one test.
    add_test(NAME stdlib_sun_tests
             COMMAND ${STDLIB_TEST_BIN}_test --test-sequential
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # The tls bundle's Sun test suite (tls/*_tests.sun), built the same way
    # when the static OpenSSL archives are present. Sequential too: the
    # handshake tests point SSL_CERT_FILE, a process-wide setting, at a
    # throwaway certificate.
    if(SUN_HAVE_OPENSSL)
        add_test(NAME tls_sun_tests
                 COMMAND ${TLS_TEST_BIN}_test --test-sequential
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
endif()

# Shared library for the C FFI tests. Its symbols are deliberately absent
# from the test binary, so -l/-L are actually exercised rather than being
# satisfied by libc already being resident in the process.
add_library(sun_ffi_testlib SHARED fixtures/ffi_testlib.c)
set_target_properties(sun_ffi_testlib PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
add_dependencies(sun_tests sun_ffi_testlib)

# Static archive counterpart, for the tests that JIT-load a .a named with -l
# (issue #133). Position-independent, as any archive meant for the JIT is.
add_library(sun_ffi_static_testlib STATIC fixtures/ffi_static_testlib.c)
set_target_properties(sun_ffi_static_testlib PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
    POSITION_INDEPENDENT_CODE ON
)
add_dependencies(sun_tests sun_ffi_static_testlib)
# Passed as a define rather than a test ENVIRONMENT entry: gtest_discover_tests
# splits a multi-entry ENVIRONMENT list into separate property arguments.
target_compile_definitions(sun_tests
    PRIVATE SUN_FFI_TESTLIB_DIR="${CMAKE_BINARY_DIR}/tests")

# Discover tests automatically (nice per-test reporting in ctest)
include(GoogleTest)
# PRE_TEST: enumerate tests at ctest time instead of re-running the binary
# after every relink during builds
gtest_discover_tests(sun_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    PROPERTIES ENVIRONMENT "SUN_PATH=${CMAKE_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
)

# No need to copy programs - tests run from workspace root using source files