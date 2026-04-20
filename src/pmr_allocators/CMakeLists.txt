add_library(mem_allocator_pmr STATIC per_thread_allocator.cpp)
add_library(mem_allocator::pmr ALIAS mem_allocator_pmr)

target_include_directories(mem_allocator_pmr
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(mem_allocator_pmr PRIVATE "-fPIC")
