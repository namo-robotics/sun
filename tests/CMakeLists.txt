# Prefer a system-installed or package-manager-provided GoogleTest
find_package(GTest REQUIRED)

# Optional: for Windows, if using a manually built GTest
# set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# (Only needed if mixing CRT types; usually not required with package managers)

# Create test executable
add_executable(sun_tests
    test_allocator.cpp
    test_arrays.cpp
    test_binops.cpp
    test_borrow_checker.cpp
    test_classes.cpp
    test_compilation.cpp
    test_enums.cpp
    test_errors.cpp
    test_file_io.cpp
    test_generic_classes.cpp
    test_generic_functions.cpp
    test_generic_methods.cpp
    test_generic_specializations.cpp
    test_interfaces.cpp
    test_intrinsics.cpp
    test_linked_list.cpp
    test_loops.cpp
    test_map.cpp
    test_match.cpp
    test_matrix.cpp
    test_memory_safety.cpp
    test_modules.cpp
    test_parser.cpp
    test_pointers.cpp
    test_programs.cpp
    test_refs.cpp
    test_regex_parser.cpp
    test_string.cpp
    test_vec.cpp
    # Add more test files
)

target_link_libraries(sun_tests
    PRIVATE
        sun_lib          # Your code
        GTest::gtest_main  # Provides main() and nice output
)

# Ensure stdlib.moon is built before tests run
add_dependencies(sun_tests stdlib_moon)

# Discover tests automatically (nice per-test reporting in ctest)
include(GoogleTest)
gtest_discover_tests(sun_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    PROPERTIES ENVIRONMENT "SUN_PATH=${CMAKE_SOURCE_DIR}"
)

# No need to copy programs - tests run from workspace root using source files