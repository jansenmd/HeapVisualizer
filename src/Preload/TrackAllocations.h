#pragma once

#ifdef __cplusplus

#include <cstdint>

namespace HeapVisualizer::AllocationHooks
{

enum class TrackedAllocationType : std::uint32_t
{
    Malloc = 0,
    Calloc,
    Realloc,
    ReallocArray,
    PosixMemalign,
    AlignedAlloc,
    Memalign,
    Valloc,
    PValloc,
    Count
};

enum class TrackedDeallocationType : std::uint32_t
{
    Free = 0,
    Realloc,
    ReallocArray,
    CFree,
    Count
};

}

#else

#include <stddef.h>
#include <stdint.h>

typedef enum TrackedAllocationType
{
    TrackedAllocationType_Malloc = 0,
    TrackedAllocationType_Calloc,
    TrackedAllocationType_Realloc,
    TrackedAllocationType_ReallocArray,
    TrackedAllocationType_PosixMemalign,
    TrackedAllocationType_AlignedAlloc,
    TrackedAllocationType_Memalign,
    TrackedAllocationType_Valloc,
    TrackedAllocationType_PValloc,
    TrackedAllocationType_Count
} TrackedAllocationType;

typedef enum TrackedDeallocationType
{
    TrackedDeallocationType_Free = 0,
    TrackedDeallocationType_Realloc,
    TrackedDeallocationType_ReallocArray,
    TrackedDeallocationType_CFree,
    TrackedDeallocationType_Count
} TrackedDeallocationType;

void TrackAllocation(void* pointer, size_t size, TrackedAllocationType type);
void UntrackAllocation(void* pointer, TrackedDeallocationType type);

#endif
