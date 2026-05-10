#include "TrackAllocations.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct AllocationRecord
{
    void* mAddress;
    size_t mSize;
    AllocationType mType;
} AllocationRecord;

typedef struct AllocationPool
{
    pthread_mutex_t mLock;
    AllocationRecord* mRecords;
    uint64_t mCapacity;
    uint64_t mAllocationCount;
    uint64_t mAllocationBytes;
    uint64_t mTrackedAllocations;
    uint64_t mTrackedDeallocations;
    uint64_t mUntrackedAllocations;
    uint64_t mUntrackedDeallocations;
    uint64_t mTrackedAllocationsByType[AllocationType_Count];
    uint64_t mTrackedDeallocationsByType[AllocationType_Count];
} AllocationPool;

static const char SnapshotMagic[] = {'H', 'V', 'I', 'Z', 'A', 'L', 'L', 'C'};
static const char SnapshotFooter[] = {'H', 'V', 'I', 'Z', 'D', 'O', 'N', 'E'};
static const long StreamingIntervalNanoseconds = 100000000;
static const uint64_t DefaultSystemPageSize = 4096;
static void* const TombstoneAddress = (void*)(uintptr_t)1;

static pthread_mutex_t gStreamingWaitMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t gStreamingThread;
static uint64_t gSystemPageSize = DefaultSystemPageSize;

// Track up to 1 million allocations
#define NUM_ALLOCATION_RECORDS      0x100000
#define SNAPSHOT_POOL_COUNTER_COUNT 6                   // AllocationPool number of counters
#define SNAPSHOT_PAGE_SIZE_BYTES    sizeof(uint64_t)    // Additional page size size
#define SNAPSHOT_COUNTER_BYTES      sizeof(uint64_t)    // AllocationPool count member sizes
#define SNAPSHOT_ADDRESS_BYTES      sizeof(uint64_t)    // AllocationRecord.mAddress, converted to uint64_t when serialized
#define SNAPSHOT_SIZE_BYTES         sizeof(size_t)      // AllocationRecord.mSize
#define SNAPSHOT_TYPE_BYTES         sizeof(uint32_t)    // AllocationRecord.mType, converted to uint32_t when serialized
#define SNAPSHOT_ALLOCATION_RECORD_BYTES (SNAPSHOT_ADDRESS_BYTES + SNAPSHOT_SIZE_BYTES + SNAPSHOT_TYPE_BYTES)
static AllocationRecord gAllocationRecords[NUM_ALLOCATION_RECORDS];
static AllocationPool gAllocationPool = {
    PTHREAD_MUTEX_INITIALIZER,
    gAllocationRecords,
    NUM_ALLOCATION_RECORDS,
    0,
    0,
    0,
    0,
    0,
    0,
    {0},
    {0}
};
static uint8_t gSnapshotSendBuffer[
    sizeof(SnapshotMagic) +                                         // header
    (SNAPSHOT_POOL_COUNTER_COUNT * SNAPSHOT_COUNTER_BYTES) +        // counters
    (SNAPSHOT_PAGE_SIZE_BYTES) +                                    // page size
    (AllocationType_Count * SNAPSHOT_COUNTER_BYTES) +               // allocations by type
    (AllocationType_Count * SNAPSHOT_COUNTER_BYTES) +               // deallocations by type
    (NUM_ALLOCATION_RECORDS * SNAPSHOT_ALLOCATION_RECORD_BYTES) +   // allocation records
    sizeof(SnapshotFooter)                                          // footer
];


//------------------------------------------------------------------------------
// OperationTypeIndex
//------------------------------------------------------------------------------
//  Convert an allocation operation type into a count-array index.
//------------------------------------------------------------------------------
static inline size_t OperationTypeIndex(AllocationType type)
{
    return (size_t)type;
}


//------------------------------------------------------------------------------
// IsValidOperationType
//------------------------------------------------------------------------------
//  Return whether an allocation operation type is inside the known range.
//------------------------------------------------------------------------------
static inline bool IsValidOperationType(AllocationType type)
{
    return OperationTypeIndex(type) < OperationTypeIndex(AllocationType_Count);
}


//------------------------------------------------------------------------------
// IsTrackedAddress
//------------------------------------------------------------------------------
//  Return whether an allocation record contains a live tracked allocation.
//------------------------------------------------------------------------------
static bool IsTrackedAddress(const AllocationRecord* record)
{
    return ((record->mAddress != NULL) && (record->mAddress != TombstoneAddress));
}


//------------------------------------------------------------------------------
// ReadEnvironmentInt
//------------------------------------------------------------------------------
//  Read an integer environment variable without allocating memory.
//------------------------------------------------------------------------------
static int ReadEnvironmentInt(const char* variableName, int fallbackValue)
{
    char* endPointer = NULL;
    const char* value = getenv(variableName);
    long parsedValue = 0;

    if ((value == NULL) || (*value == '\0'))
    {
        return fallbackValue;
    }

    parsedValue = strtol(value, &endPointer, 10);
    if ((endPointer == value) || (parsedValue <= 0) || (parsedValue > 65535))
    {
        return fallbackValue;
    }

    return (int)parsedValue;
}


//------------------------------------------------------------------------------
// ReadSystemPageSize
//------------------------------------------------------------------------------
//  Read the operating system memory page size.
//------------------------------------------------------------------------------
static uint64_t ReadSystemPageSize(void)
{
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0)
    {
        return DefaultSystemPageSize;
    }
    return (uint64_t)pageSize;
}


//------------------------------------------------------------------------------
// ReadEnvironmentString
//------------------------------------------------------------------------------
//  Read a string environment variable with a non-owning fallback.
//------------------------------------------------------------------------------
static const char* ReadEnvironmentString(const char* variableName, const char* fallbackValue)
{
    const char* value = getenv(variableName);
    return ((value == NULL) || (*value == '\0')) ? fallbackValue : value;
}


//------------------------------------------------------------------------------
// SendAll
//------------------------------------------------------------------------------
//  Write a full buffer to a socket unless the peer disconnects.
//------------------------------------------------------------------------------
static bool SendAll(int fd, const uint8_t* data, size_t size)
{
    size_t offset = 0;
    while (offset < size)
    {
        const ssize_t bytesSent = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (bytesSent <= 0)
        {
            return false;
        }
        offset += (size_t)bytesSent;
    }
    return true;
}


//------------------------------------------------------------------------------
// LockPool
//------------------------------------------------------------------------------
//  Acquire the tracking pool lock.
//------------------------------------------------------------------------------
static inline void LockPool(AllocationPool* pool)
{
    pthread_mutex_lock(&pool->mLock);
}


//------------------------------------------------------------------------------
// UnlockPool
//------------------------------------------------------------------------------
//  Release the tracking pool lock.
//------------------------------------------------------------------------------
static inline void UnlockPool(AllocationPool* pool)
{
    pthread_mutex_unlock(&pool->mLock);
}


//------------------------------------------------------------------------------
// AppendBytes
//------------------------------------------------------------------------------
//  Append raw bytes to a bounded serialization buffer.
//------------------------------------------------------------------------------
static bool AppendBytes(uint8_t* buffer, size_t capacity, size_t* offset, const void* data, size_t size)
{
    if ((*offset > capacity) || (size > capacity - *offset))
    {
        return false;
    }

    memcpy(buffer + *offset, data, size);
    *offset += size;
    return true;
}


//------------------------------------------------------------------------------
// AppendUInt32
//------------------------------------------------------------------------------
//  Append a 32-bit unsigned integer in little-endian byte order.
//------------------------------------------------------------------------------
static bool AppendUInt32(unsigned char* buffer, size_t capacity, size_t* offset, uint32_t value)
{
    const uint8_t bytes[4] = {
        (uint8_t)((value >>  0) & 0xFF),
        (uint8_t)((value >>  8) & 0xFF),
        (uint8_t)((value >> 16) & 0xFF),
        (uint8_t)((value >> 24) & 0xFF)
    };

    return AppendBytes(buffer, capacity, offset, bytes, sizeof(bytes));
}


//------------------------------------------------------------------------------
// AppendUInt64
//------------------------------------------------------------------------------
//  Append a 64-bit unsigned integer in little-endian byte order.
//------------------------------------------------------------------------------
static bool AppendUInt64(uint8_t* buffer, size_t capacity, size_t* offset, uint64_t value)
{
    const uint8_t bytes[8] = {
        (uint8_t)((value >>  0) & 0xFF),
        (uint8_t)((value >>  8) & 0xFF),
        (uint8_t)((value >> 16) & 0xFF),
        (uint8_t)((value >> 24) & 0xFF),
        (uint8_t)((value >> 32) & 0xFF),
        (uint8_t)((value >> 40) & 0xFF),
        (uint8_t)((value >> 48) & 0xFF),
        (uint8_t)((value >> 56) & 0xFF)
    };

    return AppendBytes(buffer, capacity, offset, bytes, sizeof(bytes));
}


//------------------------------------------------------------------------------
// SendSerializedAllocations
//------------------------------------------------------------------------------
//  Serialize tracked heap allocation state, then send it to a connected socket.
//------------------------------------------------------------------------------
static bool SendSerializedAllocations(int fd)
{
    size_t snapshotSize = 0;

    // Lock the allocation pool so we can serialize its content.
    LockPool(&gAllocationPool);

    // Add the header and counters.
    bool serialized =   AppendBytes(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, SnapshotMagic, sizeof(SnapshotMagic)) &&
                        AppendUInt64(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, gAllocationPool.mAllocationCount) &&
                        AppendUInt64(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, gAllocationPool.mAllocationBytes) &&
                        AppendUInt64(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, gSystemPageSize) &&
                        AppendUInt64(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, gAllocationPool.mTrackedAllocations) &&
                        AppendUInt64(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, gAllocationPool.mTrackedDeallocations) &&
                        AppendUInt64(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, gAllocationPool.mUntrackedAllocations) &&
                        AppendUInt64(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, gAllocationPool.mUntrackedDeallocations);

    // Add the allocations by type.
    for (size_t typeIndex = 0; serialized && (typeIndex < OperationTypeIndex(AllocationType_Count)); typeIndex++)
    {
        serialized &= AppendUInt64(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, gAllocationPool.mTrackedAllocationsByType[typeIndex]);
    }
    // Add the deallocations by type.
    for (size_t typeIndex = 0; serialized && (typeIndex < OperationTypeIndex(AllocationType_Count)); typeIndex++)
    {
        serialized &= AppendUInt64(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, gAllocationPool.mTrackedDeallocationsByType[typeIndex]);
    }

    // Add the allocations records.
    for (size_t index = 0; serialized && (index < gAllocationPool.mCapacity); index++)
    {
        const AllocationRecord* record = &gAllocationPool.mRecords[index];
        if (IsTrackedAddress(record))
        {
            serialized &=   AppendUInt64(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, (uint64_t)(uintptr_t)record->mAddress) &&
                            AppendUInt64(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, (uint64_t)record->mSize) &&
                            AppendUInt32(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, (uint32_t)record->mType);
        }
    }

    // Add the footer.
    serialized &= AppendBytes(gSnapshotSendBuffer, sizeof(gSnapshotSendBuffer), &snapshotSize, SnapshotFooter, sizeof(SnapshotFooter));

    // Unlock the pool when serialization is done and before we try to send the data.
    UnlockPool(&gAllocationPool);

    // If we didn't have serialization issues, send the buffer.
    if (serialized)
    {
        return SendAll(fd, gSnapshotSendBuffer, snapshotSize);
    }
    return false;
}


//------------------------------------------------------------------------------
// ConnectToVisualizer
//------------------------------------------------------------------------------
//  Connect to the visualizer and return the connected socket.
//------------------------------------------------------------------------------
static int ConnectToVisualizer(void)
{
    const char* host = ReadEnvironmentString("HEAP_VISUALIZER_STREAM_HOST", "127.0.0.1");
    const int port = ReadEnvironmentInt("HEAP_VISUALIZER_STREAM_PORT", 47329);
    int socketFileDescriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (socketFileDescriptor < 0)
    {
        return -1;
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = 0;
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1)
    {
        close(socketFileDescriptor);
        return -1;
    }

    if (connect(socketFileDescriptor, (struct sockaddr*)&address, sizeof(address)) != 0)
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
static struct timespec StreamingIntervalFromNow(void)
{
    struct timespec timeout = {.tv_sec = 0, .tv_nsec = 0};
    clock_gettime(CLOCK_REALTIME, &timeout);

    timeout.tv_nsec += StreamingIntervalNanoseconds;
    if (timeout.tv_nsec >= 1000000000)
    {
        timeout.tv_nsec -= 1000000000;
        timeout.tv_sec++;
    }

    return timeout;
}


//------------------------------------------------------------------------------
// StreamingThreadMain
//------------------------------------------------------------------------------
//  Periodically connect to the visualizer and send allocation snapshots.
//------------------------------------------------------------------------------
static void* StreamingThreadMain(void* arg)
{
    (void)arg;

    int fd = -1;

    while (true)
    {
        if (fd < 0)
        {
            fd = ConnectToVisualizer();
        }

        if (fd >= 0)
        {
            if (!SendSerializedAllocations(fd))
            {
                close(fd);
                fd = -1;
            }
        }

        // Sleep until told to stop or time to try sending again.
        const struct timespec timeout = StreamingIntervalFromNow();
        const int waitResult = pthread_mutex_timedlock(&gStreamingWaitMutex, &timeout);
        if (waitResult == ETIMEDOUT)
        {
            continue;
        }
        if (waitResult == 0)
        {
            pthread_mutex_unlock(&gStreamingWaitMutex);
            break;
        }
    }

    if (fd >= 0)
    {
        // Do one final send if still open.
        (void)SendSerializedAllocations(fd);
        close(fd);
    }

    return NULL;
}


//------------------------------------------------------------------------------
// StartStreamingThread
//------------------------------------------------------------------------------
//  Start the allocation snapshot sender thread.
//------------------------------------------------------------------------------
static void StartStreamingThread(void)
{
    pthread_mutex_lock(&gStreamingWaitMutex);

    if (pthread_create(&gStreamingThread, NULL, StreamingThreadMain, NULL) != 0)
    {
        pthread_mutex_unlock(&gStreamingWaitMutex);
    }
}


//------------------------------------------------------------------------------
// StopStreamingThread
//------------------------------------------------------------------------------
//  Stop the allocation snapshot sender thread.
//------------------------------------------------------------------------------
static void StopStreamingThread(void)
{
    pthread_mutex_unlock(&gStreamingWaitMutex);
    pthread_join(gStreamingThread, NULL);
}


//------------------------------------------------------------------------------
// HashPointer
//------------------------------------------------------------------------------
//  Convert an allocation address into a stable tracking-table index.
//------------------------------------------------------------------------------
static size_t HashPointer(const void* pointer, size_t capacity)
{
    const uintptr_t pointerValue = (uintptr_t)pointer;
    const size_t mixedValue = (size_t)(pointerValue >> 4U) ^ (size_t)(pointerValue >> 32U);
    return (capacity == 0) ? 0 : (mixedValue % capacity);
}


//------------------------------------------------------------------------------
// PreviousRecordIndex
//------------------------------------------------------------------------------
//  Return the previous circular record index in an allocation pool.
//------------------------------------------------------------------------------
static size_t PreviousRecordIndex(const AllocationPool* pool, size_t recordIndex)
{
    return (recordIndex == 0) ? (pool->mCapacity - 1) : (recordIndex - 1);
}


//------------------------------------------------------------------------------
// CleanupTrailingTombstones
//------------------------------------------------------------------------------
//  Clear tombstones that no longer protect a live open-addressing probe chain.
//------------------------------------------------------------------------------
static void CleanupTrailingTombstones(size_t deletedRecordIndex)
{
    size_t recordIndex = (deletedRecordIndex + 1) % gAllocationPool.mCapacity;
    for (size_t offset = 0; offset < gAllocationPool.mCapacity; offset++)
    {
        const AllocationRecord* record = &gAllocationPool.mRecords[recordIndex];
        if ((record->mAddress != NULL) && (record->mAddress != TombstoneAddress))
        {
            return;
        }
        if (record->mAddress == NULL)
        {
            break;
        }
        recordIndex = (recordIndex + 1) % gAllocationPool.mCapacity;
    }

    recordIndex = deletedRecordIndex;
    for (size_t offset = 0; offset < gAllocationPool.mCapacity; offset++)
    {
        AllocationRecord* record = &gAllocationPool.mRecords[recordIndex];
        if (record->mAddress != TombstoneAddress)
        {
            break;
        }
        record->mAddress = NULL;
        recordIndex = PreviousRecordIndex(&gAllocationPool, recordIndex);
    }
}


//------------------------------------------------------------------------------
// TrackAllocation
//------------------------------------------------------------------------------
//  Track a single allocation in the fixed-size tracking pool.
//------------------------------------------------------------------------------
void TrackAllocation(void* pointer, size_t size, AllocationType type)
{
    if ((pointer == NULL) || (gAllocationPool.mRecords == NULL) || (gAllocationPool.mCapacity == 0) || (!IsValidOperationType(type)))
    {
        return;
    }

    LockPool(&gAllocationPool);

    size_t startIndex = HashPointer(pointer, gAllocationPool.mCapacity);
    for (size_t offset = 0; offset < gAllocationPool.mCapacity; offset++)
    {
        const size_t recordIndex = (startIndex + offset) % gAllocationPool.mCapacity;
        AllocationRecord* record = &gAllocationPool.mRecords[recordIndex];

        if ((record->mAddress == NULL) || (record->mAddress == TombstoneAddress))
        {
            record->mAddress = pointer;
            record->mSize = size;
            record->mType = type;
            gAllocationPool.mAllocationCount++;
            gAllocationPool.mTrackedAllocations++;
            gAllocationPool.mTrackedAllocationsByType[OperationTypeIndex(type)]++;
            gAllocationPool.mAllocationBytes += size;
            UnlockPool(&gAllocationPool);
            return;
        }
    }

    gAllocationPool.mUntrackedAllocations++;
    UnlockPool(&gAllocationPool);
}


//------------------------------------------------------------------------------
// UntrackAllocation
//------------------------------------------------------------------------------
//  Remove an allocation from the allocation pool.
//------------------------------------------------------------------------------
void UntrackAllocation(void* pointer, AllocationType type)
{
    if (pointer == NULL)
    {
        return;
    }

    LockPool(&gAllocationPool);

    if ((gAllocationPool.mRecords == NULL) || (gAllocationPool.mCapacity == 0) || (!IsValidOperationType(type)))
    {
        gAllocationPool.mUntrackedDeallocations++;
        UnlockPool(&gAllocationPool);
        return;
    }

    size_t startIndex = HashPointer(pointer, gAllocationPool.mCapacity);
    for (size_t offset = 0; offset < gAllocationPool.mCapacity; offset++)
    {
        const size_t recordIndex = (startIndex + offset) % gAllocationPool.mCapacity;
        AllocationRecord* record = &gAllocationPool.mRecords[recordIndex];

        if (record->mAddress == pointer)
        {
            gAllocationPool.mAllocationBytes -= record->mSize;
            gAllocationPool.mAllocationCount--;
            gAllocationPool.mTrackedDeallocations++;
            gAllocationPool.mTrackedDeallocationsByType[OperationTypeIndex(type)]++;
            record->mAddress = TombstoneAddress;
            record->mSize = 0;
            record->mType = AllocationType_Malloc;
            CleanupTrailingTombstones(recordIndex);
            UnlockPool(&gAllocationPool);
            return;
        }

        if (record->mAddress == NULL)
        {
            break;
        }
    }

    gAllocationPool.mUntrackedDeallocations++;
    UnlockPool(&gAllocationPool);
}


//------------------------------------------------------------------------------
// OnLibraryLoaded
//------------------------------------------------------------------------------
//  Start streaming allocation snapshots when the preload hook library loads.
//------------------------------------------------------------------------------
__attribute__((constructor)) static void OnLibraryLoaded(void)
{
    gSystemPageSize = ReadSystemPageSize();
    StartStreamingThread();
}


//------------------------------------------------------------------------------
// OnLibraryUnloaded
//------------------------------------------------------------------------------
//  Stop streaming allocation snapshots before the preload hook library unloads.
//------------------------------------------------------------------------------
__attribute__((destructor)) static void OnLibraryUnloaded(void)
{
    StopStreamingThread();
}
