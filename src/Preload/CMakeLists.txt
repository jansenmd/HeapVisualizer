set(allocation_hook_sources
    MemoryFunctions.cpp
    TrackAllocations.cpp
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)

    add_library(HeapAllocationHooks SHARED ${allocation_hook_sources})
    set_target_properties(HeapAllocationHooks
        PROPERTIES
            AUTOMOC OFF
            AUTORCC OFF
            AUTOUIC OFF
            CXX_VISIBILITY_PRESET hidden
            OUTPUT_NAME "heap_allocation_hooks"
            VISIBILITY_INLINES_HIDDEN YES
    )

    target_compile_options(HeapAllocationHooks
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
            $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )

    target_link_libraries(HeapAllocationHooks PRIVATE Threads::Threads)

    target_link_options(HeapAllocationHooks
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wl,-z,now>
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map>
    )
endif()
