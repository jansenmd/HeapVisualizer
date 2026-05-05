#pragma once

#ifdef __cplusplus

#include <cstdint>

namespace HeapVisualizer::AllocationHooks
{

enum class AllocationType : std::uint32_t
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

}

#else

#include <stddef.h>
#include <stdint.h>

typedef enum AllocationType
{
    AllocationType_Malloc = 0,
    AllocationType_Calloc,
    AllocationType_Realloc,
    AllocationType_ReallocArray,
    AllocationType_Free,
    AllocationType_CFree,
    AllocationType_PosixMemalign,
    AllocationType_AlignedAlloc,
    AllocationType_Memalign,
    AllocationType_Valloc,
    AllocationType_PValloc,
    AllocationType_Count
} AllocationType;

void TrackAllocation(void* pointer, size_t size, AllocationType type);
void UntrackAllocation(void* pointer, AllocationType type);

#endif
