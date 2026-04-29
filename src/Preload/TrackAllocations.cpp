#include "TrackAllocations.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <thread>

namespace
{

std::atomic_bool gStreamingThreadRunning = false;
std::thread gStreamingThread;
pthread_mutex_t gStreamingWaitMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t gStreamingStopCondition = PTHREAD_COND_INITIALIZER;

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
    std::size_t mAllocationCount = 0;
    std::size_t mAllocationBytes = 0;
    unsigned long long mTrackedAllocations = 0;
    unsigned long long mTrackedDeallocations = 0;
    unsigned long long mUntrackedAllocations = 0;
    unsigned long long mUntrackedDeallocations = 0;
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
    0,
    0};

}


namespace HeapVisualizer::AllocationHooks
{

namespace
{


void LockPool(AllocationPool& pool);
void UnlockPool(AllocationPool& pool);


//------------------------------------------------------------------------------
// IsTrackedAddress
//------------------------------------------------------------------------------
//  Return whether an allocation record contains a live tracked allocation.
//------------------------------------------------------------------------------
bool IsTrackedAddress(const AllocationRecord& record)
{
    return record.mAddress != nullptr && record.mAddress != TombstoneAddress;
}


//------------------------------------------------------------------------------
// ReadEnvironmentInt
//------------------------------------------------------------------------------
//  Read an integer environment variable without allocating memory.
//------------------------------------------------------------------------------
int ReadEnvironmentInt(const char* variableName, int fallbackValue)
{
    const char* value = std::getenv(variableName);
    if ((value == nullptr) || (*value == '\0'))
    {
        return fallbackValue;
    }

    char* endPointer = nullptr;
    const long parsedValue = std::strtol(value, &endPointer, 10);
    if ((endPointer == value) || (parsedValue <= 0) || (parsedValue > 65535))
    {
        return fallbackValue;
    }

    return static_cast<int>(parsedValue);
}


//------------------------------------------------------------------------------
// ReadEnvironmentString
//------------------------------------------------------------------------------
//  Read a string environment variable with a non-owning fallback.
//------------------------------------------------------------------------------
const char* ReadEnvironmentString(const char* variableName, const char* fallbackValue)
{
    const char* value = std::getenv(variableName);
    return value == nullptr || *value == '\0' ? fallbackValue : value;
}


//------------------------------------------------------------------------------
// SendAll
//------------------------------------------------------------------------------
//  Write a full buffer to a socket unless the peer disconnects.
//------------------------------------------------------------------------------
bool SendAll(int socketFileDescriptor, const char* data, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size)
    {
        const ssize_t bytesSent = send(
            socketFileDescriptor,
            data + offset,
            size - offset,
            MSG_NOSIGNAL);

        if (bytesSent <= 0)
        {
            return false;
        }

        offset += static_cast<std::size_t>(bytesSent);
    }

    return true;
}


//------------------------------------------------------------------------------
// SendFormatted
//------------------------------------------------------------------------------
//  Format a small streaming line on the stack and send it to a socket.
//------------------------------------------------------------------------------
bool SendFormatted(int socketFileDescriptor, const char* format, ...)
{
    char buffer[256] = {};
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);

    if (written <= 0)
    {
        return false;
    }

    const std::size_t writeLength = static_cast<std::size_t>(written) < sizeof(buffer)
                                        ? static_cast<std::size_t>(written)
                                        : sizeof(buffer) - 1;
    return SendAll(socketFileDescriptor, buffer, writeLength);
}


//------------------------------------------------------------------------------
// SendAllocationPool
//------------------------------------------------------------------------------
//  Serialize one allocation tracking pool to a socket.
//------------------------------------------------------------------------------
bool SendAllocationPool(int socketFileDescriptor, const char* poolName, AllocationPool& pool)
{
    LockPool(pool);

    bool sent = SendFormatted(
        socketFileDescriptor,
        "POOL %s %zu %zu %llu %llu %llu %llu\n",
        poolName,
        pool.mAllocationCount,
        pool.mAllocationBytes,
        pool.mTrackedAllocations,
        pool.mTrackedDeallocations,
        pool.mUntrackedAllocations,
        pool.mUntrackedDeallocations);

    for (std::size_t index = 0; sent && (index < pool.mCapacity); ++index)
    {
        const AllocationRecord& record = pool.mRecords[index];
        if (IsTrackedAddress(record))
        {
            sent = SendFormatted(
                socketFileDescriptor,
                "ALLOC %s %zx %zu\n",
                poolName,
                reinterpret_cast<std::uintptr_t>(record.mAddress),
                record.mSize);
        }
    }

    UnlockPool(pool);
    return sent;
}


//------------------------------------------------------------------------------
// SendSerializedAllocations
//------------------------------------------------------------------------------
//  Serialize tracked heap allocation state to a connected socket.
//------------------------------------------------------------------------------
void SendSerializedAllocations(int socketFileDescriptor)
{
    if (!SendAll(socketFileDescriptor, "HEAPVIZ_ALLOCATIONS 1\n", 22))
    {
        return;
    }

    if (!SendAllocationPool(socketFileDescriptor, "main", gMainAllocationPool))
    {
        return;
    }

    if (!SendAllocationPool(socketFileDescriptor, "internal", gInternalAllocationPool))
    {
        return;
    }

    SendAll(socketFileDescriptor, "END\n", 4);
}


//------------------------------------------------------------------------------
// ConnectAndSendSerializedAllocations
//------------------------------------------------------------------------------
//  Connect to the visualizer and send the current allocation snapshot.
//------------------------------------------------------------------------------
void ConnectAndSendSerializedAllocations()
{
    const char* host = ReadEnvironmentString("HEAP_VISUALIZER_STREAM_HOST", "127.0.0.1");
    const int port = ReadEnvironmentInt("HEAP_VISUALIZER_STREAM_PORT", 47329);

    int socketFileDescriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socketFileDescriptor < 0)
    {
        return;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));

    if (inet_pton(AF_INET, host, &address.sin_addr) != 1)
    {
        close(socketFileDescriptor);
        return;
    }

    if (connect(socketFileDescriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
    {
        SendSerializedAllocations(socketFileDescriptor);
    }

    close(socketFileDescriptor);
}


//------------------------------------------------------------------------------
// OneSecondFromNow
//------------------------------------------------------------------------------
//  Return an absolute realtime clock value one second from now.
//------------------------------------------------------------------------------
timespec OneSecondFromNow()
{
    timespec timeout = {};
    clock_gettime(CLOCK_REALTIME, &timeout);
    ++timeout.tv_sec;
    return timeout;
}


//------------------------------------------------------------------------------
// WaitForStreamingStopSignal
//------------------------------------------------------------------------------
//  Wait up to one second for the streaming thread to be asked to stop.
//------------------------------------------------------------------------------
bool WaitForStreamingStopSignal()
{
    const timespec timeout = OneSecondFromNow();
    pthread_mutex_lock(&gStreamingWaitMutex);

    while (gStreamingThreadRunning.load(std::memory_order_acquire))
    {
        const int waitResult = pthread_cond_timedwait(
            &gStreamingStopCondition,
            &gStreamingWaitMutex,
            &timeout);

        if (waitResult == ETIMEDOUT)
        {
            break;
        }
    }

    const bool stopRequested = !gStreamingThreadRunning.load(std::memory_order_acquire);
    pthread_mutex_unlock(&gStreamingWaitMutex);
    return stopRequested;
}


//------------------------------------------------------------------------------
// StreamingThreadMain
//------------------------------------------------------------------------------
//  Periodically connect to the visualizer and send allocation snapshots.
//------------------------------------------------------------------------------
void StreamingThreadMain()
{
    while (gStreamingThreadRunning.load(std::memory_order_acquire))
    {
        ConnectAndSendSerializedAllocations();
        if (WaitForStreamingStopSignal())
        {
            break;
        }
    }
}


//------------------------------------------------------------------------------
// StartStreamingThread
//------------------------------------------------------------------------------
//  Start the allocation snapshot sender thread.
//------------------------------------------------------------------------------
void StartStreamingThread()
{
    bool expected = false;
    if (!gStreamingThreadRunning.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return;
    }

    gStreamingThread = std::thread(StreamingThreadMain);
}


//------------------------------------------------------------------------------
// StopStreamingThread
//------------------------------------------------------------------------------
//  Stop the allocation snapshot sender thread.
//------------------------------------------------------------------------------
void StopStreamingThread()
{
    if (!gStreamingThreadRunning.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    pthread_mutex_lock(&gStreamingWaitMutex);
    pthread_cond_signal(&gStreamingStopCondition);
    pthread_mutex_unlock(&gStreamingWaitMutex);

    if (gStreamingThread.joinable())
    {
        gStreamingThread.join();
    }
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
// PreviousRecordIndex
//------------------------------------------------------------------------------
//  Return the previous circular record index in an allocation pool.
//------------------------------------------------------------------------------
std::size_t PreviousRecordIndex(const AllocationPool& pool, std::size_t recordIndex)
{
    return recordIndex == 0 ? pool.mCapacity - 1 : recordIndex - 1;
}


//------------------------------------------------------------------------------
// CleanupTrailingTombstones
//------------------------------------------------------------------------------
//  Clear tombstones that no longer protect a live open-addressing probe chain.
//------------------------------------------------------------------------------
void CleanupTrailingTombstones(AllocationPool& pool, std::size_t deletedRecordIndex)
{
    std::size_t recordIndex = (deletedRecordIndex + 1) % pool.mCapacity;
    for (std::size_t offset = 0; offset < pool.mCapacity; ++offset)
    {
        const AllocationRecord& record = pool.mRecords[recordIndex];
        if ((record.mAddress != nullptr) && (record.mAddress != TombstoneAddress))
        {
            return;
        }

        if (record.mAddress == nullptr)
        {
            break;
        }

        recordIndex = (recordIndex + 1) % pool.mCapacity;
    }

    recordIndex = deletedRecordIndex;
    for (std::size_t offset = 0; offset < pool.mCapacity; ++offset)
    {
        AllocationRecord& record = pool.mRecords[recordIndex];
        if (record.mAddress != TombstoneAddress)
        {
            break;
        }

        record.mAddress = nullptr;
        recordIndex = PreviousRecordIndex(pool, recordIndex);
    }
}


//------------------------------------------------------------------------------
// TrackAllocationInPool
//------------------------------------------------------------------------------
//  Track a single allocation in a fixed-size tracking pool.
//------------------------------------------------------------------------------
void TrackAllocationInPool(AllocationPool& pool, void* pointer, std::size_t size)
{
    if ((pointer == nullptr) || (pool.mRecords == nullptr) || (pool.mCapacity == 0))
    {
        return;
    }

    LockPool(pool);

    const std::size_t startIndex = HashPointer(pointer, pool.mCapacity);
    for (std::size_t offset = 0; offset < pool.mCapacity; ++offset)
    {
        const std::size_t recordIndex = (startIndex + offset) % pool.mCapacity;
        AllocationRecord& record = pool.mRecords[recordIndex];

        if ((record.mAddress == nullptr) || (record.mAddress == TombstoneAddress))
        {
            record.mAddress = pointer;
            record.mSize = size;
            ++pool.mAllocationCount;
            ++pool.mTrackedAllocations;
            pool.mAllocationBytes += size;
            UnlockPool(pool);
            return;
        }
    }

    ++pool.mUntrackedAllocations;
    UnlockPool(pool);
}


//------------------------------------------------------------------------------
// UntrackAllocationInPool
//------------------------------------------------------------------------------
//  Remove a single allocation from a fixed-size tracking pool.
//------------------------------------------------------------------------------
bool UntrackAllocationInPool(AllocationPool& pool, void* pointer)
{
    if ((pointer == nullptr) || (pool.mRecords == nullptr) || (pool.mCapacity == 0))
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
            pool.mAllocationBytes -= record.mSize;
            --pool.mAllocationCount;
            ++pool.mTrackedDeallocations;
            record.mAddress = TombstoneAddress;
            record.mSize = 0;
            CleanupTrailingTombstones(pool, recordIndex);
            UnlockPool(pool);
            return true;
        }

        if (record.mAddress == nullptr)
        {
            break;
        }
    }

    UnlockPool(pool);
    return false;
}


//------------------------------------------------------------------------------
// RecordUnknownDeallocation
//------------------------------------------------------------------------------
//  Record a deallocation that did not match any tracked allocation.
//------------------------------------------------------------------------------
void RecordUnknownDeallocation(AllocationPool& pool)
{
    LockPool(pool);
    ++pool.mUntrackedDeallocations;
    UnlockPool(pool);
}


}


//------------------------------------------------------------------------------
// TrackAllocationTarget
//------------------------------------------------------------------------------
//  Track an allocation owned by the target application.
//------------------------------------------------------------------------------
void TrackAllocationTarget(void* pointer, std::size_t size)
{
    TrackAllocationInPool(gMainAllocationPool, pointer, size);
}


//------------------------------------------------------------------------------
// TrackAllocationInternal
//------------------------------------------------------------------------------
//  Track an allocation owned by the preload hook library internals.
//------------------------------------------------------------------------------
void TrackAllocationInternal(void* pointer, std::size_t size)
{
    TrackAllocationInPool(gInternalAllocationPool, pointer, size);
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

    if (UntrackAllocationInPool(gMainAllocationPool, pointer))
    {
        return;
    }

    if (UntrackAllocationInPool(gInternalAllocationPool, pointer))
    {
        return;
    }

    RecordUnknownDeallocation(gMainAllocationPool);
}


namespace
{


//------------------------------------------------------------------------------
// OnLibraryLoaded
//------------------------------------------------------------------------------
//  Start streaming allocation snapshots when the preload hook library loads.
//------------------------------------------------------------------------------
__attribute__((constructor)) void OnLibraryLoaded()
{
    StartStreamingThread();
}


//------------------------------------------------------------------------------
// OnLibraryUnloaded
//------------------------------------------------------------------------------
//  Stop streaming allocation snapshots before the preload hook library unloads.
//------------------------------------------------------------------------------
__attribute__((destructor)) void OnLibraryUnloaded()
{
    StopStreamingThread();
}

}

}
