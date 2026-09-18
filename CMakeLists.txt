cmake_minimum_required(VERSION 3.20)
project(SunCompiler LANGUAGES C CXX)

include(CTest)  # Defines BUILD_TESTING before creating any test targets.

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
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT APPLE)
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
    # Driver: pipeline orchestration
    src/driver/driver.cpp
    src/driver/build_record.cpp
    src/driver/input_hash.cpp
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
    src/ast/control_flow.cpp
    # Semantic analysis
    src/semantic_analysis/portable_declaration_key.cpp
    src/semantic_analysis/specialization_key.cpp
    src/semantic_analysis/declaration_identity_pass.cpp
    src/semantic_analysis/semantic_context.cpp
    src/semantic_analysis/scope_declarations.cpp
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
    src/semantic_analysis/field_initialization.cpp
    src/semantic_analysis/body_analyzer.cpp
    src/semantic_analysis/analysis.cpp
    src/semantic_analysis/analysis_declarations.cpp
    src/semantic_analysis/analysis_control_flow.cpp
    src/semantic_analysis/analysis_statements.cpp
    src/semantic_analysis/analysis_expressions.cpp
    src/semantic_analysis/analysis_utils.cpp
    src/semantic_analysis/call_analyzer.cpp
    src/semantic_analysis/field_initializer_preparation_pass.cpp
    src/semantic_analysis/declaration_naming_pass.cpp
    src/semantic_analysis/semantic_pipeline.cpp
    src/semantic_analysis/declaration_collection_pass.cpp
    src/semantic_analysis/access_checker.cpp
    src/semantic_analysis/type_rules.cpp
    src/semantic_analysis/symbol_names.cpp
    src/semantic_analysis/enum_analyzer.cpp
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
    src/codegen/abi/aapcs64.cpp
    src/codegen/abi/c_abi.cpp
    src/codegen/abi/extern_c.cpp
    src/codegen/abi/sysv_x86_64.cpp
    src/codegen/intrinsics/atomic.cpp
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
    src/cli/fmt_command.cpp
    src/cli/test_command.cpp
    src/cli/bundle_command.cpp
    src/cli/compile_command.cpp
    src/cli/config_build_command.cpp
    src/cli/jit_command.cpp
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

set(SUN_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/sun_version.h)
add_custom_target(sun_version
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DOUTPUT_FILE=${SUN_VERSION_HEADER}
        -DSUN_VERSION=${SUN_VERSION}
        -P ${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake
    BYPRODUCTS ${SUN_VERSION_HEADER}
    COMMENT "Updating sun_version.h")
# The compiler's version is part of every .moon bundle's content hash
# (moon_builder.cpp), so the library needs the header too.
add_dependencies(sun_lib sun_version)

add_executable(sun src/cli/main.cpp)
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
    # targets, which run on every build, and pass --skip-if-unchanged so the
    # compiler decides whether there is work to do: each artifact records a
    # hash of the inputs it was built from, and one whose inputs are
    # unchanged is left untouched. The
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

    # The canonical build drives everything from sun-config.json: one
    # `sun -c sun-config.json` builds stdlib.moon, the stdlib test binary,
    # tls.moon and the tls test binary, exactly as a developer running the
    # same command would get (config entrypoints resolve relative to the
    # config's folder). Out-of-tree builds and builds without tests use
    # explicit per-artifact commands.
    # AArch64 Linux config outputs use a target directory. Explicit commands
    # retain the native CMake output paths on those hosts.
    if(BUILD_TESTING AND CMAKE_BINARY_DIR STREQUAL ${CMAKE_SOURCE_DIR}/build
       AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$"))
        add_custom_target(sun_config_products ALL
            COMMAND $<TARGET_FILE:sun> -c --skip-if-unchanged ${SUN_BUNDLE_FLAGS}
                    ${CMAKE_SOURCE_DIR}/sun-config.json
            BYPRODUCTS ${STDLIB_MOON} ${STDLIB_TEST_BIN}_test ${TLS_MOON} ${TLS_TEST_BIN}_test
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Checking sun-config.json products (stdlib.moon, stdlib tests, tls.moon, tls tests)"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
        add_dependencies(sun_config_products sun)
        # One target owns the shared command; the per-artifact names other
        # rules depend on all lead to it.
        foreach(product stdlib_moon stdlib_sun_tests tls_moon tls_sun_tests)
            add_custom_target(${product} ALL)
            add_dependencies(${product} sun_config_products)
        endforeach()
    else()
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
                COMMAND $<TARGET_FILE:sun> -c --skip-if-unchanged
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

        # --lib-path points at this build directory, which in this
        # branch need not be <source>/build.
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
                COMMAND $<TARGET_FILE:sun> -c --skip-if-unchanged
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
        if(NOT (SUN_CROSS_TLS_AARCH64 AND cross_triple STREQUAL "aarch64-linux-gnu"))
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
        set(CROSS_BUNDLE_DIR ${CMAKE_SOURCE_DIR}/build/aarch64-linux-gnu)
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
            BYPRODUCTS ${CROSS_BUNDLE_DIR}/stdlib.moon ${CROSS_BUNDLE_DIR}/tls.moon
            COMMENT "Checking AArch64 stdlib and TLS from sun-config.json"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
        add_dependencies(cross_bundles_aarch64 sun)
        # The shared config names source-relative outputs. Package builds
        # collect those artifacts in their separate CMake build directory.
        set(CROSS_PACKAGE_DIR ${CMAKE_BINARY_DIR}/aarch64-linux-gnu)
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
        add_custom_target(tls_cross_moon ALL DEPENDS ${CROSS_PACKAGE_DIR}/tls.moon)
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
