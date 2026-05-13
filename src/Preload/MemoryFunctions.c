#include "TrackAllocations.h"

#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define HEAPVISUALIZER_PRELOAD_EXPORT __attribute__((visibility("default")))

void* __libc_malloc(size_t size) __attribute__((weak));
void* __libc_calloc(size_t count, size_t size) __attribute__((weak));
void* __libc_realloc(void* pointer, size_t size) __attribute__((weak));
void* __libc_memalign(size_t alignment, size_t size) __attribute__((weak));
void __libc_free(void* pointer) __attribute__((weak));

//------------------------------------------------------------------------------
// PageSize
//------------------------------------------------------------------------------
//  Return the operating system page size with a conservative fallback.
//------------------------------------------------------------------------------
static size_t PageSize(void)
{
    const size_t fallbackPageSize = 4096;
    const long pageSize = sysconf(_SC_PAGESIZE);
    return pageSize <= 0 ? fallbackPageSize : (size_t)pageSize;
}


//------------------------------------------------------------------------------
// CallLibcMalloc
//------------------------------------------------------------------------------
//  Forward to the glibc malloc implementation.
//------------------------------------------------------------------------------
static void* CallLibcMalloc(size_t size)
{
    if (__libc_malloc == NULL)
    {
        errno = ENOMEM;
        return NULL;
    }

    return __libc_malloc(size);
}


//------------------------------------------------------------------------------
// CallLibcCalloc
//------------------------------------------------------------------------------
//  Forward to the glibc calloc implementation.
//------------------------------------------------------------------------------
static void* CallLibcCalloc(size_t count, size_t size)
{
    if (__libc_calloc == NULL)
    {
        errno = ENOMEM;
        return NULL;
    }

    return __libc_calloc(count, size);
}


//------------------------------------------------------------------------------
// CallLibcRealloc
//------------------------------------------------------------------------------
//  Forward to the glibc realloc implementation.
//------------------------------------------------------------------------------
static void* CallLibcRealloc(void* pointer, size_t size)
{
    if (__libc_realloc == NULL)
    {
        errno = ENOMEM;
        return NULL;
    }

    return __libc_realloc(pointer, size);
}


//------------------------------------------------------------------------------
// CallLibcFree
//------------------------------------------------------------------------------
//  Forward to the glibc free implementation.
//------------------------------------------------------------------------------
static void CallLibcFree(void* pointer)
{
    if (__libc_free != NULL)
    {
        __libc_free(pointer);
    }
}


//------------------------------------------------------------------------------
// CallLibcMemalign
//------------------------------------------------------------------------------
//  Forward to glibc memalign or fall back to malloc.
//------------------------------------------------------------------------------
static void* CallLibcMemalign(size_t alignment, size_t size)
{
    if (__libc_memalign != NULL)
    {
        return __libc_memalign(alignment, size);
    }

    return CallLibcMalloc(size);
}


//------------------------------------------------------------------------------
// malloc
//------------------------------------------------------------------------------
//  Hook malloc allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* malloc(size_t size)
{
    void* pointer = CallLibcMalloc(size);
    TrackAllocation(pointer, size, TrackedAllocationType_Malloc);
    return pointer;
}


//------------------------------------------------------------------------------
// calloc
//------------------------------------------------------------------------------
//  Hook calloc allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* calloc(size_t count, size_t size)
{
    // Rely on libc calloc() returning NULL if (count * size) would overflow.
    void* pointer = CallLibcCalloc(count, size);
    if (pointer != NULL)
    {
        TrackAllocation(pointer, count * size, TrackedAllocationType_Calloc);
    }

    return pointer;
}


//------------------------------------------------------------------------------
// realloc
//------------------------------------------------------------------------------
//  Hook realloc allocation changes.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* realloc(void* pointer, size_t size)
{
    void* newPointer = CallLibcRealloc(pointer, size);
    if ((newPointer != NULL) || (size == 0))
    {
        UntrackAllocation(pointer, TrackedDeallocationType_Realloc);
        TrackAllocation(newPointer, size, TrackedAllocationType_Realloc);
    }

    return newPointer;
}


//------------------------------------------------------------------------------
// reallocarray
//------------------------------------------------------------------------------
//  Hook reallocarray allocation changes.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* reallocarray(void* pointer, size_t count, size_t size)
{
    if ((size != 0) && (count > (SIZE_MAX / size)))
    {
        errno = ENOMEM;
        return NULL;
    }

    const size_t allocationSize = count * size;
    void* newPointer = CallLibcRealloc(pointer, allocationSize);
    if ((newPointer != NULL) || (allocationSize == 0))
    {
        UntrackAllocation(pointer, TrackedDeallocationType_ReallocArray);
        TrackAllocation(newPointer, allocationSize, TrackedAllocationType_ReallocArray);
    }

    return newPointer;
}


//------------------------------------------------------------------------------
// free
//------------------------------------------------------------------------------
//  Hook deallocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void free(void* pointer)
{
    UntrackAllocation(pointer, TrackedDeallocationType_Free);
    CallLibcFree(pointer);
}


//------------------------------------------------------------------------------
// cfree
//------------------------------------------------------------------------------
//  Hook legacy deallocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void cfree(void* pointer)
{
    UntrackAllocation(pointer, TrackedDeallocationType_CFree);
    CallLibcFree(pointer);
}


//------------------------------------------------------------------------------
// posix_memalign
//------------------------------------------------------------------------------
//  Hook POSIX aligned allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT int posix_memalign(void** memoryPointer, size_t alignment, size_t size)
{
    void* pointer = NULL;

    if ((alignment < sizeof(void*)) ||
        ((alignment % sizeof(void*)) != 0) ||
        ((alignment & (alignment - 1)) != 0))
    {
        return EINVAL;
    }

    pointer = CallLibcMemalign(alignment, size);
    if (pointer == NULL)
    {
        return ENOMEM;
    }

    TrackAllocation(pointer, size, TrackedAllocationType_PosixMemalign);
    *memoryPointer = pointer;
    return 0;
}


//------------------------------------------------------------------------------
// aligned_alloc
//------------------------------------------------------------------------------
//  Hook C11 aligned allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* aligned_alloc(size_t alignment, size_t size)
{
    void* pointer = CallLibcMemalign(alignment, size);
    TrackAllocation(pointer, size, TrackedAllocationType_AlignedAlloc);
    return pointer;
}


//------------------------------------------------------------------------------
// memalign
//------------------------------------------------------------------------------
//  Hook legacy aligned allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* memalign(size_t alignment, size_t size)
{
    void* pointer = CallLibcMemalign(alignment, size);
    TrackAllocation(pointer, size, TrackedAllocationType_Memalign);
    return pointer;
}


//------------------------------------------------------------------------------
// valloc
//------------------------------------------------------------------------------
//  Hook page-aligned allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* valloc(size_t size)
{
    void* pointer = CallLibcMemalign(PageSize(), size);
    TrackAllocation(pointer, size, TrackedAllocationType_Valloc);
    return pointer;
}


//------------------------------------------------------------------------------
// pvalloc
//------------------------------------------------------------------------------
//  Hook rounded page-aligned allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* pvalloc(size_t size)
{
    const size_t pageSize = PageSize();
    size_t allocationSize = SIZE_MAX;
    void* pointer = NULL;

    if (size <= SIZE_MAX - pageSize + 1)
    {
        allocationSize = ((size + pageSize - 1) / pageSize) * pageSize;
    }

    pointer = CallLibcMemalign(pageSize, allocationSize);
    TrackAllocation(pointer, allocationSize, TrackedAllocationType_PValloc);
    return pointer;
}
