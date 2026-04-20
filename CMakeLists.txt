cmake_minimum_required(VERSION 3.23)

project(mem_allocator
    VERSION 0.1.0
    DESCRIPTION "Learning PMR memory allocation in C++20"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(src/pmr_allocators)
add_subdirectory(src/preload)

option(MEM_ALLOCATOR_BUILD_TESTS "Build tests" ON)

if(MEM_ALLOCATOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
