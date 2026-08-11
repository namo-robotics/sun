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

# Create test executable
add_executable(sun_tests
    test_allocator.cpp
    test_arithmetic.cpp
    test_arrays.cpp
    test_bitwise.cpp
    test_compound_assignment.cpp
    test_ref_targets.cpp
    test_ref_captures.cpp
    test_unsigned.cpp
    test_borrow_checker.cpp
    test_bound_methods.cpp
    test_classes.cpp
    test_compilation.cpp
    test_contiguous_buffer.cpp
    test_debug.cpp
    test_enums.cpp
    test_errors.cpp
    test_file_io.cpp
    test_sun_io.cpp
    test_generic_classes.cpp
    test_generic_functions.cpp
    test_generic_methods.cpp
    test_generic_specializations.cpp
    test_http.cpp
    test_interfaces.cpp
    test_interpolation.cpp
    test_intrinsics.cpp
    test_lambda_params.cpp
    test_linked_list.cpp
    test_logical.cpp
    test_loops.cpp
    test_map.cpp
    test_match.cpp
    test_matrix.cpp
    test_memory_safety.cpp
    test_modules.cpp
    test_mutex.cpp
    test_parser.cpp
    test_pointers.cpp
    test_programs.cpp
    test_refs.cpp
    test_regex_parser.cpp
    test_serialization.cpp
    test_stdlib_linked_list.cpp
    test_stdlib_math.cpp
    test_string.cpp
    test_ternary.cpp
    test_threads.cpp
    test_vec.cpp
    # Add more test files
)

target_link_libraries(sun_tests
    PRIVATE
        sun_lib          # Your code
        GTest::gtest_main  # Provides main() and nice output
)

target_precompile_headers(sun_tests PRIVATE
    <gtest/gtest.h>
    ${CMAKE_SOURCE_DIR}/include/ast.h
    ${CMAKE_SOURCE_DIR}/include/parser.h
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(sun_tests PRIVATE -Xclang -fno-pch-timestamp)
endif()

# Ensure stdlib.moon is built before tests run
if(NOT SKIP_STDLIB)
    add_dependencies(sun_tests stdlib_moon)
endif()

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