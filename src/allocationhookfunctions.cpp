#include "allocationhook_support.hpp"

#include <cerrno>
#include <cstddef>

using namespace HeapVisualizer::AllocationHooks;

extern "C"
{

//------------------------------------------------------------------------------
// malloc
//------------------------------------------------------------------------------
//  Hook malloc allocations.
//------------------------------------------------------------------------------
void* malloc(std::size_t size)
{
    if (IsInsideHook())
    {
        void* pointer = CallLibcMalloc(size);
        TrackAllocation(pointer, size, true);
        return pointer;
    }

    HookScope scope;
    void* pointer = CallLibcMalloc(size);
    TrackAllocation(pointer, size, false);
    RecordMalloc();
    return pointer;
}


//------------------------------------------------------------------------------
// calloc
//------------------------------------------------------------------------------
//  Hook calloc allocations.
//------------------------------------------------------------------------------
void* calloc(std::size_t count, std::size_t size)
{
    const std::size_t allocationSize = AllocationSizeProduct(count, size);

    if (IsInsideHook())
    {
        void* pointer = CallLibcCalloc(count, size);
        TrackAllocation(pointer, allocationSize, true);
        return pointer;
    }

    HookScope scope;
    void* pointer = CallLibcCalloc(count, size);
    TrackAllocation(pointer, allocationSize, false);
    RecordCalloc();
    return pointer;
}


//------------------------------------------------------------------------------
// realloc
//------------------------------------------------------------------------------
//  Hook realloc allocation changes.
//------------------------------------------------------------------------------
void* realloc(void* pointer, std::size_t size)
{
    if (IsInsideHook())
    {
        void* newPointer = CallLibcRealloc(pointer, size);
        if (newPointer != nullptr || size == 0)
        {
            UntrackAllocation(pointer);
            TrackAllocation(newPointer, size, true);
        }

        return newPointer;
    }

    HookScope scope;
    void* newPointer = CallLibcRealloc(pointer, size);
    if (newPointer != nullptr || size == 0)
    {
        UntrackAllocation(pointer);
        TrackAllocation(newPointer, size, false);
    }

    RecordRealloc();
    return newPointer;
}


//------------------------------------------------------------------------------
// reallocarray
//------------------------------------------------------------------------------
//  Hook reallocarray allocation changes.
//------------------------------------------------------------------------------
void* reallocarray(void* pointer, std::size_t count, std::size_t size)
{
    if (HasMultiplicationOverflow(count, size))
    {
        errno = ENOMEM;
        return nullptr;
    }

    const std::size_t allocationSize = count * size;
    if (IsInsideHook())
    {
        void* newPointer = CallLibcRealloc(pointer, allocationSize);
        if (newPointer != nullptr || allocationSize == 0)
        {
            UntrackAllocation(pointer);
            TrackAllocation(newPointer, allocationSize, true);
        }

        return newPointer;
    }

    HookScope scope;
    void* newPointer = CallLibcRealloc(pointer, allocationSize);
    if (newPointer != nullptr || allocationSize == 0)
    {
        UntrackAllocation(pointer);
        TrackAllocation(newPointer, allocationSize, false);
    }

    RecordReallocArray();
    return newPointer;
}


//------------------------------------------------------------------------------
// free
//------------------------------------------------------------------------------
//  Hook deallocations.
//------------------------------------------------------------------------------
void free(void* pointer)
{
    if (IsInsideHook())
    {
        UntrackAllocation(pointer);
        CallLibcFree(pointer);
        return;
    }

    HookScope scope;
    UntrackAllocation(pointer);
    CallLibcFree(pointer);
    RecordFree();
}


//------------------------------------------------------------------------------
// cfree
//------------------------------------------------------------------------------
//  Hook legacy deallocations.
//------------------------------------------------------------------------------
void cfree(void* pointer)
{
    free(pointer);
}


//------------------------------------------------------------------------------
// posix_memalign
//------------------------------------------------------------------------------
//  Hook POSIX aligned allocations.
//------------------------------------------------------------------------------
int posix_memalign(void** memoryPointer, std::size_t alignment, std::size_t size)
{
    if (!IsValidAlignment(alignment))
    {
        return EINVAL;
    }

    if (IsInsideHook())
    {
        void* pointer = CallLibcMemalign(alignment, size);
        if (pointer == nullptr)
        {
            return ENOMEM;
        }

        TrackAllocation(pointer, size, true);
        *memoryPointer = pointer;
        return 0;
    }

    HookScope scope;
    void* pointer = CallLibcMemalign(alignment, size);
    if (pointer == nullptr)
    {
        return ENOMEM;
    }

    TrackAllocation(pointer, size, false);
    *memoryPointer = pointer;
    RecordPosixMemalign();
    return 0;
}


//------------------------------------------------------------------------------
// aligned_alloc
//------------------------------------------------------------------------------
//  Hook C11 aligned allocations.
//------------------------------------------------------------------------------
void* aligned_alloc(std::size_t alignment, std::size_t size)
{
    if (IsInsideHook())
    {
        void* pointer = CallLibcMemalign(alignment, size);
        TrackAllocation(pointer, size, true);
        return pointer;
    }

    HookScope scope;
    void* pointer = CallLibcMemalign(alignment, size);
    TrackAllocation(pointer, size, false);
    RecordAlignedAlloc();
    return pointer;
}


//------------------------------------------------------------------------------
// memalign
//------------------------------------------------------------------------------
//  Hook legacy aligned allocations.
//------------------------------------------------------------------------------
void* memalign(std::size_t alignment, std::size_t size)
{
    if (IsInsideHook())
    {
        void* pointer = CallLibcMemalign(alignment, size);
        TrackAllocation(pointer, size, true);
        return pointer;
    }

    HookScope scope;
    void* pointer = CallLibcMemalign(alignment, size);
    TrackAllocation(pointer, size, false);
    RecordMemalign();
    return pointer;
}


//------------------------------------------------------------------------------
// valloc
//------------------------------------------------------------------------------
//  Hook page-aligned allocations.
//------------------------------------------------------------------------------
void* valloc(std::size_t size)
{
    if (IsInsideHook())
    {
        void* pointer = CallLibcMemalign(PageSize(), size);
        TrackAllocation(pointer, size, true);
        return pointer;
    }

    HookScope scope;
    void* pointer = CallLibcMemalign(PageSize(), size);
    TrackAllocation(pointer, size, false);
    RecordValloc();
    return pointer;
}


//------------------------------------------------------------------------------
// pvalloc
//------------------------------------------------------------------------------
//  Hook rounded page-aligned allocations.
//------------------------------------------------------------------------------
void* pvalloc(std::size_t size)
{
    const std::size_t allocationSize = RoundUpToPageSize(size);

    if (IsInsideHook())
    {
        void* pointer = CallLibcMemalign(PageSize(), allocationSize);
        TrackAllocation(pointer, allocationSize, true);
        return pointer;
    }

    HookScope scope;
    void* pointer = CallLibcMemalign(PageSize(), allocationSize);
    TrackAllocation(pointer, allocationSize, false);
    RecordPvalloc();
    return pointer;
}

} // extern "C"
