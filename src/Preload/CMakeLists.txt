set(allocation_hook_sources
    MemoryFunctions.c
    TrackAllocations.c
)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

enable_language(C)
find_package(Threads REQUIRED)

add_library(HeapAllocationHooks SHARED ${allocation_hook_sources})
set_target_properties(HeapAllocationHooks
    PROPERTIES
        AUTOMOC OFF
        AUTORCC OFF
        AUTOUIC OFF
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        C_VISIBILITY_PRESET hidden
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        OUTPUT_NAME "heap_allocation_hooks"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_compile_definitions(HeapAllocationHooks PRIVATE _GNU_SOURCE)

target_compile_options(HeapAllocationHooks
    PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

target_link_libraries(HeapAllocationHooks PRIVATE Threads::Threads)

target_link_options(HeapAllocationHooks
    PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wl,-z,now>
        $<$<C_COMPILER_ID:GNU,Clang>:-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map>
)
