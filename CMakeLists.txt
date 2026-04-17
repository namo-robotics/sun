cmake_minimum_required(VERSION 3.20)
project(SunCompiler LANGUAGES C CXX)

# ------------------------------------------------------------------
# General settings
# ------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # Useful for clang-tidy, IDEs, etc.

# Allow Debug/Release selection via command line or IDE (don't hardcode)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------------
# Find LLVM
# ------------------------------------------------------------------
find_package(LLVM 20.1 REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# LLVM is target-based in modern versions → prefer imported targets
# add_definitions(${LLVM_DEFINITIONS}) and include_directories(${LLVM_INCLUDE_DIRS})
# are old-style and should be avoided when possible.

# ------------------------------------------------------------------
# Create the core library (recommended)
# ------------------------------------------------------------------
add_library(sun_lib
    src/parser.cpp
    src/ast.cpp
    src/codegen.cpp
    src/driver.cpp
    src/codegen/codegen_visitor.cpp
    src/codegen/functions_lambdas.cpp
    src/codegen/pointers.cpp
    src/codegen/intrinsics.cpp
    src/codegen/arrays.cpp
    src/codegen/variable_creation.cpp
    src/codegen/variable_references.cpp
    src/codegen/call_expressions.cpp
    src/codegen/if_expressions.cpp
    src/codegen/match_expressions.cpp
    src/codegen/loops.cpp
    src/codegen/return_statements.cpp
    src/codegen/block_expressions.cpp
    src/codegen/classes.cpp
    src/codegen/error_handling.cpp
    src/semantic_analyzer.cpp
    src/semantic_analysis/scope_variables.cpp
    src/semantic_analysis/classes.cpp
    src/semantic_analysis/interfaces.cpp
    src/semantic_analysis/type_conversion.cpp
    src/semantic_analysis/type_inference.cpp
    src/semantic_analysis/captures.cpp
    src/semantic_analysis/analysis.cpp
    src/llvm_type_resolver.cpp
    src/borrow_checker/borrow_state.cpp
    src/borrow_checker/borrow_checker.cpp
    # Precompiled library support
    src/moon.cpp
    src/library_cache.cpp
    src/module_linker.cpp
    src/module_types.cpp
    # Add other implementation files here (but NOT main.cpp)
)

# Public headers go in include/SunCompiler/ or include/
target_include_directories(sun_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>  # For potential installation later
    PRIVATE
        src  # If you have private headers in src/
)

# Link LLVM the modern way (target-based)
target_link_libraries(sun_lib PUBLIC LLVM)  # Add more components as needed

# Ensure the library uses the same definitions/includes as LLVM
target_compile_definitions(sun_lib PUBLIC ${LLVM_DEFINITIONS})
target_include_directories(sun_lib SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

# ------------------------------------------------------------------
# Main executable
# ------------------------------------------------------------------
add_executable(sun src/main.cpp)
target_link_libraries(sun PRIVATE sun_lib)

# ------------------------------------------------------------------
# Precompile stdlib as .moon for faster test imports
# ------------------------------------------------------------------
set(STDLIB_MOON ${CMAKE_BINARY_DIR}/stdlib.moon)
set(STDLIB_SOURCES
    ${CMAKE_SOURCE_DIR}/stdlib/print.sun
    ${CMAKE_SOURCE_DIR}/stdlib/allocator.sun
    ${CMAKE_SOURCE_DIR}/stdlib/unique.sun
    ${CMAKE_SOURCE_DIR}/stdlib/slice.sun
    ${CMAKE_SOURCE_DIR}/stdlib/matrix.sun
    ${CMAKE_SOURCE_DIR}/stdlib/string.sun
    ${CMAKE_SOURCE_DIR}/stdlib/vec.sun
    ${CMAKE_SOURCE_DIR}/stdlib/map.sun
)

add_custom_command(
    OUTPUT ${STDLIB_MOON}
    COMMAND $<TARGET_FILE:sun> --emit-moon --bundle -o ${STDLIB_MOON} ${STDLIB_SOURCES}
    DEPENDS sun ${STDLIB_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building precompiled stdlib.moon"
    VERBATIM
)

add_custom_target(stdlib_moon ALL DEPENDS ${STDLIB_MOON})

# ------------------------------------------------------------------
# Testing (optional, can be disabled)
# ------------------------------------------------------------------
include(CTest)  # Enables add_test and creates BUILD_TESTING option
option(BUILD_TESTING "Build the tests" ON)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)  # We'll create this folder next
endif()