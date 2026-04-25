cmake_minimum_required(VERSION 3.23)

project(mem_allocator
    VERSION 0.1.0
    DESCRIPTION "Learning PMR memory allocation in C++20"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# LLVM XRay instrumentation for performance profiling
option(MEM_ALLOCATOR_ENABLE_XRAY "Enable LLVM XRay instrumentation" OFF)

if(MEM_ALLOCATOR_ENABLE_XRAY)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "XRay instrumentation requires Clang compiler")
    endif()

    add_compile_options(-fxray-instrument)
    add_compile_options(-fxray-instruction-threshold=10)  # Instrument functions with ≥10 instructions
    add_link_options(-fxray-instrument)

    message(STATUS "LLVM XRay instrumentation enabled (basic mode)")
endif()

add_subdirectory(src/pmr_allocators)
add_subdirectory(src/preload)

option(MEM_ALLOCATOR_BUILD_TESTS "Build tests" ON)

if(MEM_ALLOCATOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
