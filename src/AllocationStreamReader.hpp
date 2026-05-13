#pragma once

#include "Preload/TrackAllocations.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket InvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket InvalidSocket = -1;
#endif

struct StreamedAllocationBin
{
    std::size_t mMinimumSize = 0;
    std::size_t mMaximumSize = 0;
    std::uint64_t mAllocationCount = 0;
    std::uint64_t mAllocationBytes = 0;
    std::size_t mMinimumAllocationSize = 0;
    std::size_t mMaximumAllocationSize = 0;
    bool mSuppressed = false;
    std::vector<std::size_t> mAllocationSizes;
};

struct StreamedAllocation
{
    std::uintptr_t mAddress = 0;
    std::size_t mSize = 0;
    HeapVisualizer::AllocationHooks::AllocationType mType =
        HeapVisualizer::AllocationHooks::AllocationType::Malloc;
};

struct StreamedHeapSnapshot
{
    std::vector<StreamedAllocation> mAllocations;
    std::size_t mAllocationCount = 0;
    std::size_t mAllocationBytes = 0;
    std::size_t mPageSize = 4096;
    bool mHasPageSize = false;
    std::uint64_t mTrackedAllocations = 0;
    std::uint64_t mTrackedDeallocations = 0;
    std::uint64_t mUntrackedAllocations = 0;
    std::uint64_t mUntrackedDeallocations = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(HeapVisualizer::AllocationHooks::AllocationType::Count)>
        mTrackedAllocationsByType = {};
    std::array<std::uint64_t, static_cast<std::size_t>(HeapVisualizer::AllocationHooks::AllocationType::Count)>
        mTrackedDeallocationsByType = {};
};

struct StreamedAddressRange
{
    bool mHasAllocations = false;
    std::uintptr_t mLowestAddress = 0;
    std::uintptr_t mHighestAddress = 0;
    std::size_t mHighestAddressAllocationSize = 0;
};

struct StreamedAllocationSummary
{
    std::size_t mStreamPayloadSize = 0;
    std::size_t mAllocationCount = 0;
    std::size_t mAllocationBytes = 0;
    std::size_t mPageSize = 4096;
    bool mHasPageSize = false;
    std::size_t mAllocatedPages = 0;
    StreamedAddressRange mAddressRange;
    std::uint64_t mTrackedAllocations = 0;
    std::uint64_t mTrackedDeallocations = 0;
    std::uint64_t mUntrackedAllocations = 0;
    std::uint64_t mUntrackedDeallocations = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(HeapVisualizer::AllocationHooks::AllocationType::Count)>
        mTrackedAllocationsByType = {};
    std::array<std::uint64_t, static_cast<std::size_t>(HeapVisualizer::AllocationHooks::AllocationType::Count)>
        mTrackedDeallocationsByType = {};
    std::vector<StreamedAllocationBin> mBins;
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
    void StreamingThreadMain(std::uintptr_t listenSocket);
    void ReceiveSocketFrame(NativeSocket clientSocket);
    void ParseStreamingData(std::span<const std::uint8_t> data);
    void UpdateAddressRange(StreamedAddressRange& range, const StreamedAllocation& allocation) const;
    void AddAllocationToBin(std::vector<StreamedAllocationBin>& bins, const StreamedAllocation& allocation) const;
    [[nodiscard]] StreamedAllocationSummary BuildAllocationSummary(const StreamedHeapSnapshot& snapshot) const;

    SummaryHandler mSummaryHandler;
    std::atomic_bool mStreamingActive = false;
    std::atomic_uintptr_t mStreamingListenSocket = static_cast<std::uintptr_t>(-1);
    std::atomic_uintptr_t mStreamingClientSocket = static_cast<std::uintptr_t>(-1);
    std::thread mStreamingThread;
    std::mutex mStreamedSnapshotsMutex;
    std::vector<StreamedHeapSnapshot> mStreamedSnapshots;
};
