# Optional NVIDIA support keeps CUDA out of ordinary compiler and stdlib builds.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|aarch64|arm64)$" OR
   CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "SUN_ENABLE_CUDA requires a native Linux x86-64 or ARM64 build")
endif()
include(CheckCSourceCompiles)
check_c_source_compiles("#include <features.h>\n#ifndef __GLIBC__\n#error glibc required\n#endif\nint main(void) { return 0; }" SUN_CUDA_HAS_GLIBC)
if(NOT SUN_CUDA_HAS_GLIBC OR SKIP_STDLIB)
    message(FATAL_ERROR "SUN_ENABLE_CUDA requires glibc and the standard library")
endif()
find_package(CUDAToolkit 12 REQUIRED)
find_package(Threads REQUIRED)
add_library(sun_cuda_native STATIC native.c)
set_target_properties(sun_cuda_native PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_STANDARD 11
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(sun_cuda_native PRIVATE CUDA::cudart CUDA::cublas Threads::Threads)
target_compile_options(sun_cuda_native PRIVATE -Wall -Wextra -Werror)
add_custom_target(cuda_moon ALL
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/build-moon.sh" "$<TARGET_FILE:sun>"
            "${CMAKE_CURRENT_SOURCE_DIR}/cuda.sun" "${CMAKE_BINARY_DIR}/cuda.moon"
            --path-var "SUN_CUDA_NATIVE=${CMAKE_CURRENT_BINARY_DIR}"
            --path-var "SUN_CUDA_STDLIB=${CMAKE_BINARY_DIR}"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/cuda.moon"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VERBATIM)
add_dependencies(cuda_moon sun stdlib_moon sun_cuda_native)

# The root config may also build CUDA when its entrypoint is enabled.
if(TARGET sun_config_products)
    add_dependencies(sun_config_products sun_cuda_native)
endif()

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(cuda_native_fault_checks native_fault_checks.c)
    target_include_directories(cuda_native_fault_checks PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
    target_link_libraries(cuda_native_fault_checks PRIVATE Threads::Threads)
    set_target_properties(cuda_native_fault_checks PROPERTIES C_STANDARD 11)
    target_compile_options(cuda_native_fault_checks PRIVATE -Wall -Wextra -Werror)
    add_test(NAME cuda_native_faults COMMAND cuda_native_fault_checks)
    set_tests_properties(cuda_native_faults PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_library(sun_cuda_fake SHARED native_fake.c)
    target_include_directories(sun_cuda_fake PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
    set_target_properties(sun_cuda_fake PROPERTIES C_STANDARD 11)
    add_test(NAME cuda_simulated
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_gpu.py"
          --sun "$<TARGET_FILE:sun>" --bundle-dir "${CMAKE_BINARY_DIR}"
          --cuda-lib "$<TARGET_FILE_DIR:sun_cuda_fake>"
          --backend-library sun_cuda_fake --require-gpu ON)
    set_tests_properties(cuda_simulated PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 180)
    add_executable(cuda_native_checks native_checks.c)
    target_link_libraries(cuda_native_checks PRIVATE sun_cuda_native CUDA::cudart CUDA::cublas Threads::Threads)
    add_test(NAME cuda_validation COMMAND cuda_native_checks)
    set_tests_properties(cuda_validation PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    option(SUN_CUDA_REQUIRE_GPU "Fail CUDA hardware tests when no usable GPU is present" OFF)
    add_test(NAME cuda_gpu
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_gpu.py"
          --sun "$<TARGET_FILE:sun>" --bundle-dir "${CMAKE_BINARY_DIR}"
          --cuda-lib "$<TARGET_FILE_DIR:CUDA::cublas>"
          --require-gpu "${SUN_CUDA_REQUIRE_GPU}")
    set_tests_properties(cuda_gpu PROPERTIES LABELS "gpu;nvidia" SKIP_RETURN_CODE 77
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 180)
endif()
