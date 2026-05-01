#include "TrackAllocations.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
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
    HeapVisualizer::AllocationHooks::AllocationOperationType mType =
        HeapVisualizer::AllocationHooks::AllocationOperationType::Malloc;
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
    std::array<unsigned long long,
               static_cast<std::size_t>(HeapVisualizer::AllocationHooks::AllocationOperationType::Count)>
        mTrackedAllocationsByType = {};
    std::array<unsigned long long,
               static_cast<std::size_t>(HeapVisualizer::AllocationHooks::AllocationOperationType::Count)>
        mTrackedDeallocationsByType = {};
};

constexpr std::size_t MainAllocationCapacity = 0x40000;
constexpr std::size_t InternalAllocationCapacity = 0x40000;
constexpr char SnapshotMagic[] = {'H', 'V', 'I', 'Z', 'A', 'L', 'L', 'C'};
constexpr char SnapshotFooter[] = {'H', 'V', 'I', 'Z', 'D', 'O', 'N', 'E'};
constexpr std::uint32_t PoolIdentifierMain = 0;
constexpr std::uint32_t PoolIdentifierInternal = 1;
constexpr long StreamingIntervalNanoseconds = 100000000;
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
    0,
    {},
    {}};
AllocationPool gInternalAllocationPool = {
    ATOMIC_FLAG_INIT,
    gInternalAllocationRecords,
    InternalAllocationCapacity,
    0,
    0,
    0,
    0,
    0,
    0,
    {},
    {}};

}


namespace HeapVisualizer::AllocationHooks
{

namespace
{


void LockPool(AllocationPool& pool);
void UnlockPool(AllocationPool& pool);


//------------------------------------------------------------------------------
// OperationTypeIndex
//------------------------------------------------------------------------------
//  Convert an allocation operation type into a count-array index.
//------------------------------------------------------------------------------
std::size_t OperationTypeIndex(AllocationOperationType type)
{
    return static_cast<std::size_t>(type);
}


//------------------------------------------------------------------------------
// IsValidOperationType
//------------------------------------------------------------------------------
//  Return whether an allocation operation type is inside the known range.
//------------------------------------------------------------------------------
bool IsValidOperationType(AllocationOperationType type)
{
    return OperationTypeIndex(type) < OperationTypeIndex(AllocationOperationType::Count);
}


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
// SendUInt32
//------------------------------------------------------------------------------
//  Send a 32-bit unsigned integer in little-endian byte order.
//------------------------------------------------------------------------------
bool SendUInt32(int socketFileDescriptor, std::uint32_t value)
{
    const char bytes[4] = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU)};

    return SendAll(socketFileDescriptor, bytes, sizeof(bytes));
}


//------------------------------------------------------------------------------
// SendUInt64
//------------------------------------------------------------------------------
//  Send a 64-bit unsigned integer in little-endian byte order.
//------------------------------------------------------------------------------
bool SendUInt64(int socketFileDescriptor, std::uint64_t value)
{
    const char bytes[8] = {
        static_cast<char>(value & 0xffULL),
        static_cast<char>((value >> 8U) & 0xffULL),
        static_cast<char>((value >> 16U) & 0xffULL),
        static_cast<char>((value >> 24U) & 0xffULL),
        static_cast<char>((value >> 32U) & 0xffULL),
        static_cast<char>((value >> 40U) & 0xffULL),
        static_cast<char>((value >> 48U) & 0xffULL),
        static_cast<char>((value >> 56U) & 0xffULL)};

    return SendAll(socketFileDescriptor, bytes, sizeof(bytes));
}


//------------------------------------------------------------------------------
// SendAllocationPool
//------------------------------------------------------------------------------
//  Serialize one allocation tracking pool to a socket.
//------------------------------------------------------------------------------
bool SendAllocationPool(int socketFileDescriptor, std::uint32_t poolIdentifier, AllocationPool& pool)
{
    LockPool(pool);

    bool sent = SendUInt32(socketFileDescriptor, poolIdentifier) &&
                SendUInt64(socketFileDescriptor, static_cast<std::uint64_t>(pool.mAllocationCount)) &&
                SendUInt64(socketFileDescriptor, static_cast<std::uint64_t>(pool.mAllocationBytes)) &&
                SendUInt64(socketFileDescriptor, static_cast<std::uint64_t>(pool.mTrackedAllocations)) &&
                SendUInt64(socketFileDescriptor, static_cast<std::uint64_t>(pool.mTrackedDeallocations)) &&
                SendUInt64(socketFileDescriptor, static_cast<std::uint64_t>(pool.mUntrackedAllocations)) &&
                SendUInt64(socketFileDescriptor, static_cast<std::uint64_t>(pool.mUntrackedDeallocations));

    for (std::size_t typeIndex = 0;
         sent && (typeIndex < OperationTypeIndex(AllocationOperationType::Count));
         ++typeIndex)
    {
        sent = SendUInt64(socketFileDescriptor, static_cast<std::uint64_t>(pool.mTrackedAllocationsByType[typeIndex]));
    }

    for (std::size_t typeIndex = 0;
         sent && (typeIndex < OperationTypeIndex(AllocationOperationType::Count));
         ++typeIndex)
    {
        sent = SendUInt64(socketFileDescriptor, static_cast<std::uint64_t>(pool.mTrackedDeallocationsByType[typeIndex]));
    }

    for (std::size_t index = 0; sent && (index < pool.mCapacity); ++index)
    {
        const AllocationRecord& record = pool.mRecords[index];
        if (IsTrackedAddress(record))
        {
            const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(record.mAddress));
            sent = SendUInt64(socketFileDescriptor, address) &&
                   SendUInt64(socketFileDescriptor, static_cast<std::uint64_t>(record.mSize)) &&
                   SendUInt32(socketFileDescriptor, static_cast<std::uint32_t>(record.mType));
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
bool SendSerializedAllocations(int socketFileDescriptor)
{
    // Send the header.
    if (!SendAll(socketFileDescriptor, SnapshotMagic, sizeof(SnapshotMagic)))
    {
        return false;
    }

    // Send the number of pools.
    if (!SendUInt32(socketFileDescriptor, 2))
    {
        return false;
    }

    // Send the main pool.
    if (!SendAllocationPool(socketFileDescriptor, PoolIdentifierMain, gMainAllocationPool))
    {
        return false;
    }

    // Send the internal pool.
    if (!SendAllocationPool(socketFileDescriptor, PoolIdentifierInternal, gInternalAllocationPool))
    {
        return false;
    }

    // Send the footer.
    return SendAll(socketFileDescriptor, SnapshotFooter, sizeof(SnapshotFooter));
}


//------------------------------------------------------------------------------
// ConnectToVisualizer
//------------------------------------------------------------------------------
//  Connect to the visualizer and return the connected socket.
//------------------------------------------------------------------------------
int ConnectToVisualizer()
{
    const char* host = ReadEnvironmentString("HEAP_VISUALIZER_STREAM_HOST", "127.0.0.1");
    const int port = ReadEnvironmentInt("HEAP_VISUALIZER_STREAM_PORT", 47329);

    int socketFileDescriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socketFileDescriptor < 0)
    {
        return -1;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));

    if (inet_pton(AF_INET, host, &address.sin_addr) != 1)
    {
        close(socketFileDescriptor);
        return -1;
    }

    if (connect(socketFileDescriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        close(socketFileDescriptor);
        return -1;
    }

    return socketFileDescriptor;
}


//------------------------------------------------------------------------------
// StreamingIntervalFromNow
//------------------------------------------------------------------------------
//  Return an absolute realtime clock value for the next streaming interval.
//------------------------------------------------------------------------------
timespec StreamingIntervalFromNow()
{
    timespec timeout = {};
    clock_gettime(CLOCK_REALTIME, &timeout);

    timeout.tv_nsec += StreamingIntervalNanoseconds;
    if (timeout.tv_nsec >= 1000000000)
    {
        timeout.tv_nsec -= 1000000000;
        ++timeout.tv_sec;
    }

    return timeout;
}


//------------------------------------------------------------------------------
// WaitForStreamingStopSignal
//------------------------------------------------------------------------------
//  Wait up to one streaming interval for the streaming thread to stop.
//------------------------------------------------------------------------------
bool WaitForStreamingStopSignal()
{
    const timespec timeout = StreamingIntervalFromNow();
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
    int socketFileDescriptor = -1;

    while (gStreamingThreadRunning.load(std::memory_order_acquire))
    {
        if (socketFileDescriptor < 0)
        {
            socketFileDescriptor = ConnectToVisualizer();
        }

        if (socketFileDescriptor >= 0)
        {
            if (!SendSerializedAllocations(socketFileDescriptor))
            {
                close(socketFileDescriptor);
                socketFileDescriptor = -1;
            }
        }

        if (WaitForStreamingStopSignal())
        {
            break;
        }
    }

    if (socketFileDescriptor >= 0)
    {
        close(socketFileDescriptor);
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
void TrackAllocationInPool(AllocationPool& pool, void* pointer, std::size_t size, AllocationOperationType type)
{
    if ((pointer == nullptr) || (pool.mRecords == nullptr) || (pool.mCapacity == 0) || (!IsValidOperationType(type)))
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
            record.mType = type;
            ++pool.mAllocationCount;
            ++pool.mTrackedAllocations;
            ++pool.mTrackedAllocationsByType[OperationTypeIndex(type)];
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
bool UntrackAllocationInPool(AllocationPool& pool, void* pointer, AllocationOperationType type)
{
    if ((pointer == nullptr) || (pool.mRecords == nullptr) || (pool.mCapacity == 0) || (!IsValidOperationType(type)))
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
            ++pool.mTrackedDeallocationsByType[OperationTypeIndex(type)];
            record.mAddress = TombstoneAddress;
            record.mSize = 0;
            record.mType = AllocationOperationType::Malloc;
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
void TrackAllocationTarget(void* pointer, std::size_t size, AllocationOperationType type)
{
    TrackAllocationInPool(gMainAllocationPool, pointer, size, type);
}


//------------------------------------------------------------------------------
// TrackAllocationInternal
//------------------------------------------------------------------------------
//  Track an allocation owned by the preload hook library internals.
//------------------------------------------------------------------------------
void TrackAllocationInternal(void* pointer, std::size_t size, AllocationOperationType type)
{
    TrackAllocationInPool(gInternalAllocationPool, pointer, size, type);
}


//------------------------------------------------------------------------------
// UntrackAllocation
//------------------------------------------------------------------------------
//  Remove an allocation from whichever pool owns it.
//------------------------------------------------------------------------------
void UntrackAllocation(void* pointer, AllocationOperationType type)
{
    if (pointer == nullptr)
    {
        return;
    }

    if (UntrackAllocationInPool(gMainAllocationPool, pointer, type))
    {
        return;
    }

    if (UntrackAllocationInPool(gInternalAllocationPool, pointer, type))
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
