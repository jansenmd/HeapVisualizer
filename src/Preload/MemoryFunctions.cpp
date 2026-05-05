#include "TrackAllocations.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <malloc.h>
#include <unistd.h>

using namespace HeapVisualizer::AllocationHooks;

#ifdef _WIN32
#define HEAPVISUALIZER_PRELOAD_EXPORT __declspec(dllexport)
#else
#define HEAPVISUALIZER_PRELOAD_EXPORT __attribute__((visibility("default")))
#endif

// These are glibc's C allocator entry points. Keep C linkage here so the C++
// compiler does not mangle the symbol names and the preload library can call
// the real libc allocator functions without resolving through its own hooks.
extern "C"
{
void* __libc_malloc(std::size_t size) __attribute__((weak));
void* __libc_calloc(std::size_t count, std::size_t size) __attribute__((weak));
void* __libc_realloc(void* pointer, std::size_t size) __attribute__((weak));
void* __libc_memalign(std::size_t alignment, std::size_t size) __attribute__((weak));
void __libc_free(void* pointer) __attribute__((weak));
}

namespace
{

//------------------------------------------------------------------------------
// PageSize
//------------------------------------------------------------------------------
//  Return the operating system page size with a conservative fallback.
//------------------------------------------------------------------------------
std::size_t PageSize()
{
    constexpr std::size_t fallbackPageSize = 4096;
    const long pageSize = sysconf(_SC_PAGESIZE);
    return pageSize <= 0 ? fallbackPageSize : static_cast<std::size_t>(pageSize);
}


//------------------------------------------------------------------------------
// CallLibcMalloc
//------------------------------------------------------------------------------
//  Forward to the glibc malloc implementation.
//------------------------------------------------------------------------------
void* CallLibcMalloc(std::size_t size)
{
    if (__libc_malloc == nullptr)
    {
        errno = ENOMEM;
        return nullptr;
    }
    return __libc_malloc(size);
}


//------------------------------------------------------------------------------
// CallLibcCalloc
//------------------------------------------------------------------------------
//  Forward to the glibc calloc implementation.
//------------------------------------------------------------------------------
void* CallLibcCalloc(std::size_t count, std::size_t size)
{
    if (__libc_calloc == nullptr)
    {
        errno = ENOMEM;
        return nullptr;
    }
    return __libc_calloc(count, size);
}


//------------------------------------------------------------------------------
// CallLibcRealloc
//------------------------------------------------------------------------------
//  Forward to the glibc realloc implementation.
//------------------------------------------------------------------------------
void* CallLibcRealloc(void* pointer, std::size_t size)
{
    if (__libc_realloc == nullptr)
    {
        errno = ENOMEM;
        return nullptr;
    }
    return __libc_realloc(pointer, size);
}


//------------------------------------------------------------------------------
// CallLibcFree
//------------------------------------------------------------------------------
//  Forward to the glibc free implementation.
//------------------------------------------------------------------------------
void CallLibcFree(void* pointer)
{
    if (__libc_free != nullptr)
    {
        __libc_free(pointer);
    }
}


//------------------------------------------------------------------------------
// CallLibcMemalign
//------------------------------------------------------------------------------
//  Forward to glibc memalign or fall back to malloc.
//------------------------------------------------------------------------------
void* CallLibcMemalign(std::size_t alignment, std::size_t size)
{
    if (__libc_memalign != nullptr)
    {
        return __libc_memalign(alignment, size);
    }
    return CallLibcMalloc(size);
}

}

extern "C"
{

//------------------------------------------------------------------------------
// malloc
//------------------------------------------------------------------------------
//  Hook malloc allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* malloc(std::size_t size)
{
    void* pointer = CallLibcMalloc(size);
    TrackAllocation(pointer, size, AllocationOperationType::Malloc);
    return pointer;
}


//------------------------------------------------------------------------------
// calloc
//------------------------------------------------------------------------------
//  Hook calloc allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* calloc(std::size_t count, std::size_t size)
{
    // Rely on libc calloc() returning NULL if (count * size) would overflow.
    void* pointer = CallLibcCalloc(count, size);
    if (pointer != NULL)
    {
        TrackAllocation(pointer, count * size, AllocationOperationType::Calloc);
    }
    return pointer;
}


//------------------------------------------------------------------------------
// realloc
//------------------------------------------------------------------------------
//  Hook realloc allocation changes.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* realloc(void* pointer, std::size_t size)
{
    void* newPointer = CallLibcRealloc(pointer, size);
    if ((newPointer != nullptr) || (size == 0))
    {
        UntrackAllocation(pointer, AllocationOperationType::Realloc);
        TrackAllocation(newPointer, size, AllocationOperationType::Realloc);
    }
    return newPointer;
}


//------------------------------------------------------------------------------
// reallocarray
//------------------------------------------------------------------------------
//  Hook reallocarray allocation changes.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* reallocarray(void* pointer, std::size_t count, std::size_t size)
{
    if ((size != 0) && (count > (std::numeric_limits<std::size_t>::max() / size)))
    {
        errno = ENOMEM;
        return nullptr;
    }

    const std::size_t allocationSize = count * size;
    void* newPointer = CallLibcRealloc(pointer, allocationSize);
    if ((newPointer != nullptr) || (allocationSize == 0))
    {
        UntrackAllocation(pointer, AllocationOperationType::ReallocArray);
        TrackAllocation(newPointer, allocationSize, AllocationOperationType::ReallocArray);
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
    UntrackAllocation(pointer, AllocationOperationType::Free);
    CallLibcFree(pointer);
}


//------------------------------------------------------------------------------
// cfree
//------------------------------------------------------------------------------
//  Hook legacy deallocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void cfree(void* pointer)
{
    UntrackAllocation(pointer, AllocationOperationType::CFree);
    CallLibcFree(pointer);
}


//------------------------------------------------------------------------------
// posix_memalign
//------------------------------------------------------------------------------
//  Hook POSIX aligned allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT int posix_memalign(void** memoryPointer, std::size_t alignment, std::size_t size)
{
    int result = 0;
    if ((alignment < sizeof(void*)) ||
        ((alignment % sizeof(void*)) != 0) ||
        ((alignment & (alignment - 1)) != 0))
    {
        return EINVAL;
    }

    void* pointer = CallLibcMemalign(alignment, size);
    if (pointer == nullptr)
    {
        result = ENOMEM;
    }
    else
    {
        TrackAllocation(pointer, size, AllocationOperationType::PosixMemalign);
        *memoryPointer = pointer;
    }
    return result;
}


//------------------------------------------------------------------------------
// aligned_alloc
//------------------------------------------------------------------------------
//  Hook C11 aligned allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* aligned_alloc(std::size_t alignment, std::size_t size)
{
    void* pointer = CallLibcMemalign(alignment, size);
    TrackAllocation(pointer, size, AllocationOperationType::AlignedAlloc);
    return pointer;
}


//------------------------------------------------------------------------------
// memalign
//------------------------------------------------------------------------------
//  Hook legacy aligned allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* memalign(std::size_t alignment, std::size_t size)
{
    void* pointer = CallLibcMemalign(alignment, size);
    TrackAllocation(pointer, size, AllocationOperationType::Memalign);
    return pointer;
}


//------------------------------------------------------------------------------
// valloc
//------------------------------------------------------------------------------
//  Hook page-aligned allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* valloc(std::size_t size)
{
    void* pointer = CallLibcMemalign(PageSize(), size);
    TrackAllocation(pointer, size, AllocationOperationType::Valloc);
    return pointer;
}


//------------------------------------------------------------------------------
// pvalloc
//------------------------------------------------------------------------------
//  Hook rounded page-aligned allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* pvalloc(std::size_t size)
{
    const std::size_t pageSize = PageSize();
    std::size_t allocationSize = std::numeric_limits<std::size_t>::max();
    if (size <= std::numeric_limits<std::size_t>::max() - pageSize + 1)
    {
        allocationSize = ((size + pageSize - 1) / pageSize) * pageSize;
    }

    void* pointer = CallLibcMemalign(pageSize, allocationSize);
    TrackAllocation(pointer, allocationSize, AllocationOperationType::PValloc);
    return pointer;
}

} // extern "C"
