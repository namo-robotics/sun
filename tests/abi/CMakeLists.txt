target_sources(sun_tests PRIVATE test_compatibility.cpp test_boundary_requirements.cpp test_semantic_requirements.cpp test_native_requirements.cpp)

# Verify release configuration independently of the current compiler identity.
add_test(NAME ABI_ReleaseIdentity.Configuration
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/test_release_identity.cmake
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ABI checks remain buildable while an unsupported product dependency is diagnosed.
add_executable(sun_abi_tests EXCLUDE_FROM_ALL
    test_compatibility.cpp test_boundary_requirements.cpp
    test_semantic_requirements.cpp test_native_requirements.cpp)
target_link_libraries(sun_abi_tests PRIVATE sun_lib GTest::gtest_main)
target_compile_definitions(sun_abi_tests PRIVATE
    SUN_FFI_TESTLIB_DIR="${CMAKE_BINARY_DIR}/tests"
    SUN_FFI_TESTLIB_V2_DIR="${CMAKE_BINARY_DIR}/tests/v2")
add_dependencies(sun_abi_tests sun_ffi_testlib sun_ffi_static_testlib sun_ffi_static_testlib_v2)
