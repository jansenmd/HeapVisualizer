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

class HookScope
{
public:
    HookScope();
    ~HookScope();
    [[nodiscard]] static bool IsInsideHook();

private:
    static std::atomic_bool mInsideHook;
    bool mPreviousValue = false;
};

std::atomic_bool HookScope::mInsideHook = false;


//------------------------------------------------------------------------------
// HookScope
//------------------------------------------------------------------------------
//  Mark this thread as being inside a hook while forwarding to libc.
//------------------------------------------------------------------------------
HookScope::HookScope()
    : mPreviousValue(mInsideHook.exchange(true, std::memory_order_acq_rel))
{
}


//------------------------------------------------------------------------------
// HookScope
//------------------------------------------------------------------------------
//  Restore the previous hook recursion state.
//------------------------------------------------------------------------------
HookScope::~HookScope()
{
    mInsideHook.store(mPreviousValue, std::memory_order_release);
}


//------------------------------------------------------------------------------
// IsInsideHook
//------------------------------------------------------------------------------
//  Return whether the current hook library is forwarding an allocation.
//------------------------------------------------------------------------------
bool HookScope::IsInsideHook()
{
    return mInsideHook.load(std::memory_order_acquire);
}


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
    void* pointer = NULL;
    if (HookScope::IsInsideHook())
    {
        pointer = CallLibcMalloc(size);
        TrackAllocationInternal(pointer, size);
    }
    else
    {
        HookScope scope;
        pointer = CallLibcMalloc(size);
        TrackAllocationTarget(pointer, size);
    }
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
    void* pointer = NULL;
    if (HookScope::IsInsideHook())
    {
        pointer = CallLibcCalloc(count, size);
        if (pointer != NULL)
        {
            TrackAllocationInternal(pointer, count * size);
        }
    }
    else
    {
        HookScope scope;
        pointer = CallLibcCalloc(count, size);
        if (pointer != NULL)
        {
            TrackAllocationTarget(pointer, count * size);
        }
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
    void* newPointer = NULL;
    if (HookScope::IsInsideHook())
    {
        newPointer = CallLibcRealloc(pointer, size);
        if ((newPointer != nullptr) || (size == 0))
        {
            UntrackAllocation(pointer);
            TrackAllocationInternal(newPointer, size);
        }
    }
    else
    {
        HookScope scope;
        newPointer = CallLibcRealloc(pointer, size);
        if ((newPointer != nullptr) || (size == 0))
        {
            UntrackAllocation(pointer);
            TrackAllocationTarget(newPointer, size);
        }
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
    void* newPointer = NULL;
    if (HookScope::IsInsideHook())
    {
        newPointer = CallLibcRealloc(pointer, allocationSize);
        if ((newPointer != nullptr) || (allocationSize == 0))
        {
            UntrackAllocation(pointer);
            TrackAllocationInternal(newPointer, allocationSize);
        }
    }
    else
    {
        HookScope scope;
        newPointer = CallLibcRealloc(pointer, allocationSize);
        if ((newPointer != nullptr) || (allocationSize == 0))
        {
            UntrackAllocation(pointer);
            TrackAllocationTarget(newPointer, allocationSize);
        }
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
    if (HookScope::IsInsideHook())
    {
        UntrackAllocation(pointer);
        CallLibcFree(pointer);
    }
    else
    {
        HookScope scope;
        UntrackAllocation(pointer);
        CallLibcFree(pointer);
    }
}


//------------------------------------------------------------------------------
// cfree
//------------------------------------------------------------------------------
//  Hook legacy deallocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void cfree(void* pointer)
{
    free(pointer);
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

    void* pointer = NULL;
    if (HookScope::IsInsideHook())
    {
        pointer = CallLibcMemalign(alignment, size);
        if (pointer == nullptr)
        {
            result = ENOMEM;
        }
        else
        {
            TrackAllocationInternal(pointer, size);
            *memoryPointer = pointer;
        }
    }
    else
    {
        HookScope scope;
        pointer = CallLibcMemalign(alignment, size);
        if (pointer == nullptr)
        {
            result = ENOMEM;
        }
        else
        {
            TrackAllocationTarget(pointer, size);
            *memoryPointer = pointer;
        }
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
    void* pointer = NULL;
    if (HookScope::IsInsideHook())
    {
        pointer = CallLibcMemalign(alignment, size);
        TrackAllocationInternal(pointer, size);
    }
    else
    {
        HookScope scope;
        pointer = CallLibcMemalign(alignment, size);
        TrackAllocationTarget(pointer, size);
    }
    return pointer;
}


//------------------------------------------------------------------------------
// memalign
//------------------------------------------------------------------------------
//  Hook legacy aligned allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* memalign(std::size_t alignment, std::size_t size)
{
    void* pointer = NULL;
    if (HookScope::IsInsideHook())
    {
        pointer = CallLibcMemalign(alignment, size);
        TrackAllocationInternal(pointer, size);
    }
    else
    {
        HookScope scope;
        pointer = CallLibcMemalign(alignment, size);
        TrackAllocationTarget(pointer, size);
    }
    return pointer;
}


//------------------------------------------------------------------------------
// valloc
//------------------------------------------------------------------------------
//  Hook page-aligned allocations.
//------------------------------------------------------------------------------
HEAPVISUALIZER_PRELOAD_EXPORT void* valloc(std::size_t size)
{
    void* pointer = NULL;
    if (HookScope::IsInsideHook())
    {
        pointer = CallLibcMemalign(PageSize(), size);
        TrackAllocationInternal(pointer, size);
    }
    else
    {
        HookScope scope;
        pointer = CallLibcMemalign(PageSize(), size);
        TrackAllocationTarget(pointer, size);
    }
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

    void* pointer = NULL;
    if (HookScope::IsInsideHook())
    {
        pointer = CallLibcMemalign(pageSize, allocationSize);
        TrackAllocationInternal(pointer, allocationSize);
    }
    else
    {
        HookScope scope;
        pointer = CallLibcMemalign(pageSize, allocationSize);
        TrackAllocationTarget(pointer, allocationSize);
    }
    return pointer;
}

} // extern "C"
