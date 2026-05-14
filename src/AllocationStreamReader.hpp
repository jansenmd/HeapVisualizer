#pragma once

#include "Preload/TrackAllocations.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

constexpr std::size_t StreamedTrackedAllocationTypeCount =
    static_cast<std::size_t>(HeapVisualizer::AllocationHooks::TrackedAllocationType::Count);
constexpr std::size_t StreamedTrackedDeallocationTypeCount =
    static_cast<std::size_t>(HeapVisualizer::AllocationHooks::TrackedDeallocationType::Count);

struct StreamedAllocationBin
{
    // Bin min and max range.
    std::uint64_t mMinimumSize = 0;
    std::uint64_t mMaximumSize = 0;
    // Total number of bytes within the bin.
    std::uint64_t mAllocationBytes = 0;
    // Minimum, maxmium, mean, and median allocations within the bin.
    std::uint64_t mMinimumAllocationSize = 0;
    std::uint64_t mMaximumAllocationSize = 0;
    std::uint64_t mMeanAllocationSize = 0;
    std::uint64_t mMedianAllocationSize = 0;
    // Suppress showing the total bytes or number of allocations for this bin.
    bool mSuppressed = false;
    std::vector<std::size_t> mAllocationSizes;
};

struct StreamedAllocation
{
    std::uintptr_t mAddress = 0;
    std::uint64_t mSize = 0;
    HeapVisualizer::AllocationHooks::TrackedAllocationType mType =
        HeapVisualizer::AllocationHooks::TrackedAllocationType::Malloc;
};

struct StreamedHeapSnapshot
{
    std::vector<StreamedAllocation> mAllocations;
    std::uint64_t mAllocationCount = 0;
    std::uint64_t mAllocationBytes = 0;
    std::uint64_t mPageSize = 4096;
    bool mHasPageSize = false;
    std::uint64_t mTrackedAllocations = 0;
    std::uint64_t mTrackedDeallocations = 0;
    std::uint64_t mUntrackedAllocations = 0;
    std::uint64_t mUntrackedDeallocations = 0;
    std::array<std::uint64_t, StreamedTrackedAllocationTypeCount> mTrackedAllocationsByType = {};
    std::array<std::uint64_t, StreamedTrackedDeallocationTypeCount> mTrackedDeallocationsByType = {};
};

struct StreamedAddressRange
{
    bool mHasAllocations = false;
    std::uintptr_t mLowestAddress = 0;
    std::uintptr_t mHighestAddress = 0;
};

struct StreamedAllocationGroup
{
    std::size_t mFirstAllocationIndex = 0;
    std::size_t mAllocationCount = 0;
    std::uintptr_t mStartAddress = 0;
    std::uintptr_t mEndAddress = 0;
};

struct StreamedAllocationSummary
{
    std::uint64_t mStreamPayloadSize = 0;
    std::uint64_t mSocketReadMicroseconds = 0;
    std::uint64_t mParserProcessingMicroseconds = 0;
    std::uint64_t mAllocationCount = 0;
    std::uint64_t mAllocationBytes = 0;
    std::uint64_t mPageSize = 4096;
    bool mHasPageSize = false;
    std::uint64_t mAllocatedPages = 0;
    StreamedAddressRange mAddressRange;
    std::uint64_t mTrackedAllocations = 0;
    std::uint64_t mTrackedDeallocations = 0;
    std::uint64_t mUntrackedAllocations = 0;
    std::uint64_t mUntrackedDeallocations = 0;
    std::array<std::uint64_t, StreamedTrackedAllocationTypeCount> mTrackedAllocationsByType = {};
    std::array<std::uint64_t, StreamedTrackedDeallocationTypeCount> mTrackedDeallocationsByType = {};
    std::vector<StreamedAllocationBin> mBins;
    std::vector<StreamedAllocationGroup> mAllocationGroups;
    std::vector<std::vector<uint32_t>> mHeatmaps;
};


//------------------------------------------------------------------------------
// AllocationStreamReader
//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
class AllocationStreamReader
{
public:
    using SummaryHandler = std::function<void(const StreamedAllocationSummary& summary)>;

    // Constructor and Destructor
    AllocationStreamReader();
    ~AllocationStreamReader();

    // Disallow copying an instance of this class.
    AllocationStreamReader(const AllocationStreamReader&) = delete;
    AllocationStreamReader& operator=(const AllocationStreamReader&) = delete;

    // Set the callback to display the latest summary in the UI.
    void SetSummaryHandler(SummaryHandler handler);

    // Start and stop streaming.
    bool Start(const std::string& address, std::uint16_t port, std::string& errorMessage);
    void Stop();
    bool IsActive() const;

private:
    [[nodiscard]] static inline bool IsValidTrackedAllocationType(
        HeapVisualizer::AllocationHooks::TrackedAllocationType type);
    static void ConsumeBufferedData(std::vector<std::uint8_t>& bufferedData,
                                    std::size_t& bufferedDataSize,
                                    std::size_t bytesToConsume);
    [[nodiscard]] static bool ReadUInt32(std::span<const std::uint8_t> data,
                                         std::size_t& offset,
                                         std::uint32_t& value);
    [[nodiscard]] static bool ReadUInt64(std::span<const std::uint8_t> data,
                                         std::size_t& offset,
                                         std::uint64_t& value);
    [[nodiscard]] static std::string LastSocketError();
    [[nodiscard]] static bool InitializeSockets(std::string& errorMessage);
    static void ShutdownSockets();
    static void CloseSocket(std::uintptr_t socketValue);
    static void ShutdownSocket(std::uintptr_t socketValue);
    [[nodiscard]] static std::uintptr_t CreateListenSocket(const std::string& addressText,
                                                           std::uint16_t port,
                                                           std::string& errorMessage);

    void StreamingThreadMain(std::uintptr_t listenSocket);
    void ReceiveSocketFrame(std::uintptr_t clientSocket);
    void ParseStreamingData(std::span<const std::uint8_t> data, std::uint64_t socketReadMicroseconds);
    void AddAllocationToBin(std::vector<StreamedAllocationBin>& bins, const StreamedAllocation& allocation) const;
    void CalculateBinStatistics(StreamedAllocationBin& bin) const;
    [[nodiscard]] StreamedAllocationSummary BuildAllocationSummary(const StreamedHeapSnapshot& snapshot) const;

    SummaryHandler mSummaryHandler;
    std::atomic_bool mStreamingActive = false;
    std::atomic_uintptr_t mStreamingListenSocket = static_cast<std::uintptr_t>(-1);
    std::atomic_uintptr_t mStreamingClientSocket = static_cast<std::uintptr_t>(-1);
    std::thread mStreamingThread;
};
