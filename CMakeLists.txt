cmake_minimum_required(VERSION 3.20)
project(SunCompiler LANGUAGES C CXX)

# ------------------------------------------------------------------
# General settings
# ------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # Useful for clang-tidy, IDEs, etc.

# Instantiate templates while building precompiled headers (clang)
set(CMAKE_PCH_INSTANTIATE_TEMPLATES ON)

# lld links debug binaries much faster than the default bfd linker
# (clang-only, like the -Xclang PCH flags below — GCC configures still work)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_link_options(-fuse-ld=lld)
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

# ------------------------------------------------------------------
# Find Protobuf (for AST serialization)
# ------------------------------------------------------------------
find_package(Protobuf REQUIRED)
message(STATUS "Found Protobuf ${Protobuf_VERSION}")

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
    # Driver: pipeline orchestration (the sun executable is src/driver/main.cpp)
    src/driver/driver.cpp
    src/driver/manifest_processor.cpp
    src/driver/sun_config.cpp
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
    # Semantic analysis
    src/semantic_analysis/qualified_name.cpp
    src/semantic_analysis/semantic_context.cpp
    src/semantic_analysis/scope_lookup.cpp
    src/semantic_analysis/packed_classes.cpp
    src/semantic_analysis/generic_specializer.cpp
    src/semantic_analysis/argument_conversion.cpp
    src/semantic_analysis/generic_type_arguments.cpp
    src/semantic_analysis/interfaces.cpp
    src/semantic_analysis/type_annotations.cpp
    src/semantic_analysis/type_inferer.cpp
    src/semantic_analysis/type_traits.cpp
    src/semantic_analysis/captures.cpp
    src/semantic_analysis/analysis.cpp
    src/semantic_analysis/analysis_declarations.cpp
    src/semantic_analysis/analysis_control_flow.cpp
    src/semantic_analysis/analysis_statements.cpp
    src/semantic_analysis/analysis_expressions.cpp
    src/semantic_analysis/analysis_utils.cpp
    src/semantic_analysis/declaration_collector.cpp
    src/semantic_analysis/access_checker.cpp
    src/semantic_analysis/type_rules.cpp
    src/semantic_analysis/symbol_names.cpp
    src/semantic_analysis/enums.cpp
    # Borrow checker
    src/borrow_checker/borrow_state.cpp
    src/borrow_checker/borrow_checker.cpp
    # Codegen
    src/codegen/codegen_visitor.cpp
    src/codegen/llvm_type_resolver.cpp
    src/codegen/debug_info_builder.cpp
    src/codegen/functions_lambdas.cpp
    src/codegen/arrays.cpp
    src/codegen/variable_creation.cpp
    src/codegen/variable_references.cpp
    src/codegen/lvalues.cpp
    src/codegen/call_expressions.cpp
    src/codegen/extern_c.cpp
    src/codegen/if_expressions.cpp
    src/codegen/match_expressions.cpp
    src/codegen/enums.cpp
    src/codegen/loops.cpp
    src/codegen/return_statements.cpp
    src/codegen/block_expressions.cpp
    src/codegen/classes.cpp
    src/codegen/error_handling.cpp
    src/codegen/threads.cpp
    src/codegen/thread_utils.cpp
    src/codegen/abi/aapcs64.cpp
    src/codegen/abi/c_abi.cpp
    src/codegen/abi/sysv_x86_64.cpp
    src/codegen/intrinsics/builtins.cpp
    src/codegen/intrinsics/generic.cpp
    src/codegen/intrinsics/io.cpp
    src/codegen/intrinsics/print.cpp
    src/codegen/intrinsics/memory.cpp
    src/codegen/intrinsics/atomic.cpp
    src/codegen/intrinsics/bits.cpp
    src/codegen/intrinsics/network.cpp
    # AST serialization
    src/serialization/ast_serializer.cpp
    src/serialization/ast_deserializer.cpp
    # Precompiled library support (moon format)
    src/moon_bundling/moon.cpp
    src/moon_bundling/moon_builder.cpp
    src/moon_bundling/library_cache.cpp
    src/moon_bundling/moon_cache.cpp
    src/moon_bundling/module_linker.cpp
    src/moon_bundling/module_types.cpp
    src/moon_bundling/metadata_extractor.cpp
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
# libprotoc provides google::protobuf::compiler::Importer, used to parse .proto
# schemas listed in a manifest's `protos:` at compile time (no libprotobuf
# dependency ends up in compiled Sun programs). Both protobuf libraries are
# linked STATICALLY into the sun binaries so users of a prebuilt compiler
# need no libprotobuf/libprotoc shared objects installed (libprotoc depends
# on libprotobuf, so they must be static together to avoid two copies of the
# protobuf runtime).
find_package(Threads REQUIRED)
find_library(PROTOBUF_STATIC_LIB NAMES libprotobuf.a)
find_library(PROTOC_STATIC_LIB NAMES libprotoc.a)
if(PROTOBUF_STATIC_LIB AND PROTOC_STATIC_LIB)
    message(STATUS "Linking protobuf statically: ${PROTOC_STATIC_LIB} ${PROTOBUF_STATIC_LIB}")
    target_include_directories(sun_lib PUBLIC ${Protobuf_INCLUDE_DIRS})
    target_link_libraries(sun_lib PUBLIC LLVM ${PROTOC_STATIC_LIB} ${PROTOBUF_STATIC_LIB} Threads::Threads)
else()
    # libprotoc-dev provides both the headers (compiler/importer.h) and the
    # static archives; without it neither the static nor the shared link works
    message(FATAL_ERROR "libprotoc static archive not found: install libprotoc-dev "
                        "(alongside libprotobuf-dev) to build the compiler")
endif()

# Ensure the library uses the same definitions/includes as LLVM
target_compile_definitions(sun_lib PUBLIC ${LLVM_DEFINITIONS})
target_include_directories(sun_lib SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

# Precompile the common headers (each TU otherwise parses ~330k preprocessed
# lines of AST + LLVM headers; measured ~2x per-file compile speedup)
target_precompile_headers(sun_lib PRIVATE
    include/ast.h
    include/codegen/codegen.h
    include/codegen/codegen_visitor.h
    include/parsing/parser.h
    include/codegen/intrinsics/intrinsics.h
)
# clang embeds a timestamp in PCHs by default, which defeats ccache
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(sun_lib PRIVATE -Xclang -fno-pch-timestamp)
endif()

# ------------------------------------------------------------------
# Main executable
# ------------------------------------------------------------------
# Build-time git hash for `sun --version` (regenerated every build, only
# rewritten when it changes). SUN_VERSION may be passed to override the
# version string read from debian/changelog.
set(SUN_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/sun_version.h)
add_custom_target(sun_version
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DOUTPUT_FILE=${SUN_VERSION_HEADER}
        -DSUN_VERSION=${SUN_VERSION}
        -P ${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake
    BYPRODUCTS ${SUN_VERSION_HEADER}
    COMMENT "Updating sun_version.h")

add_executable(sun src/driver/main.cpp)
add_dependencies(sun sun_version)
target_include_directories(sun PRIVATE ${CMAKE_BINARY_DIR}/generated)
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
        ${CMAKE_SOURCE_DIR}/stdlib/sys.sun
        ${CMAKE_SOURCE_DIR}/stdlib/option.sun
        ${CMAKE_SOURCE_DIR}/stdlib/iterator.sun
        ${CMAKE_SOURCE_DIR}/stdlib/slice.sun
        ${CMAKE_SOURCE_DIR}/stdlib/contiguous_buffer.sun
        ${CMAKE_SOURCE_DIR}/stdlib/vec.sun
        ${CMAKE_SOURCE_DIR}/stdlib/matrix.sun
        ${CMAKE_SOURCE_DIR}/stdlib/string.sun
        ${CMAKE_SOURCE_DIR}/stdlib/map.sun
        ${CMAKE_SOURCE_DIR}/stdlib/linked_list.sun
        ${CMAKE_SOURCE_DIR}/stdlib/mutex.sun
        ${CMAKE_SOURCE_DIR}/stdlib/math.sun
        ${CMAKE_SOURCE_DIR}/stdlib/networking.sun
        ${CMAKE_SOURCE_DIR}/stdlib/io.sun
        ${CMAKE_SOURCE_DIR}/stdlib/env.sun
        ${CMAKE_SOURCE_DIR}/stdlib/time.sun
        ${CMAKE_SOURCE_DIR}/stdlib/process.sun
        ${CMAKE_SOURCE_DIR}/stdlib/http.sun
        ${CMAKE_SOURCE_DIR}/stdlib/simple_math.sun
        ${CMAKE_SOURCE_DIR}/stdlib/proto_wire.sun
        ${CMAKE_SOURCE_DIR}/stdlib/json.sun
        ${CMAKE_SOURCE_DIR}/stdlib/bigint.sun
    )

    # Built with -g: debug compiles can step into stdlib; non-debug compiles
    # strip the bundle's debug info at link time.
    add_custom_command(
        OUTPUT ${STDLIB_MOON}
        COMMAND $<TARGET_FILE:sun> --emit-moon -g -o ${STDLIB_MOON} ${STDLIB_ENTRYPOINT}
        DEPENDS sun ${STDLIB_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Building precompiled stdlib.moon"
        VERBATIM
    )

    add_custom_target(stdlib_moon ALL DEPENDS ${STDLIB_MOON})

    # Cross-compiled stdlib bundles live in per-target directories under the
    # same canonical name (build/<triple>/stdlib.moon). Bundles are resolved
    # by exact name; the metadata's target triple validates the match.
    set(SUN_CROSS_STDLIB_TARGETS "aarch64-linux-gnu" CACHE STRING
        "Triples to build cross stdlib .moon bundles for (empty disables)")
    set(CROSS_MOONS "")
    foreach(cross_triple ${SUN_CROSS_STDLIB_TARGETS})
        set(CROSS_MOON ${CMAKE_BINARY_DIR}/${cross_triple}/stdlib.moon)
        add_custom_command(
            OUTPUT ${CROSS_MOON}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/${cross_triple}
            COMMAND $<TARGET_FILE:sun> --emit-moon -g --target ${cross_triple} -o ${CROSS_MOON} ${STDLIB_ENTRYPOINT}
            DEPENDS sun ${STDLIB_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Building precompiled ${cross_triple}/stdlib.moon"
            VERBATIM
        )
        list(APPEND CROSS_MOONS ${CROSS_MOON})
    endforeach()
    if(CROSS_MOONS)
        add_custom_target(stdlib_cross_moons ALL DEPENDS ${CROSS_MOONS})
    endif()

    # --------------------------------------------------------------
    # tls.moon — TLS client support, bundled separately from the stdlib
    # --------------------------------------------------------------
    # The bundle carries static OpenSSL archives (see the manifest's `libs:`),
    # so programs using TLS need no -lssl/-lcrypto and no OpenSSL installed.
    # Build the archives once with scripts/fetch-openssl.sh; without them
    # tls.moon is skipped rather than failing the build.
    set(SUN_OPENSSL_DIR ${CMAKE_SOURCE_DIR}/third_party/openssl/x86_64-linux-musl
        CACHE PATH "Directory holding the static libssl.a/libcrypto.a for tls.moon")

    if(EXISTS ${SUN_OPENSSL_DIR}/libssl.a AND EXISTS ${SUN_OPENSSL_DIR}/libcrypto.a)
        set(TLS_MOON ${CMAKE_BINARY_DIR}/tls.moon)
        set(TLS_ENTRYPOINT ${CMAKE_SOURCE_DIR}/tls/tls.sun)
        set(TLS_SOURCES
            ${TLS_ENTRYPOINT}
            ${CMAKE_SOURCE_DIR}/tls/openssl.sun
            ${CMAKE_SOURCE_DIR}/tls/tls_stream.sun
            ${CMAKE_SOURCE_DIR}/tls/https_client.sun
        )

        # sun-config.json at the repo root supplies $OPENSSL_LIBS; --lib-path
        # points at this build directory, which need not be <source>/build.
        add_custom_command(
            OUTPUT ${TLS_MOON}
            COMMAND $<TARGET_FILE:sun> --emit-moon -g
                    --lib-path ${CMAKE_BINARY_DIR}
                    -o ${TLS_MOON} ${TLS_ENTRYPOINT}
            DEPENDS sun ${STDLIB_MOON} ${TLS_SOURCES}
                    ${SUN_OPENSSL_DIR}/libssl.a ${SUN_OPENSSL_DIR}/libcrypto.a
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Building precompiled tls.moon (with static OpenSSL)"
            VERBATIM
        )

        add_custom_target(tls_moon ALL DEPENDS ${TLS_MOON})
    else()
        message(STATUS
            "tls.moon skipped: no static OpenSSL in ${SUN_OPENSSL_DIR}. "
            "Run ./scripts/fetch-openssl.sh to enable TLS support.")
    endif()
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