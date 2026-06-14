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

# ------------------------------------------------------------------
# Find Protobuf (for AST serialization)
# ------------------------------------------------------------------
find_package(Protobuf REQUIRED)
message(STATUS "Found Protobuf ${Protobuf_VERSION}")

# ------------------------------------------------------------------
# Generate Protobuf sources (AST serialization + Moon library)
# ------------------------------------------------------------------
set(AST_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/ast)
set(MOON_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/moon)

# AST proto files
set(AST_PROTO_FILES
    ${AST_PROTO_DIR}/types.proto
    ${AST_PROTO_DIR}/ast.proto
    ${AST_PROTO_DIR}/analysis.proto
)

# Moon proto files (imports AST protos)
set(MOON_PROTO_FILES
    ${MOON_PROTO_DIR}/moon.proto
)

# Generate C++ from AST proto files
set(PROTO_SRCS)
set(PROTO_HDRS)
foreach(PROTO_FILE ${AST_PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    list(APPEND PROTO_SRCS ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc)
    list(APPEND PROTO_HDRS ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h)
    
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc
               ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h
        COMMAND protobuf::protoc
        ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
             --proto_path=${AST_PROTO_DIR}
             ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}
        COMMENT "Running protoc on ${PROTO_NAME}.proto"
        VERBATIM
    )
endforeach()

# Generate C++ from Moon proto files (needs both proto paths for imports)
foreach(PROTO_FILE ${MOON_PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    list(APPEND PROTO_SRCS ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc)
    list(APPEND PROTO_HDRS ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h)
    
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc
               ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h
        COMMAND protobuf::protoc
        ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
             --proto_path=${AST_PROTO_DIR}
             --proto_path=${MOON_PROTO_DIR}
             ${PROTO_FILE}
        DEPENDS ${PROTO_FILE} ${AST_PROTO_FILES}
        COMMENT "Running protoc on ${PROTO_NAME}.proto"
        VERBATIM
    )
endforeach()

message(STATUS "Proto sources: ${PROTO_SRCS}")
message(STATUS "Proto headers: ${PROTO_HDRS}")

# LLVM is target-based in modern versions → prefer imported targets
# add_definitions(${LLVM_DEFINITIONS}) and include_directories(${LLVM_INCLUDE_DIRS})
# are old-style and should be avoided when possible.

# ------------------------------------------------------------------
# Create the core library (recommended)
# ------------------------------------------------------------------
add_library(sun_lib
    # Generated protobuf sources
    ${PROTO_SRCS}
    src/parser.cpp
    src/interpolated_string_parser.cpp
    # AST node implementations (in src/ast/)
    src/ast/expr_ast.cpp
    src/ast/prototype_ast.cpp
    src/ast/call_expr_ast.cpp
    src/qualified_name.cpp
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
    src/codegen/threads.cpp
    src/codegen/thread_utils.cpp
    src/semantic_analyzer.cpp
    src/semantic_analysis/scope_variables.cpp
    src/semantic_analysis/scope_lookup.cpp
    src/semantic_analysis/classes.cpp
    src/semantic_analysis/interfaces.cpp
    src/semantic_analysis/type_conversion.cpp
    src/semantic_analysis/type_inference.cpp
    src/semantic_analysis/captures.cpp
    src/semantic_analysis/analysis.cpp
    src/semantic_analysis/analysis_utils.cpp
    src/semantic_analysis/declaration_collection.cpp
    src/llvm_type_resolver.cpp
    src/borrow_checker/borrow_state.cpp
    src/borrow_checker/borrow_checker.cpp
    # Precompiled library support (moon format)
    src/moon/moon.cpp
    src/library_cache.cpp
    src/module_linker.cpp
    src/module_types.cpp
    src/metadata_extractor.cpp
    # Debug utilities
    src/debug/ast_dot_generator.cpp
    src/debug/scope_tree_generator.cpp
    # AST serialization
    src/serialization/ast_serializer.cpp
    src/serialization/ast_deserializer.cpp
    # AST caching
    src/ast_cache.cpp
    # Add other implementation files here (but NOT main.cpp)
)

# Public headers go in include/SunCompiler/ or include/
target_include_directories(sun_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>  # For generated proto headers
        $<INSTALL_INTERFACE:include>  # For potential installation later
    PRIVATE
        src  # If you have private headers in src/
)

# Link LLVM the modern way (target-based)
target_link_libraries(sun_lib PUBLIC LLVM protobuf::libprotobuf)  # Add more components as needed

# Ensure the library uses the same definitions/includes as LLVM
target_compile_definitions(sun_lib PUBLIC ${LLVM_DEFINITIONS})
target_include_directories(sun_lib SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

# ------------------------------------------------------------------
# Main executable
# ------------------------------------------------------------------
add_executable(sun src/main.cpp)
target_link_libraries(sun PRIVATE sun_lib)

# ------------------------------------------------------------------
# Language Server Protocol executable
# ------------------------------------------------------------------
add_executable(sun-lsp src/lsp/main.cpp)
target_link_libraries(sun-lsp PRIVATE sun_lib)

# ------------------------------------------------------------------
# Precompile stdlib as .moon for faster test imports
# ------------------------------------------------------------------
option(SKIP_STDLIB "Skip building the precompiled stdlib.moon" OFF)

if(NOT SKIP_STDLIB)
    set(STDLIB_MOON ${CMAKE_BINARY_DIR}/stdlib.moon)
    set(STDLIB_ENTRYPOINT ${CMAKE_SOURCE_DIR}/stdlib/stdlib.sun)
    set(STDLIB_SOURCES
        ${STDLIB_ENTRYPOINT}
        ${CMAKE_SOURCE_DIR}/stdlib/print.sun
        ${CMAKE_SOURCE_DIR}/stdlib/allocator.sun
        ${CMAKE_SOURCE_DIR}/stdlib/unique.sun
        ${CMAKE_SOURCE_DIR}/stdlib/errors.sun
        ${CMAKE_SOURCE_DIR}/stdlib/slice.sun
        ${CMAKE_SOURCE_DIR}/stdlib/contiguous_buffer.sun
        ${CMAKE_SOURCE_DIR}/stdlib/vec.sun
        ${CMAKE_SOURCE_DIR}/stdlib/matrix.sun
        ${CMAKE_SOURCE_DIR}/stdlib/string.sun
        ${CMAKE_SOURCE_DIR}/stdlib/map.sun
        ${CMAKE_SOURCE_DIR}/stdlib/linked_list.sun
        ${CMAKE_SOURCE_DIR}/stdlib/mutex.sun
        ${CMAKE_SOURCE_DIR}/stdlib/math.sun
    )

    add_custom_command(
        OUTPUT ${STDLIB_MOON}
        COMMAND $<TARGET_FILE:sun> --emit-moon -o ${STDLIB_MOON} ${STDLIB_ENTRYPOINT}
        DEPENDS sun ${STDLIB_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Building precompiled stdlib.moon"
        VERBATIM
    )

    add_custom_target(stdlib_moon ALL DEPENDS ${STDLIB_MOON})
endif()

# ------------------------------------------------------------------
# Testing (optional, can be disabled)
# ------------------------------------------------------------------
include(CTest)  # Enables add_test and creates BUILD_TESTING option
option(BUILD_TESTING "Build the tests" ON)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)  # We'll create this folder next
endif()