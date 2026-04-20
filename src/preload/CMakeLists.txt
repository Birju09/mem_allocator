add_library(mem_allocator_preload SHARED
    preload.cpp
)
add_library(mem_allocator::preload ALIAS mem_allocator_preload)

target_link_libraries(mem_allocator_preload
    PRIVATE
        mem_allocator::pmr
)

target_compile_options(mem_allocator_preload PRIVATE "-fPIC")


# Platform-specific linker flags needed for symbol interposition
if(APPLE)
    target_link_options(mem_allocator_preload PRIVATE
        -undefined dynamic_lookup
    )
elseif(UNIX)
    target_link_libraries(mem_allocator_preload PRIVATE dl)
endif()
