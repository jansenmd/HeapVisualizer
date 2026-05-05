#pragma once

#include <cstddef>
#include <cstdint>

namespace HeapVisualizer::AllocationHooks
{

enum class AllocationOperationType : std::uint32_t
{
    Malloc = 0,
    Calloc,
    Realloc,
    ReallocArray,
    Free,
    CFree,
    PosixMemalign,
    AlignedAlloc,
    Memalign,
    Valloc,
    PValloc,
    Count
};

void TrackAllocation(void* pointer, std::size_t size, AllocationOperationType type);
void UntrackAllocation(void* pointer, AllocationOperationType type);

}
