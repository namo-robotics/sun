cmake_minimum_required(VERSION 3.22)
project(SunCompiler LANGUAGES C CXX)

include(CTest)  # Defines BUILD_TESTING before creating any test targets.

# CUDA CI reuses the compiler and standard library from the Linux build.
# Return before discovering compiler development dependencies or adding its targets.
set(SUN_CUDA_PREBUILT_DIR "" CACHE PATH "Directory containing sun and stdlib.moon for a CUDA-only build")
if(SUN_CUDA_PREBUILT_DIR)
    foreach(artifact sun stdlib.moon)
        if(NOT EXISTS "${SUN_CUDA_PREBUILT_DIR}/${artifact}")
            message(FATAL_ERROR "SUN_CUDA_PREBUILT_DIR is missing ${artifact}")
        endif()
    endforeach()
    add_executable(sun IMPORTED GLOBAL)
    set_target_properties(sun PROPERTIES IMPORTED_LOCATION "${SUN_CUDA_PREBUILT_DIR}/sun")
    configure_file("${SUN_CUDA_PREBUILT_DIR}/stdlib.moon"
                   "${CMAKE_BINARY_DIR}/stdlib.moon" COPYONLY)
    add_custom_target(stdlib_moon)
    add_subdirectory(cuda)
    return()
endif()

# ------------------------------------------------------------------
# General settings
# ------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # Useful for clang-tidy, IDEs, etc.

# Instantiate templates while building precompiled headers (clang)
set(CMAKE_PCH_INSTANTIATE_TEMPLATES ON)

# lld links debug binaries much faster than the default bfd linker
# (clang-only, like the -Xclang PCH flags below — GCC configures still work).
# Not on macOS: Apple's clang has no bundled lld and ld64 is fine.
set(SUN_BUILD_LINKER "${CMAKE_LINKER}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT APPLE)
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-prog-name=ld.lld
        OUTPUT_VARIABLE SUN_BUILD_LINKER OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT IS_ABSOLUTE "${SUN_BUILD_LINKER}")
        find_program(SUN_LLD_EXECUTABLE NAMES "${SUN_BUILD_LINKER}" REQUIRED)
        set(SUN_BUILD_LINKER "${SUN_LLD_EXECUTABLE}")
    endif()
    add_link_options("-fuse-ld=${SUN_BUILD_LINKER}")
endif()

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

# Native C header parsing for sun bindgen uses the matching LLVM toolchain.
find_path(SUN_LIBCLANG_INCLUDE_DIR clang-c/Index.h
          HINTS ${LLVM_INCLUDE_DIRS} REQUIRED)
find_library(SUN_LIBCLANG_LIBRARY NAMES clang-20 clang
             HINTS ${LLVM_LIBRARY_DIRS} REQUIRED)

# ------------------------------------------------------------------
# Protobuf (for AST serialization)
# ------------------------------------------------------------------
# Bundled by default: one pinned protobuf, built from source and linked
# statically, so every platform compiles the same descriptor API and protoc
# generates code matching the runtime exactly. System protobuf versions
# drift hard (Homebrew ships the Abseil generation with a different
# descriptor API than Ubuntu's 3.21), which repeatedly broke the macOS
# build. 3.21.12 is pinned because it matches Ubuntu 24.04 and is the last
# generation without the Abseil dependency. -DSUN_BUNDLED_PROTOBUF=OFF
# restores the system search (useful offline; needs libprotoc-dev).
option(SUN_BUNDLED_PROTOBUF
       "Build the pinned protobuf from source instead of using the system's"
       ON)
if(SUN_BUNDLED_PROTOBUF)
    include(FetchContent)
    set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
    set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # Static archives linked into executables must be relocatable.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    FetchContent_Declare(protobuf
        URL https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protobuf-cpp-3.21.12.tar.gz
        URL_HASH SHA256=4eab9b524aa5913c6fffb20b2a8abf5ef7f95a80bc0701f3a6dbb4c607f73460
    )
    # protobuf 3.21 predates C++20 cleanups; build it as C++17 (the standard
    # Ubuntu builds it with). The ABI is unaffected.
    set(SUN_SAVED_CXX_STANDARD ${CMAKE_CXX_STANDARD})
    set(CMAKE_CXX_STANDARD 17)
    FetchContent_MakeAvailable(protobuf)
    set(CMAKE_CXX_STANDARD ${SUN_SAVED_CXX_STANDARD})
    message(STATUS "Using bundled protobuf 3.21.12 (static)")
else()
    find_package(Protobuf REQUIRED)
    message(STATUS "Found system Protobuf ${Protobuf_VERSION}")
endif()

# ------------------------------------------------------------------
# Generate Protobuf sources (AST serialization + Moon library)
# ------------------------------------------------------------------
set(AST_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/ast)
set(MOON_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/moon_bundling)

# AST proto files
set(AST_PROTO_FILES
    ${AST_PROTO_DIR}/types.proto
    ${AST_PROTO_DIR}/ast.proto
    ${AST_PROTO_DIR}/analysis.proto
)

# Bundle and ABI schemas (Moon also imports the AST schemas).
set(BUNDLE_PROTO_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/abi/compatibility.proto
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
foreach(PROTO_FILE ${BUNDLE_PROTO_FILES})
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
             --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/include/abi
             ${PROTO_FILE}
        DEPENDS ${PROTO_FILE} ${AST_PROTO_FILES}
                ${CMAKE_CURRENT_SOURCE_DIR}/include/abi/compatibility.proto
        COMMENT "Running protoc on ${PROTO_NAME}.proto"
        VERBATIM
    )
endforeach()

# Validate and embed support before compiling any ABI consumer.
add_executable(sun_embed_abi_support
    cmake/abi/embed_support.cpp src/abi/support_manifest.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/compatibility.pb.cc)
target_include_directories(sun_embed_abi_support PRIVATE include ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(sun_embed_abi_support PRIVATE protobuf::libprotobuf)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/sun_abi_support.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND sun_embed_abi_support ${CMAKE_CURRENT_SOURCE_DIR}/src/abi/compiler_support.textproto
        ${CMAKE_CURRENT_BINARY_DIR}/generated/sun_abi_support.h
    DEPENDS sun_embed_abi_support src/abi/compiler_support.textproto
    COMMENT "Validating and embedding compiler ABI support"
    VERBATIM)
add_custom_target(sun_abi_support DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/sun_abi_support.h)

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
    # Driver: pipeline orchestration
    src/driver/driver.cpp
    src/driver/build_record.cpp
    src/driver/input_hash.cpp
    src/driver/c_bindings.cpp
    src/driver/bindgen/generator.cpp
    src/driver/manifest_processor.cpp
    src/driver/sun_config.cpp
    src/driver/package.cpp
    src/driver/git_source.cpp
    # Parsing
    src/parsing/parser.cpp
    src/parsing/interpolated_string_parser.cpp
    src/parsing/lowering_pass.cpp
    src/parsing/formatter.cpp
    src/parsing/doc_comments.cpp
    # AST
    src/ast/expr_ast.cpp
    src/ast/prototype_ast.cpp
    src/ast/call_expr_ast.cpp
    src/ast/ast_children.cpp
    src/ast/ast_utils.cpp
    src/ast/control_flow.cpp
    src/ast/free_variables.cpp
    # Shared types
    src/types/type.cpp
    src/types/type_identity.cpp
    src/types/nominal_type.cpp
    src/types/reference_type.cpp
    src/types/pointer_types.cpp
    src/types/class_type.cpp
    src/types/argument_compatibility.cpp
    src/types/type_utils.cpp
    # Semantic analysis
    src/semantic_analysis/access_checker.cpp
    src/semantic_analysis/analysis.cpp
    src/semantic_analysis/body_analysis/analysis_control_flow.cpp
    src/semantic_analysis/body_analysis/analysis_statements.cpp
    src/semantic_analysis/body_analysis/body_analyzer.cpp
    src/semantic_analysis/body_analysis/module_assignments.cpp
    src/semantic_analysis/body_analysis/statement_dispatch.cpp
    src/semantic_analysis/body_analysis/type_guards.cpp
    src/semantic_analysis/call_analysis/argument_conversion.cpp
    src/semantic_analysis/call_analysis/call_analyzer.cpp
    src/semantic_analysis/call_analysis/call_result_resolution.cpp
    src/semantic_analysis/class_analysis/class_definitions.cpp
    src/semantic_analysis/class_analysis/field_initialization.cpp
    src/semantic_analysis/class_analysis/packed_classes.cpp
    src/semantic_analysis/constants/constant_evaluator.cpp
    src/semantic_analysis/constants/constant_value.cpp
    src/semantic_analysis/declaration_analysis/captures.cpp
    src/semantic_analysis/declaration_analysis/declaration_analyzer.cpp
    src/semantic_analysis/declaration_analysis/declaration_dispatch.cpp
    src/semantic_analysis/declaration_analysis/signatures.cpp
    src/semantic_analysis/declaration_analysis/declaration_rules.cpp
    src/semantic_analysis/declaration_analysis/variable_declarations.cpp
    src/semantic_analysis/declaration_table.cpp
    src/semantic_analysis/enum_analysis/enum_analyzer.cpp
    src/semantic_analysis/expression_analysis/analysis_expressions.cpp
    src/semantic_analysis/expression_analysis/analysis_utils.cpp
    src/semantic_analysis/expression_analysis/conditional_expressions.cpp
    src/semantic_analysis/expression_analysis/expression_analyzer.cpp
    src/semantic_analysis/expression_analysis/expression_dispatch.cpp
    src/semantic_analysis/expression_analysis/expression_properties.cpp
    src/semantic_analysis/expression_analysis/expression_resolution.cpp
    src/semantic_analysis/expression_analysis/lambdas.cpp
    src/semantic_analysis/generic_analysis/generic_specializer.cpp
    src/semantic_analysis/generic_analysis/generic_type_arguments.cpp
    src/semantic_analysis/interface_analysis/interface_analyzer.cpp
    src/semantic_analysis/passes/body_analysis_pass.cpp
    src/semantic_analysis/passes/declaration_collection_pass.cpp
    src/semantic_analysis/passes/declaration_identity_pass.cpp
    src/semantic_analysis/passes/declaration_naming_pass.cpp
    src/semantic_analysis/passes/field_initializer_preparation_pass.cpp
    src/semantic_analysis/passes/global_initializer_evaluation_pass.cpp
    src/semantic_analysis/passes/global_registration_pass.cpp
    src/semantic_analysis/passes/import_completion_pass.cpp
    src/semantic_analysis/passes/import_dependency_validation_pass.cpp
    src/semantic_analysis/passes/import_library_declarations_pass.cpp
    src/semantic_analysis/passes/specialization_body_analysis_pass.cpp
    src/semantic_analysis/passes/type_registration_pass.cpp
    src/semantic_analysis/portable_declaration_key.cpp
    src/semantic_analysis/scope_declarations.cpp
    src/semantic_analysis/scope_lookup.cpp
    src/semantic_analysis/semantic_context.cpp
    src/semantic_analysis/semantic_pipeline.cpp
    src/semantic_analysis/specialization_key.cpp
    src/semantic_analysis/symbol_names.cpp
    src/semantic_analysis/type_analysis/generic_type_arguments.cpp
    src/semantic_analysis/type_analysis/type_checking.cpp
    src/semantic_analysis/type_analysis/type_inferer.cpp
    src/semantic_analysis/type_analysis/type_resolver.cpp
    src/semantic_analysis/type_analysis/type_rules.cpp
    src/semantic_analysis/type_analysis/builtin_methods.cpp
    src/semantic_analysis/type_analysis/type_traits.cpp
    # Borrow checker
    src/borrow_checker/borrow_state.cpp
    src/borrow_checker/borrow_checker.cpp
    # Codegen
    src/codegen/codegen_visitor.cpp
    src/codegen/debug_info_builder.cpp
    src/codegen/llvm_type_resolver.cpp
    src/codegen/support/scalar_ops.cpp
    src/codegen/support/struct_access.cpp
    src/codegen/scopes/scope_manager.cpp
    src/codegen/classes/class_generator.cpp
    src/codegen/classes/classes.cpp
    src/codegen/enums/enum_generator.cpp
    src/codegen/enums/drops.cpp
    src/codegen/classes/interfaces.cpp
    src/codegen/functions/function_generator.cpp
    src/codegen/functions/function_registry.cpp
    src/codegen/functions/functions_lambdas.cpp
    src/codegen/functions/return_statements.cpp
    src/codegen/variables/lvalues.cpp
    src/codegen/variables/variable_creation.cpp
    src/codegen/variables/variable_generator.cpp
    src/codegen/variables/variable_references.cpp
    src/codegen/expressions/arrays.cpp
    src/codegen/expressions/block_expressions.cpp
    src/codegen/expressions/call_expressions.cpp
    src/codegen/expressions/if_expressions.cpp
    src/codegen/expressions/match_expressions.cpp
    src/codegen/loops/loop_generator.cpp
    src/codegen/loops/loops.cpp
    src/codegen/errors/error_generator.cpp
    src/codegen/errors/error_handling.cpp
    src/codegen/abi/aapcs64/aapcs64.cpp
    src/codegen/abi/c_abi.cpp
    src/codegen/abi/extern_c.cpp
    src/codegen/abi/sysv/sysv_x86_64.cpp
    src/codegen/intrinsics/signals.cpp
    src/codegen/intrinsics/atomic.cpp
    src/codegen/intrinsics/declarations.cpp
    src/codegen/intrinsics/simd.cpp
    src/codegen/intrinsics/bits.cpp
    src/codegen/intrinsics/builtins.cpp
    src/codegen/intrinsics/generic.cpp
    src/codegen/intrinsics/intrinsics_generator.cpp
    src/codegen/intrinsics/io.cpp
    src/codegen/intrinsics/memory.cpp
    src/codegen/intrinsics/network.cpp
    src/codegen/intrinsics/print.cpp
    src/codegen/intrinsics/thread_utils.cpp
    src/codegen/intrinsics/threads.cpp
    # AST serialization
    src/serialization/ast_serializer.cpp
    src/serialization/ast_deserializer.cpp
    # Structured terminal diagnostics
    src/support/terminal_format.cpp
    # ABI requirements and compatibility decisions
    src/abi/cpu_requirements.cpp
    src/abi/compatibility.cpp
    src/abi/payload_requirements.cpp
    src/abi/semantic_requirements.cpp
    src/abi/native_requirements.cpp
    src/abi/nominal_identity.cpp
    src/abi/library_abi_snapshot.cpp
    src/abi/support_inventory.cpp
    src/abi/support_manifest.cpp
    src/abi/lowering_descriptors.cpp
    src/abi/boundary_requirements.cpp
    src/abi/producer_identity.cpp
    src/abi/abi_compatibility_checker.cpp
    src/abi/abi_validation_scope.cpp
    # Precompiled library support (moon format)
    src/moon_bundling/moon.cpp
    src/moon_bundling/archive_symbols.cpp
    src/moon_bundling/moon_builder.cpp
    src/moon_bundling/library_cache.cpp
    src/moon_bundling/moon_cache.cpp
    src/moon_bundling/module_linker.cpp
    src/moon_bundling/metadata_extractor.cpp
    src/moon_bundling/metadata_types.cpp
    src/moon_bundling/metadata_modules.cpp
    src/moon_bundling/proto_importer.cpp
    # Debug utilities
    src/debug/ast_dot_generator.cpp
    src/debug/scope_tree_generator.cpp
    # Language server helpers (the sun-lsp executable is src/lsp/main.cpp)
    src/lsp/declarations.cpp
    src/lsp/hover.cpp
    src/lsp/definition.cpp
    src/lsp/name_ranges.cpp
    src/lsp/references.cpp
    src/lsp/rename.cpp
    src/lsp/tests.cpp
    # Command-line front end (the sun executable is src/cli/main.cpp)
    src/cli/option_parser.cpp
    src/cli/usage.cpp
    src/cli/command_support.cpp
    src/cli/bindgen_command.cpp
    src/cli/fmt_command.cpp
    src/cli/test_command.cpp
    src/cli/bundle_command.cpp
    src/cli/compile_command.cpp
    src/cli/config_build_command.cpp
    src/cli/jit_command.cpp
    # Add other implementation files here (but NOT main.cpp)
)

# Build the token automaton once instead of rebuilding it in every compiler process.
add_executable(sun_token_table_generator scripts/generate_token_table.cpp)
target_include_directories(sun_token_table_generator PRIVATE ${CMAKE_SOURCE_DIR}/include)
set(TOKEN_TABLE ${CMAKE_BINARY_DIR}/generated/token_table.inc)
add_custom_command(
    OUTPUT ${TOKEN_TABLE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND sun_token_table_generator ${TOKEN_TABLE}
    DEPENDS sun_token_table_generator
    VERBATIM
)
add_custom_target(token_table DEPENDS ${TOKEN_TABLE})
add_dependencies(sun_lib token_table)
target_sources(sun_lib PRIVATE src/parsing/token_table.cpp)

# Public headers go in include/SunCompiler/ or include/
target_include_directories(sun_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>  # For generated proto headers
        $<INSTALL_INTERFACE:include>  # For potential installation later
    PRIVATE
        src  # If you have private headers in src/
)

target_include_directories(sun_lib SYSTEM PRIVATE ${SUN_LIBCLANG_INCLUDE_DIR})
target_link_libraries(sun_lib PUBLIC ${SUN_LIBCLANG_LIBRARY})

# Link LLVM the modern way (target-based)
# libprotoc provides google::protobuf::compiler::Importer, used to parse .proto
# schemas listed in a manifest's `protos:` at compile time (no libprotobuf
# dependency ends up in compiled Sun programs). Both protobuf libraries are
# linked STATICALLY into the sun binaries so users of a prebuilt compiler
# need no libprotobuf/libprotoc shared objects installed (libprotoc depends
# on libprotobuf, so they must be static together to avoid two copies of the
# protobuf runtime).
find_package(Threads REQUIRED)
if(SUN_BUNDLED_PROTOBUF)
    # The bundled build's own static targets, carrying their include dirs.
    target_link_libraries(sun_lib PUBLIC LLVM protobuf::libprotoc
                          protobuf::libprotobuf Threads::Threads)
else()
    find_library(PROTOBUF_STATIC_LIB NAMES libprotobuf.a
                 HINTS /opt/homebrew/lib /usr/local/lib)
    find_library(PROTOC_STATIC_LIB NAMES libprotoc.a
                 HINTS /opt/homebrew/lib /usr/local/lib)
    if(PROTOBUF_STATIC_LIB AND PROTOC_STATIC_LIB)
        message(STATUS "Linking protobuf statically: ${PROTOC_STATIC_LIB} ${PROTOBUF_STATIC_LIB}")
        target_include_directories(sun_lib PUBLIC ${Protobuf_INCLUDE_DIRS})
        target_link_libraries(sun_lib PUBLIC LLVM ${PROTOC_STATIC_LIB} ${PROTOBUF_STATIC_LIB} Threads::Threads)
    else()
        # libprotoc-dev provides both the headers (compiler/importer.h) and
        # the static archives; without it neither the static nor the shared
        # link works
        message(FATAL_ERROR "libprotoc static archive not found: install libprotoc-dev "
                            "(alongside libprotobuf-dev) to build the compiler, "
                            "or configure with -DSUN_BUNDLED_PROTOBUF=ON")
    endif()
endif()

# Ensure the library uses the same definitions/includes as LLVM
target_compile_definitions(sun_lib PUBLIC ${LLVM_DEFINITIONS})
target_include_directories(sun_lib SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

# Precompile the common headers (each TU otherwise parses ~330k preprocessed
# lines of AST + LLVM headers; measured ~2x per-file compile speedup)
target_precompile_headers(sun_lib PRIVATE
    include/ast.h
    include/codegen/codegen.h
    include/codegen/codegen_state.h
    include/codegen/codegen_visitor.h
    include/parsing/parser.h
    include/codegen/support/scalar_ops.h
    include/codegen/support/struct_access.h
    include/codegen/scopes/scope_manager.h
    include/codegen/classes/class_generator.h
    include/codegen/enums/enum_generator.h
    include/codegen/functions/function_generator.h
    include/codegen/functions/function_registry.h
    include/codegen/variables/variable_generator.h
    include/codegen/loops/loop_generator.h
    include/codegen/errors/error_generator.h
    include/codegen/intrinsics/intrinsics.h
    include/codegen/intrinsics/intrinsics_generator.h
)
# This file must include LLVM Mach-O headers before Protobuf introduces Apple
# Mach macros. The shared precompiled header would reverse that order.
set_source_files_properties(src/abi/native_requirements.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON)

# clang embeds a timestamp in PCHs by default, which defeats ccache
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(sun_lib PRIVATE -Xclang -fno-pch-timestamp)
endif()

# ------------------------------------------------------------------
# Main executable
# ------------------------------------------------------------------
# Build-time git hash for `sun --version` (regenerated every build, only
# rewritten when it changes). SUN_VERSION_NAME overrides the display name
# otherwise derived from a clean release tag or dev.
# Header-only template engine (inja + nlohmann/json, both vendored) for
# rendering synthesized Sun source like the test runner.
target_include_directories(sun_lib PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

# The test runner's Sun source lives in a template file and is embedded
# into the compiler as a header, so driver.cpp holds no Sun code inline.
set(TEST_RUNNER_TEMPLATE ${CMAKE_SOURCE_DIR}/src/driver/test_runner_template.inja.sun)
set(TEST_RUNNER_HEADER ${CMAKE_BINARY_DIR}/generated/test_runner_template.h)
add_custom_command(
    OUTPUT ${TEST_RUNNER_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${TEST_RUNNER_TEMPLATE}
        -DOUTPUT_FILE=${TEST_RUNNER_HEADER}
        -DVARIABLE_NAME=kTestRunnerTemplate
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${TEST_RUNNER_TEMPLATE} ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    COMMENT "Embedding test_runner_template.inja.sun")
add_custom_target(test_runner_template DEPENDS ${TEST_RUNNER_HEADER})
add_dependencies(sun_lib test_runner_template)

# Keep intrinsic signatures and documentation in compiler-owned Sun source.
set(INTRINSIC_SOURCE ${CMAKE_SOURCE_DIR}/src/codegen/intrinsics/intrinsics.sun)
set(INTRINSIC_HEADER ${CMAKE_BINARY_DIR}/generated/intrinsic_source.h)
add_custom_command(
    OUTPUT ${INTRINSIC_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${INTRINSIC_SOURCE}
        -DOUTPUT_FILE=${INTRINSIC_HEADER}
        -DVARIABLE_NAME=kIntrinsicSource
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${INTRINSIC_SOURCE} ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    COMMENT "Embedding intrinsics.sun")
add_custom_target(intrinsic_source DEPENDS ${INTRINSIC_HEADER})
add_dependencies(sun_lib intrinsic_source)

# Release automation supplies a monotonic order within its named release series.
# Zero and an empty series identify builds without official ordering provenance.
set(SUN_RELEASE_ORDER "0" CACHE STRING "Official release order, or zero when unknown")
set(SUN_RELEASE_SERIES "" CACHE STRING "Namespace for official release ordering")
include(cmake/abi/ReleaseIdentity.cmake)

set(SUN_VERSION_NAME_ARGUMENT)
if(DEFINED SUN_VERSION_NAME)
    set(SUN_VERSION_NAME_ARGUMENT "-DSUN_VERSION_NAME=${SUN_VERSION_NAME}")
endif()
set(SUN_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/sun_version.h)
add_custom_target(sun_version
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DOUTPUT_FILE=${SUN_VERSION_HEADER}
        ${SUN_VERSION_NAME_ARGUMENT}
        -DSUN_RELEASE_ORDER=${SUN_RELEASE_ORDER}
        -DSUN_RELEASE_SERIES=${SUN_RELEASE_SERIES}
        -P ${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake
    BYPRODUCTS ${SUN_VERSION_HEADER}
    COMMENT "Updating sun_version.h"
    VERBATIM)
# The compiler's version is part of every .moon bundle's content hash
# (moon_builder.cpp), so the library needs the header too.
add_dependencies(sun_lib sun_version)

# Conservative experimental semantics include source, external headers and tools.
add_custom_target(sun_compatibility_fingerprint
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DBINARY_DIR=${CMAKE_BINARY_DIR}
        -DCOMPILER=${CMAKE_CXX_COMPILER}
        "-DTOOLS=${CMAKE_C_COMPILER};${SUN_BUILD_LINKER};${CMAKE_AR};${CMAKE_RANLIB};${CMAKE_COMMAND};$<TARGET_FILE:protobuf::protoc>"
        "-DEXTERNAL_INCLUDES=${LLVM_INCLUDE_DIRS};${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}"
        -P ${CMAKE_SOURCE_DIR}/cmake/abi/CompatibilityFingerprint.cmake
    DEPENDS ${PROTO_SRCS} ${PROTO_HDRS}
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/sun_compatibility.h
    COMMENT "Updating experimental compatibility implementation fingerprint"
    VERBATIM)
add_dependencies(sun_compatibility_fingerprint sun_abi_support)
add_dependencies(sun_lib sun_compatibility_fingerprint sun_abi_support)

add_executable(sun src/cli/main.cpp)
find_package(LibArchive REQUIRED)
target_link_libraries(sun_lib PUBLIC LibArchive::LibArchive)

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
    # Only the entrypoints are named here. Which .sun files, bundles, protos
    # and archives each artifact is built from is decided by the manifests,
    # which only the compiler reads. So the commands below are custom
    # targets, which run on every build, and let the compiler decide whether
    # there is work to do: each artifact records a hash of the inputs it was
    # built from, and one whose inputs are unchanged is left untouched. The
    # artifacts are declared as BYPRODUCTS so rules that consume them still
    # know where they come from.

    # Release packages ship optimized Sun bundles as well as the compiler.
    # Other configurations retain debug info; Debug also skips optimization.
    set(SUN_BUNDLE_FLAGS
        "$<$<NOT:$<CONFIG:Release,MinSizeRel>>:-g>"
        "$<$<CONFIG:Debug>:-O0>")
    set(BUILD_MOON_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/build-moon.sh)
    set(STDLIB_MOON ${CMAKE_BINARY_DIR}/stdlib.moon)
    set(STDLIB_ENTRYPOINT ${CMAKE_SOURCE_DIR}/stdlib/stdlib.sun)

    # The paths sun-config.json's entrypoints name (relative to the config's
    # folder, i.e. <source>/build). ctest runs the test binaries
    # sequentially: the runner otherwise starts one thread per test, and the
    # env/cwd tests want the process to themselves.
    set(STDLIB_TEST_BIN ${CMAKE_BINARY_DIR}/stdlib_tests)
    set(TLS_MOON ${CMAKE_BINARY_DIR}/tls.moon)
    set(TLS_ENTRYPOINT ${CMAKE_SOURCE_DIR}/tls/tls.sun)
    set(TLS_TEST_BIN ${CMAKE_BINARY_DIR}/tls_tests)

    # Keep whole-config packaging available explicitly. Independent product targets
    # let compiler tests rebuild stdlib without packaging unrelated native bundles.
    add_custom_target(sun_config_products
        COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/build-config.sh
                $<TARGET_FILE:sun> ${SUN_BUNDLE_FLAGS}
                ${CMAKE_SOURCE_DIR}/sun-config.json
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking all sun-config.json products"
        COMMAND_EXPAND_LISTS
        VERBATIM)
    add_dependencies(sun_config_products sun)

    add_custom_target(stdlib_moon ALL
        COMMAND bash ${BUILD_MOON_SCRIPT} $<TARGET_FILE:sun>
                ${STDLIB_ENTRYPOINT} ${STDLIB_MOON} ${SUN_BUNDLE_FLAGS}
        BYPRODUCTS ${STDLIB_MOON}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking precompiled stdlib.moon"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    add_dependencies(stdlib_moon sun)

    if(BUILD_TESTING)
        add_custom_target(stdlib_sun_tests ALL
            COMMAND $<TARGET_FILE:sun> -c
                    ${SUN_BUNDLE_FLAGS}
                    -o ${STDLIB_TEST_BIN} ${STDLIB_ENTRYPOINT}
            BYPRODUCTS ${STDLIB_TEST_BIN}_test
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Checking stdlib Sun test binary"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
        add_dependencies(stdlib_sun_tests sun)
    endif()

    # --lib-path points at this build directory, including out-of-tree builds.
    add_custom_target(tls_moon ALL
        COMMAND bash ${BUILD_MOON_SCRIPT} $<TARGET_FILE:sun>
                ${TLS_ENTRYPOINT} ${TLS_MOON} ${SUN_BUNDLE_FLAGS}
        BYPRODUCTS ${TLS_MOON}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking precompiled tls.moon (with static OpenSSL)"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    # tls imports stdlib.moon, so that has to be current first
    add_dependencies(tls_moon sun stdlib_moon)

    if(BUILD_TESTING)
        # A library with tests compiles to just its test binary.
        add_custom_target(tls_sun_tests ALL
            COMMAND $<TARGET_FILE:sun> -c
                    ${SUN_BUNDLE_FLAGS}
                    --lib-path ${CMAKE_BINARY_DIR}
                    -o ${TLS_TEST_BIN} ${TLS_ENTRYPOINT}
            BYPRODUCTS ${TLS_TEST_BIN}_test
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Checking tls Sun test binary"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
        add_dependencies(tls_sun_tests sun stdlib_moon)
    endif()

    # Cross-compiled stdlib bundles live in per-target directories under the
    # same canonical name (build/<triple>/stdlib.moon). Bundles are resolved
    # by exact name; the metadata's target triple validates the match.
    set(SUN_CROSS_STDLIB_TARGETS "aarch64-linux-gnu;arm64-apple-darwin" CACHE STRING
        "Triples to build cross stdlib .moon bundles for (empty disables)")
    option(SUN_CROSS_TLS_AARCH64 "Build the AArch64 Linux TLS bundle" OFF)
    set(CROSS_MOON_TARGETS "")
    foreach(cross_triple ${SUN_CROSS_STDLIB_TARGETS})
        set(CROSS_MOON ${CMAKE_BINARY_DIR}/${cross_triple}/stdlib.moon)
        # The combined config owns both AArch64 outputs when TLS is enabled.
        if(NOT (SUN_CROSS_TLS_AARCH64 AND cross_triple MATCHES "^aarch64-linux-(gnu|musl)$"))
            add_custom_target(stdlib_cross_moon_${cross_triple}
                COMMAND bash ${BUILD_MOON_SCRIPT} $<TARGET_FILE:sun>
                        ${STDLIB_ENTRYPOINT} ${CROSS_MOON}
                        ${SUN_BUNDLE_FLAGS} --target ${cross_triple}
                BYPRODUCTS ${CROSS_MOON}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMENT "Checking precompiled ${cross_triple}/stdlib.moon"
                COMMAND_EXPAND_LISTS
                VERBATIM
            )
            add_dependencies(stdlib_cross_moon_${cross_triple} sun)
            list(APPEND CROSS_MOON_TARGETS stdlib_cross_moon_${cross_triple})
        endif()
    endforeach()
    if(CROSS_MOON_TARGETS OR SUN_CROSS_TLS_AARCH64)
        add_custom_target(stdlib_cross_moons ALL)
        if(CROSS_MOON_TARGETS)
            add_dependencies(stdlib_cross_moons ${CROSS_MOON_TARGETS})
        endif()
    endif()

    if(SUN_CROSS_TLS_AARCH64)
        if(NOT "aarch64-linux-gnu" IN_LIST SUN_CROSS_STDLIB_TARGETS)
            message(FATAL_ERROR "AArch64 TLS requires aarch64-linux-gnu in SUN_CROSS_STDLIB_TARGETS")
        endif()
        set(CROSS_BUNDLE_OUTPUTS "")
        foreach(cross_triple aarch64-linux-gnu aarch64-linux-musl)
            list(APPEND CROSS_BUNDLE_OUTPUTS
                ${CMAKE_SOURCE_DIR}/build/${cross_triple}/stdlib.moon
                ${CMAKE_SOURCE_DIR}/build/${cross_triple}/tls.moon)
        endforeach()
        set(CROSS_OPENSSL_DIR ${CMAKE_SOURCE_DIR}/third_party/openssl/aarch64-linux-musl)
        foreach(archive libssl.a libcrypto.a)
            if(NOT EXISTS ${CROSS_OPENSSL_DIR}/${archive})
                message(FATAL_ERROR "Run scripts/fetch-openssl.sh --arch aarch64 before building cross TLS")
            endif()
        endforeach()
        set(BUILD_CROSS_BUNDLES_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/build-cross-bundles.sh)
        add_custom_target(cross_bundles_aarch64
            COMMAND bash ${BUILD_CROSS_BUNDLES_SCRIPT} $<TARGET_FILE:sun>
                    ${SUN_BUNDLE_FLAGS}
            BYPRODUCTS ${CROSS_BUNDLE_OUTPUTS}
            COMMENT "Checking AArch64 stdlib and TLS from sun-config.json"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
        add_dependencies(cross_bundles_aarch64 sun)
        # The shared config names source-relative outputs. Package builds
        # collect those artifacts in their separate CMake build directory.
        set(CROSS_PACKAGE_OUTPUTS "")
        foreach(cross_triple aarch64-linux-gnu aarch64-linux-musl)
            set(CROSS_BUNDLE_DIR ${CMAKE_SOURCE_DIR}/build/${cross_triple})
            set(CROSS_PACKAGE_DIR ${CMAKE_BINARY_DIR}/${cross_triple})
            if(NOT CROSS_PACKAGE_DIR STREQUAL CROSS_BUNDLE_DIR)
                add_custom_command(
                    OUTPUT ${CROSS_PACKAGE_DIR}/stdlib.moon ${CROSS_PACKAGE_DIR}/tls.moon
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CROSS_PACKAGE_DIR}
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            ${CROSS_BUNDLE_DIR}/stdlib.moon ${CROSS_BUNDLE_DIR}/tls.moon
                            ${CROSS_PACKAGE_DIR}
                    DEPENDS ${CROSS_BUNDLE_DIR}/stdlib.moon ${CROSS_BUNDLE_DIR}/tls.moon
                    VERBATIM
                )
            endif()
            list(APPEND CROSS_PACKAGE_OUTPUTS
                ${CROSS_PACKAGE_DIR}/stdlib.moon ${CROSS_PACKAGE_DIR}/tls.moon)
        endforeach()
        add_custom_target(tls_cross_moon ALL DEPENDS ${CROSS_PACKAGE_OUTPUTS})
        add_dependencies(tls_cross_moon cross_bundles_aarch64)
        # One target owns the shared outputs so parallel Make builds cannot
        # run the bundle command twice or delete an output still in use.
        add_dependencies(stdlib_cross_moons tls_cross_moon)
    endif()

endif()

# ------------------------------------------------------------------
# Testing (optional, can be disabled)
# ------------------------------------------------------------------
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

option(SUN_ENABLE_CUDA "Build the optional NVIDIA matrix library" OFF)
if(SUN_ENABLE_CUDA)
    add_subdirectory(cuda)
endif()
