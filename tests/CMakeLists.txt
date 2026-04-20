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
