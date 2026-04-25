find_package(GTest REQUIRED)

add_executable(mem_allocator_tests
    test_per_thread_allocator.cpp
)

target_link_libraries(mem_allocator_tests
    PRIVATE
        mem_allocator::pmr
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(mem_allocator_tests)

# ---------------------------------------------------------------------------
# Performance benchmark binary — no sanitizers so timing is not skewed.
# Link only against GTest; malloc/free/realloc are resolved at runtime,
# so injecting the preload library swaps in the custom allocator.
# ---------------------------------------------------------------------------
add_executable(mem_allocator_perf_tests
    test_allocator_perf.cpp
)

target_include_directories(mem_allocator_perf_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/pmr_allocators
)

target_link_libraries(mem_allocator_perf_tests
    PRIVATE
        mem_allocator::pmr
        GTest::gtest_main
)

# Use custom test registration since gtest_discover_tests would try to run
# the binary during build, but with pthread_key_t we need the test runner itself
add_test(
    NAME mem_allocator_perf_tests
    COMMAND mem_allocator_perf_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
