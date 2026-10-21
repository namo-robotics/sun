target_sources(sun_tests PRIVATE test_compatibility.cpp test_boundary_requirements.cpp test_semantic_requirements.cpp test_native_requirements.cpp test_nominal_identity.cpp)

# Verify release configuration independently of the current compiler identity.
add_test(NAME ABI_ReleaseIdentity.Configuration
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/test_release_identity.cmake
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ABI checks remain buildable while an unsupported product dependency is diagnosed.
add_executable(sun_abi_tests EXCLUDE_FROM_ALL
    test_compatibility.cpp test_boundary_requirements.cpp
    test_semantic_requirements.cpp test_native_requirements.cpp test_nominal_identity.cpp)
target_link_libraries(sun_abi_tests PRIVATE sun_lib GTest::gtest_main)
target_compile_definitions(sun_abi_tests PRIVATE
    SUN_FFI_TESTLIB_DIR="${CMAKE_BINARY_DIR}/tests"
    SUN_FFI_TESTLIB_V2_DIR="${CMAKE_BINARY_DIR}/tests/v2")
add_dependencies(sun_abi_tests sun_ffi_testlib sun_ffi_static_testlib sun_ffi_static_testlib_v2)

# Exercise the exact parser and validator used to embed compiler capabilities.
add_test(NAME ABI_CompilerSupport.BuildManifest
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DGENERATOR=$<TARGET_FILE:sun_embed_abi_support>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/test_support_manifest.cmake
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Exercise real position-independent relocations previously rejected by ABI checks.
add_library(sun_abi_relocations STATIC native_relocations.cpp
    native_relocation_value.cpp)
set_target_properties(sun_abi_relocations PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
target_link_libraries(sun_tests PRIVATE sun_abi_relocations)
target_link_libraries(sun_abi_tests PRIVATE sun_abi_relocations)

# Native execution owns object constructors and thread-local state.
add_library(sun_abi_lifecycle STATIC native_lifecycle.cpp)
set_target_properties(sun_abi_lifecycle PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
target_link_libraries(sun_tests PRIVATE sun_abi_lifecycle)
target_link_libraries(sun_abi_tests PRIVATE sun_abi_lifecycle)

# Cross-target objects need no SDK: this fixture has no headers or library calls.
find_program(SUN_ABI_CLANG NAMES clang HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
find_program(SUN_ABI_AR NAMES llvm-ar HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
set(macho_fixtures)
foreach(architecture IN ITEMS x86_64 aarch64)
    set(archive "${CMAKE_BINARY_DIR}/tests/libsun_abi_${architecture}_macho.a")
    set(object "${CMAKE_CURRENT_BINARY_DIR}/${architecture}_macho.o")
    add_custom_command(OUTPUT "${archive}"
        COMMAND "${SUN_ABI_CLANG}" --target=${architecture}-apple-macos11
            -c "${CMAKE_CURRENT_SOURCE_DIR}/native_cross_target.c" -o "${object}"
        COMMAND "${SUN_ABI_AR}" rcs "${archive}" "${object}"
        DEPENDS native_cross_target.c VERBATIM)
    list(APPEND macho_fixtures "${archive}")
endforeach()
add_custom_target(sun_abi_macho_fixtures DEPENDS ${macho_fixtures})
add_dependencies(sun_tests sun_abi_macho_fixtures)
add_dependencies(sun_abi_tests sun_abi_macho_fixtures)
