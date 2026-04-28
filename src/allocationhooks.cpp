#include "allocationhook_support.hpp"

#include <malloc.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>

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

std::atomic_bool gInsideHook = false;

std::atomic<unsigned long long> gMallocCount = 0;
std::atomic<unsigned long long> gCallocCount = 0;
std::atomic<unsigned long long> gReallocCount = 0;
std::atomic<unsigned long long> gReallocArrayCount = 0;
std::atomic<unsigned long long> gFreeCount = 0;
std::atomic<unsigned long long> gPosixMemalignCount = 0;
std::atomic<unsigned long long> gAlignedAllocCount = 0;
std::atomic<unsigned long long> gMemalignCount = 0;
std::atomic<unsigned long long> gVallocCount = 0;
std::atomic<unsigned long long> gPvallocCount = 0;

struct AllocationRecord
{
    void* mAddress = nullptr;
    std::size_t mSize = 0;
};

struct AllocationPool
{
    std::atomic_flag mLock = ATOMIC_FLAG_INIT;
    AllocationRecord* mRecords = nullptr;
    std::size_t mCapacity = 0;
    std::size_t mActiveCount = 0;
    std::size_t mActiveBytes = 0;
    unsigned long long mTrackedCount = 0;
    unsigned long long mUntrackedCount = 0;
    unsigned long long mUnknownFreeCount = 0;
};

constexpr std::size_t MainAllocationCapacity = 262144;
constexpr std::size_t InternalAllocationCapacity = 4096;
void* const TombstoneAddress = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));

AllocationRecord gMainAllocationRecords[MainAllocationCapacity] = {};
AllocationRecord gInternalAllocationRecords[InternalAllocationCapacity] = {};
AllocationPool gMainAllocationPool = {
    ATOMIC_FLAG_INIT,
    gMainAllocationRecords,
    MainAllocationCapacity,
    0,
    0,
    0,
    0,
    0};
AllocationPool gInternalAllocationPool = {
    ATOMIC_FLAG_INIT,
    gInternalAllocationRecords,
    InternalAllocationCapacity,
    0,
    0,
    0,
    0,
    0};

}


namespace HeapVisualizer::AllocationHooks
{

//------------------------------------------------------------------------------
// HookScope
//------------------------------------------------------------------------------
//  Mark this thread as being inside a hook while forwarding to libc.
//------------------------------------------------------------------------------
HookScope::HookScope()
    : mPreviousValue(gInsideHook.exchange(true, std::memory_order_acq_rel))
{
}


//------------------------------------------------------------------------------
// HookScope
//------------------------------------------------------------------------------
//  Restore the previous hook recursion state.
//------------------------------------------------------------------------------
HookScope::~HookScope()
{
    gInsideHook.store(mPreviousValue, std::memory_order_release);
}


//------------------------------------------------------------------------------
// IsInsideHook
//------------------------------------------------------------------------------
//  Return whether the current hook library is forwarding an allocation.
//------------------------------------------------------------------------------
bool IsInsideHook()
{
    return gInsideHook.load(std::memory_order_acquire);
}


namespace
{


struct PoolSnapshot
{
    std::size_t mActiveCount = 0;
    std::size_t mActiveBytes = 0;
    unsigned long long mTrackedCount = 0;
    unsigned long long mUntrackedCount = 0;
    unsigned long long mUnknownFreeCount = 0;
};


//------------------------------------------------------------------------------
// WriteText
//------------------------------------------------------------------------------
//  Write a null-terminated message without heap-allocating I/O helpers.
//------------------------------------------------------------------------------
void WriteText(const char* message)
{
    if (message == nullptr)
    {
        return;
    }

    const std::size_t messageLength = std::strlen(message);
    if (messageLength == 0)
    {
        return;
    }

    const ssize_t ignoredResult = write(STDERR_FILENO, message, messageLength);
    static_cast<void>(ignoredResult);
}


//------------------------------------------------------------------------------
// HashPointer
//------------------------------------------------------------------------------
//  Convert an allocation address into a stable tracking-table index.
//------------------------------------------------------------------------------
std::size_t HashPointer(const void* pointer, std::size_t capacity)
{
    const auto pointerValue = reinterpret_cast<std::uintptr_t>(pointer);
    const std::size_t mixedValue = static_cast<std::size_t>(pointerValue >> 4U) ^
                                   static_cast<std::size_t>(pointerValue >> 32U);
    return capacity == 0 ? 0 : mixedValue % capacity;
}


//------------------------------------------------------------------------------
// LockPool
//------------------------------------------------------------------------------
//  Acquire a tracking pool lock without allocating memory.
//------------------------------------------------------------------------------
void LockPool(AllocationPool& pool)
{
    while (pool.mLock.test_and_set(std::memory_order_acquire))
    {
    }
}


//------------------------------------------------------------------------------
// UnlockPool
//------------------------------------------------------------------------------
//  Release a tracking pool lock.
//------------------------------------------------------------------------------
void UnlockPool(AllocationPool& pool)
{
    pool.mLock.clear(std::memory_order_release);
}


//------------------------------------------------------------------------------
// TrackAllocationInPool
//------------------------------------------------------------------------------
//  Track or update a single allocation in a fixed-size tracking pool.
//------------------------------------------------------------------------------
void TrackAllocationInPool(AllocationPool& pool, void* pointer, std::size_t size)
{
    if (pointer == nullptr || pool.mRecords == nullptr || pool.mCapacity == 0)
    {
        return;
    }

    LockPool(pool);

    const std::size_t startIndex = HashPointer(pointer, pool.mCapacity);
    std::size_t firstEmptyIndex = pool.mCapacity;

    for (std::size_t offset = 0; offset < pool.mCapacity; ++offset)
    {
        const std::size_t recordIndex = (startIndex + offset) % pool.mCapacity;
        AllocationRecord& record = pool.mRecords[recordIndex];

        if (record.mAddress == pointer)
        {
            pool.mActiveBytes -= record.mSize;
            record.mSize = size;
            pool.mActiveBytes += size;
            UnlockPool(pool);
            return;
        }

        if (record.mAddress == nullptr || record.mAddress == TombstoneAddress)
        {
            if (firstEmptyIndex == pool.mCapacity)
            {
                firstEmptyIndex = recordIndex;
            }

            if (record.mAddress == nullptr)
            {
                break;
            }
        }
    }

    if (firstEmptyIndex == pool.mCapacity)
    {
        ++pool.mUntrackedCount;
        UnlockPool(pool);
        return;
    }

    AllocationRecord& record = pool.mRecords[firstEmptyIndex];
    record.mAddress = pointer;
    record.mSize = size;
    ++pool.mActiveCount;
    ++pool.mTrackedCount;
    pool.mActiveBytes += size;

    UnlockPool(pool);
}


//------------------------------------------------------------------------------
// UntrackAllocationInPool
//------------------------------------------------------------------------------
//  Remove a single allocation from a fixed-size tracking pool.
//------------------------------------------------------------------------------
bool UntrackAllocationInPool(AllocationPool& pool, void* pointer, bool countUnknownFree)
{
    if (pointer == nullptr || pool.mRecords == nullptr || pool.mCapacity == 0)
    {
        return false;
    }

    LockPool(pool);

    const std::size_t startIndex = HashPointer(pointer, pool.mCapacity);
    for (std::size_t offset = 0; offset < pool.mCapacity; ++offset)
    {
        const std::size_t recordIndex = (startIndex + offset) % pool.mCapacity;
        AllocationRecord& record = pool.mRecords[recordIndex];

        if (record.mAddress == pointer)
        {
            pool.mActiveBytes -= record.mSize;
            --pool.mActiveCount;
            record.mAddress = TombstoneAddress;
            record.mSize = 0;
            UnlockPool(pool);
            return true;
        }

        if (record.mAddress == nullptr)
        {
            break;
        }
    }

    if (countUnknownFree)
    {
        ++pool.mUnknownFreeCount;
    }

    UnlockPool(pool);
    return false;
}


//------------------------------------------------------------------------------
// TrackAllocation
//------------------------------------------------------------------------------
//  Track an allocation in either the main or internal allocation pool.
//------------------------------------------------------------------------------
}


//------------------------------------------------------------------------------
// TrackAllocation
//------------------------------------------------------------------------------
//  Track an allocation in either the main or internal allocation pool.
//------------------------------------------------------------------------------
void TrackAllocation(void* pointer, std::size_t size, bool internalAllocation)
{
    TrackAllocationInPool(internalAllocation ? gInternalAllocationPool : gMainAllocationPool, pointer, size);
}


//------------------------------------------------------------------------------
// UntrackAllocation
//------------------------------------------------------------------------------
//  Remove an allocation from whichever pool owns it.
//------------------------------------------------------------------------------
void UntrackAllocation(void* pointer)
{
    if (pointer == nullptr)
    {
        return;
    }

    if (UntrackAllocationInPool(gMainAllocationPool, pointer, false))
    {
        return;
    }

    if (UntrackAllocationInPool(gInternalAllocationPool, pointer, false))
    {
        return;
    }

    UntrackAllocationInPool(gMainAllocationPool, pointer, true);
}


//------------------------------------------------------------------------------
// SnapshotPool
//------------------------------------------------------------------------------
//  Copy pool counters while holding the pool lock briefly.
//------------------------------------------------------------------------------
PoolSnapshot SnapshotPool(AllocationPool& pool)
{
    LockPool(pool);
    PoolSnapshot snapshot;
    snapshot.mActiveCount = pool.mActiveCount;
    snapshot.mActiveBytes = pool.mActiveBytes;
    snapshot.mTrackedCount = pool.mTrackedCount;
    snapshot.mUntrackedCount = pool.mUntrackedCount;
    snapshot.mUnknownFreeCount = pool.mUnknownFreeCount;
    UnlockPool(pool);
    return snapshot;
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


//------------------------------------------------------------------------------
// HasMultiplicationOverflow
//------------------------------------------------------------------------------
//  Check whether an allocation size multiplication would overflow.
//------------------------------------------------------------------------------
bool HasMultiplicationOverflow(std::size_t count, std::size_t size)
{
    return size != 0 && count > (std::numeric_limits<std::size_t>::max() / size);
}


//------------------------------------------------------------------------------
// AllocationSizeProduct
//------------------------------------------------------------------------------
//  Calculate a multiplied allocation size with overflow saturation.
//------------------------------------------------------------------------------
std::size_t AllocationSizeProduct(std::size_t count, std::size_t size)
{
    return HasMultiplicationOverflow(count, size) ? std::numeric_limits<std::size_t>::max()
                                                 : count * size;
}


//------------------------------------------------------------------------------
// IsValidAlignment
//------------------------------------------------------------------------------
//  Check whether an alignment is valid for posix_memalign.
//------------------------------------------------------------------------------
bool IsValidAlignment(std::size_t alignment)
{
    return alignment >= sizeof(void* ) && (alignment % sizeof(void* )) == 0 &&
           (alignment & (alignment - 1)) == 0;
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
// RoundUpToPageSize
//------------------------------------------------------------------------------
//  Round a size up to a page-size multiple for pvalloc.
//------------------------------------------------------------------------------
std::size_t RoundUpToPageSize(std::size_t size)
{
    const std::size_t pageSize = PageSize();
    if (size > std::numeric_limits<std::size_t>::max() - pageSize + 1)
    {
        return std::numeric_limits<std::size_t>::max();
    }

    return ((size + pageSize - 1) / pageSize) * pageSize;
}


//------------------------------------------------------------------------------
// RecordMalloc
//------------------------------------------------------------------------------
//  Increment the malloc hook counter.
//------------------------------------------------------------------------------
void RecordMalloc()
{
    gMallocCount.fetch_add(1, std::memory_order_relaxed);
}


//------------------------------------------------------------------------------
// RecordCalloc
//------------------------------------------------------------------------------
//  Increment the calloc hook counter.
//------------------------------------------------------------------------------
void RecordCalloc()
{
    gCallocCount.fetch_add(1, std::memory_order_relaxed);
}


//------------------------------------------------------------------------------
// RecordRealloc
//------------------------------------------------------------------------------
//  Increment the realloc hook counter.
//------------------------------------------------------------------------------
void RecordRealloc()
{
    gReallocCount.fetch_add(1, std::memory_order_relaxed);
}


//------------------------------------------------------------------------------
// RecordReallocArray
//------------------------------------------------------------------------------
//  Increment the reallocarray hook counter.
//------------------------------------------------------------------------------
void RecordReallocArray()
{
    gReallocArrayCount.fetch_add(1, std::memory_order_relaxed);
}


//------------------------------------------------------------------------------
// RecordFree
//------------------------------------------------------------------------------
//  Increment the free hook counter.
//------------------------------------------------------------------------------
void RecordFree()
{
    gFreeCount.fetch_add(1, std::memory_order_relaxed);
}


//------------------------------------------------------------------------------
// RecordPosixMemalign
//------------------------------------------------------------------------------
//  Increment the posix_memalign hook counter.
//------------------------------------------------------------------------------
void RecordPosixMemalign()
{
    gPosixMemalignCount.fetch_add(1, std::memory_order_relaxed);
}


//------------------------------------------------------------------------------
// RecordAlignedAlloc
//------------------------------------------------------------------------------
//  Increment the aligned_alloc hook counter.
//------------------------------------------------------------------------------
void RecordAlignedAlloc()
{
    gAlignedAllocCount.fetch_add(1, std::memory_order_relaxed);
}


//------------------------------------------------------------------------------
// RecordMemalign
//------------------------------------------------------------------------------
//  Increment the memalign hook counter.
//------------------------------------------------------------------------------
void RecordMemalign()
{
    gMemalignCount.fetch_add(1, std::memory_order_relaxed);
}


//------------------------------------------------------------------------------
// RecordValloc
//------------------------------------------------------------------------------
//  Increment the valloc hook counter.
//------------------------------------------------------------------------------
void RecordValloc()
{
    gVallocCount.fetch_add(1, std::memory_order_relaxed);
}


//------------------------------------------------------------------------------
// RecordPvalloc
//------------------------------------------------------------------------------
//  Increment the pvalloc hook counter.
//------------------------------------------------------------------------------
void RecordPvalloc()
{
    gPvallocCount.fetch_add(1, std::memory_order_relaxed);
}


//------------------------------------------------------------------------------
// WriteSummary
//------------------------------------------------------------------------------
//  Write allocation hook counters when the preload library unloads.
//------------------------------------------------------------------------------
void WriteSummary()
{
    const PoolSnapshot mainSnapshot = SnapshotPool(gMainAllocationPool);
    const PoolSnapshot internalSnapshot = SnapshotPool(gInternalAllocationPool);
    char buffer[1024] = {};
    const int written = std::snprintf(
        buffer,
        sizeof(buffer),
        "[HeapAllocationHooks] malloc=%llu calloc=%llu realloc=%llu reallocarray=%llu free=%llu "
        "posix_memalign=%llu aligned_alloc=%llu memalign=%llu valloc=%llu pvalloc=%llu\n"
        "[HeapAllocationHooks] main_active=%zu main_bytes=%zu main_tracked=%llu main_untracked=%llu main_unknown_free=%llu\n"
        "[HeapAllocationHooks] internal_active=%zu internal_bytes=%zu internal_tracked=%llu internal_untracked=%llu internal_unknown_free=%llu\n",
        gMallocCount.load(std::memory_order_relaxed),
        gCallocCount.load(std::memory_order_relaxed),
        gReallocCount.load(std::memory_order_relaxed),
        gReallocArrayCount.load(std::memory_order_relaxed),
        gFreeCount.load(std::memory_order_relaxed),
        gPosixMemalignCount.load(std::memory_order_relaxed),
        gAlignedAllocCount.load(std::memory_order_relaxed),
        gMemalignCount.load(std::memory_order_relaxed),
        gVallocCount.load(std::memory_order_relaxed),
        gPvallocCount.load(std::memory_order_relaxed),
        mainSnapshot.mActiveCount,
        mainSnapshot.mActiveBytes,
        mainSnapshot.mTrackedCount,
        mainSnapshot.mUntrackedCount,
        mainSnapshot.mUnknownFreeCount,
        internalSnapshot.mActiveCount,
        internalSnapshot.mActiveBytes,
        internalSnapshot.mTrackedCount,
        internalSnapshot.mUntrackedCount,
        internalSnapshot.mUnknownFreeCount);

    if (written > 0)
    {
        const std::size_t writeLength = static_cast<std::size_t>(written) < sizeof(buffer)
                                            ? static_cast<std::size_t>(written)
                                            : sizeof(buffer) - 1;
        const ssize_t ignoredResult = write(STDERR_FILENO, buffer, writeLength);
        static_cast<void>(ignoredResult);
    }
}


namespace
{


//------------------------------------------------------------------------------
// OnLibraryLoaded
//------------------------------------------------------------------------------
//  Announce that the preload hook library has been injected.
//------------------------------------------------------------------------------
__attribute__((constructor)) void OnLibraryLoaded()
{
    WriteText("[HeapAllocationHooks] loaded\n");
}


//------------------------------------------------------------------------------
// OnLibraryUnloaded
//------------------------------------------------------------------------------
//  Emit allocation hook counters before the preload hook library unloads.
//------------------------------------------------------------------------------
__attribute__((destructor)) void OnLibraryUnloaded()
{
    WriteSummary();
}

}

}
