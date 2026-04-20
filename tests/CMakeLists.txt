find_package(GTest REQUIRED)

add_executable(mem_allocator_tests
    test_per_thread_allocator.cpp
)

target_link_libraries(mem_allocator_tests
    PRIVATE
        mem_allocator::pmr
        GTest::gtest_main
)

target_compile_options(mem_allocator_tests PRIVATE "-fsanitize=address,undefined")
target_link_options(mem_allocator_tests PRIVATE "-fsanitize=address,undefined")

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

target_link_libraries(mem_allocator_perf_tests
    PRIVATE
        GTest::gtest_main
)

gtest_discover_tests(mem_allocator_perf_tests)
