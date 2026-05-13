#include "AllocationStreamReader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket InvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket InvalidSocket = -1;
#endif

constexpr std::array<std::uint8_t, 8> SnapshotMagic = {'H', 'V', 'I', 'Z', 'A', 'L', 'L', 'C'};
constexpr std::array<std::uint8_t, 8> SnapshotFooter = {'H', 'V', 'I', 'Z', 'D', 'O', 'N', 'E'};
constexpr std::size_t SocketReadChunkSize = 0x10000; // 64kB
constexpr std::size_t DefaultAllocationPageSize = 4096;
constexpr std::size_t SnapshotLengthSize = sizeof(std::uint64_t);
constexpr std::size_t MinimumSnapshotFrameSize =
    SnapshotMagic.size() + SnapshotLengthSize + SnapshotFooter.size();
constexpr std::size_t BytePatternNotFound = std::numeric_limits<std::size_t>::max();
constexpr std::size_t TrackedAllocationTypeCount =
    static_cast<std::size_t>(HeapVisualizer::AllocationHooks::TrackedAllocationType::Count);
static_assert(StreamedTrackedAllocationTypeCount == TrackedAllocationTypeCount);
static_assert(StreamedTrackedDeallocationTypeCount ==
    static_cast<std::size_t>(HeapVisualizer::AllocationHooks::TrackedDeallocationType::Count));


}


//------------------------------------------------------------------------------
// IsValidTrackedAllocationType
//------------------------------------------------------------------------------
//  Return whether a tracked allocation type is inside the known range.
//------------------------------------------------------------------------------
bool inline AllocationStreamReader::IsValidTrackedAllocationType(HeapVisualizer::AllocationHooks::TrackedAllocationType type)
{
    return static_cast<std::size_t>(type) < TrackedAllocationTypeCount;
}


//------------------------------------------------------------------------------
// ConsumeBufferedData
//------------------------------------------------------------------------------
//  Remove consumed bytes from the front of the valid buffered data range.
//------------------------------------------------------------------------------
void AllocationStreamReader::ConsumeBufferedData(std::vector<std::uint8_t>& bufferedData,
                                                 std::size_t& bufferedDataSize,
                                                 std::size_t bytesToConsume)
{
    if (bytesToConsume >= bufferedDataSize)
    {
        bufferedDataSize = 0;
        return;
    }

    std::move(
        bufferedData.begin() + static_cast<std::ptrdiff_t>(bytesToConsume),
        bufferedData.begin() + static_cast<std::ptrdiff_t>(bufferedDataSize),
        bufferedData.begin());
    bufferedDataSize -= bytesToConsume;
}


//------------------------------------------------------------------------------
// ReadUInt32
//------------------------------------------------------------------------------
//  Read a 32-bit little-endian unsigned integer from a binary buffer.
//------------------------------------------------------------------------------
bool AllocationStreamReader::ReadUInt32(std::span<const std::uint8_t> data,
                                        std::size_t& offset,
                                        std::uint32_t& value)
{
    if ((offset > data.size()) || ((data.size() - offset) < sizeof(std::uint32_t)))
    {
        return false;
    }

    value = (static_cast<std::uint32_t>(data[offset + 0]) <<  0) |
            (static_cast<std::uint32_t>(data[offset + 1]) <<  8) |
            (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
            (static_cast<std::uint32_t>(data[offset + 3]) << 24);
    offset += sizeof(std::uint32_t);
    return true;
}


//------------------------------------------------------------------------------
// ReadUInt64
//------------------------------------------------------------------------------
//  Read a 64-bit little-endian unsigned integer from a binary buffer.
//------------------------------------------------------------------------------
bool AllocationStreamReader::ReadUInt64(std::span<const std::uint8_t> data,
                                        std::size_t& offset,
                                        std::uint64_t& value)
{
    if ((offset > data.size()) || ((data.size() - offset) < sizeof(std::uint64_t)))
    {
        return false;
    }

    value = 0;
    for (std::size_t byteIndex = 0; byteIndex < sizeof(std::uint64_t); byteIndex++)
    {
        value |= static_cast<std::uint64_t>(data[offset + byteIndex]) << (byteIndex * 8);
    }

    offset += sizeof(std::uint64_t);
    return true;
}


//------------------------------------------------------------------------------
// LastSocketError
//------------------------------------------------------------------------------
//  Return a portable string for the last socket error.
//------------------------------------------------------------------------------
std::string AllocationStreamReader::LastSocketError()
{
#ifdef _WIN32
    return std::system_category().message(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}


//------------------------------------------------------------------------------
// InitializeSockets
//------------------------------------------------------------------------------
//  Initialize the platform socket subsystem.
//------------------------------------------------------------------------------
bool AllocationStreamReader::InitializeSockets(std::string& errorMessage)
{
#ifdef _WIN32
    WSADATA data = {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        errorMessage = LastSocketError();
        return false;
    }
#else
    static_cast<void>(errorMessage);
#endif

    return true;
}


//------------------------------------------------------------------------------
// ShutdownSockets
//------------------------------------------------------------------------------
//  Shut down the platform socket subsystem.
//------------------------------------------------------------------------------
void AllocationStreamReader::ShutdownSockets()
{
#ifdef _WIN32
    WSACleanup();
#endif
}


//------------------------------------------------------------------------------
// CloseSocket
//------------------------------------------------------------------------------
//  Close a native socket handle.
//------------------------------------------------------------------------------
void AllocationStreamReader::CloseSocket(std::uintptr_t socketStorage)
{
    const NativeSocket socketValue = static_cast<NativeSocket>(socketStorage);
    if (socketValue == InvalidSocket)
    {
        return;
    }

#ifdef _WIN32
    closesocket(socketValue);
#else
    close(socketValue);
#endif
}


//------------------------------------------------------------------------------
// ShutdownSocket
//------------------------------------------------------------------------------
//  Shut down socket operations to wake blocking accept or receive calls.
//------------------------------------------------------------------------------
void AllocationStreamReader::ShutdownSocket(std::uintptr_t socketStorage)
{
    const NativeSocket socketValue = static_cast<NativeSocket>(socketStorage);
    if (socketValue == InvalidSocket)
    {
        return;
    }

#ifdef _WIN32
    shutdown(socketValue, SD_BOTH);
#else
    shutdown(socketValue, SHUT_RDWR);
#endif
}


//------------------------------------------------------------------------------
// CreateListenSocket
//------------------------------------------------------------------------------
//  Create, bind, and listen on a native TCP socket.
//------------------------------------------------------------------------------
std::uintptr_t AllocationStreamReader::CreateListenSocket(const std::string& addressText,
                                                          std::uint16_t port,
                                                          std::string& errorMessage)
{
    NativeSocket listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket == InvalidSocket)
    {
        errorMessage = LastSocketError();
        return static_cast<std::uintptr_t>(InvalidSocket);
    }

    int reuseAddress = 1;
    setsockopt(
        listenSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuseAddress),
        sizeof(reuseAddress));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, addressText.c_str(), &address.sin_addr) != 1)
    {
        errorMessage = "Invalid IPv4 address.";
        CloseSocket(static_cast<std::uintptr_t>(listenSocket));
        return static_cast<std::uintptr_t>(InvalidSocket);
    }

    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        errorMessage = LastSocketError();
        CloseSocket(static_cast<std::uintptr_t>(listenSocket));
        return static_cast<std::uintptr_t>(InvalidSocket);
    }

    if (listen(listenSocket, 8) != 0)
    {
        errorMessage = LastSocketError();
        CloseSocket(static_cast<std::uintptr_t>(listenSocket));
        return static_cast<std::uintptr_t>(InvalidSocket);
    }

    return static_cast<std::uintptr_t>(listenSocket);
}


//------------------------------------------------------------------------------
// AllocationStreamReader
//------------------------------------------------------------------------------
//  AllocationStreamReader constructor.  Does nothing for now.
//------------------------------------------------------------------------------
AllocationStreamReader::AllocationStreamReader()
{

}


//------------------------------------------------------------------------------
// ~AllocationStreamReader
//------------------------------------------------------------------------------
//  AllocationStreamReader destructor.
//  Stop background streaming before destroying the reader.
//------------------------------------------------------------------------------
AllocationStreamReader::~AllocationStreamReader()
{
    Stop();
}


//------------------------------------------------------------------------------
// SetSummaryHandler
//------------------------------------------------------------------------------
//  Set the callback invoked whenever a stream frame produces a summary.
//------------------------------------------------------------------------------
void AllocationStreamReader::SetSummaryHandler(SummaryHandler handler)
{
    mSummaryHandler = std::move(handler);
}


//------------------------------------------------------------------------------
// Start
//------------------------------------------------------------------------------
//  Open a native listener socket and start the streaming listener thread.
//------------------------------------------------------------------------------
bool AllocationStreamReader::Start(const std::string& address, std::uint16_t port, std::string& errorMessage)
{
    // If we're already started, just return true.
    if (mStreamingActive.load(std::memory_order_acquire))
    {
        return true;
    }

    // Initialize sockets (only matters on WIN32).
    if (!InitializeSockets(errorMessage))
    {
        return false;
    }

    // Create the socket.
    const std::uintptr_t listenSocket = CreateListenSocket(address, port, errorMessage);
    if (listenSocket == static_cast<std::uintptr_t>(InvalidSocket))
    {
        ShutdownSockets();
        return false;
    }
    mStreamingListenSocket.store(listenSocket, std::memory_order_release);
    mStreamingClientSocket.store(static_cast<std::uintptr_t>(InvalidSocket), std::memory_order_release);

    // Record that we're now streaming.
    mStreamingActive.store(true, std::memory_order_release);

    // Create the thread to read from the socket and parse the data.
    mStreamingThread = std::thread([this, listenSocket]
    {
        StreamingThreadMain(listenSocket);
    });

    return true;
}


//------------------------------------------------------------------------------
// Stop
//------------------------------------------------------------------------------
//  Stop listening for heap allocation stream data.
//------------------------------------------------------------------------------
void AllocationStreamReader::Stop()
{
    // Indicate that the thread is stopping/stopped.
    // Immediately return if it was already stopped.
    if (!mStreamingActive.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    // Close the listening socket first so the reader cannot accept a new client
    // after the current client socket is interrupted.
    const std::uintptr_t listenSocket =
        mStreamingListenSocket.exchange(static_cast<std::uintptr_t>(InvalidSocket), std::memory_order_acq_rel);
    ShutdownSocket(listenSocket);
    CloseSocket(listenSocket);

    const std::uintptr_t clientSocket =
        mStreamingClientSocket.exchange(static_cast<std::uintptr_t>(InvalidSocket), std::memory_order_acq_rel);
    ShutdownSocket(clientSocket);
    CloseSocket(clientSocket);

    // Wait for the thread to join.
    if (mStreamingThread.joinable())
    {
        mStreamingThread.join();
    }

    // WIN32 cleanup.
    ShutdownSockets();
}


//------------------------------------------------------------------------------
// IsActive
//------------------------------------------------------------------------------
//  Return whether the stream reader is currently listening.
//------------------------------------------------------------------------------
bool AllocationStreamReader::IsActive() const
{
    return mStreamingActive.load(std::memory_order_acquire);
}


//------------------------------------------------------------------------------
// ReceiveSocketFrame
//------------------------------------------------------------------------------
//  Read and parse serialized data from a socket.
//------------------------------------------------------------------------------
void AllocationStreamReader::ReceiveSocketFrame(std::uintptr_t clientSocketValue)
{
    const NativeSocket clientSocket = static_cast<NativeSocket>(clientSocketValue);
    std::vector<std::uint8_t> bufferedData;
    std::size_t bufferedDataSize = 0;

    // Read from the socket until an error occurs or it is shut down.
    while (true)
    {
        // If there's less than SocketReadChunkSize bytes available to read
        // into in the buffer, grow it by an additional SocketReadChunkSize
        // bytes.
        if ((bufferedData.size() - bufferedDataSize) < SocketReadChunkSize)
        {
            bufferedData.resize(bufferedData.size() + SocketReadChunkSize);
        }

        // Read the data from the socket.
#ifdef _WIN32
        const int bytesRead = recv(clientSocket, reinterpret_cast<char*>(bufferedData.data() + bufferedDataSize), static_cast<int>(bufferedData.size() - bufferedDataSize), 0);
#else
        const ssize_t bytesRead = recv(clientSocket, bufferedData.data() + bufferedDataSize, bufferedData.size() - bufferedDataSize, 0);
#endif
        // Something is wrong with the socket, time to return.
        if (bytesRead <= 0)
        {
            return;
        }
        // Update how much data is in the buffer.
        bufferedDataSize += static_cast<std::size_t>(bytesRead);

        // Must at least have magic to parse.
        if (bufferedDataSize < SnapshotMagic.size())
        {
            continue;
        }

        // Look for the header, throwing away any data that does not start with the Snapshot magic.
        bool foundHeader = false;
        for (size_t i = 0; i <= bufferedDataSize - SnapshotMagic.size(); i++)
        {
            if (memcmp(bufferedData.data() + i, SnapshotMagic.data(), SnapshotMagic.size()) == 0)
            {
                // Found the header.  If it's not at the beginning, throw away any extra data.
                if (i != 0)
                {
                    ConsumeBufferedData(bufferedData, bufferedDataSize, i);
                }
                foundHeader = true;
                break;
            }
        }
        // If the header wasn't found, throw away all but the last few bytes
        // that could still be part of the (incomplete) header.
        if (!foundHeader)
        {
            ConsumeBufferedData(bufferedData, bufferedDataSize, bufferedDataSize - (SnapshotMagic.size() - 1));
            continue;
        }

        // Enough data for the length?
        if (bufferedDataSize < SnapshotMagic.size() + SnapshotLengthSize)
        {
            continue;
        }
        // Get the length.
        std::uint64_t serializedFrameSize = 0;
        std::size_t sizeOffset = SnapshotMagic.size();
        if (!ReadUInt64(std::span<const std::uint8_t>(bufferedData.data(), bufferedDataSize), sizeOffset, serializedFrameSize))
        {
            // Clear the buffer and try again.
            bufferedDataSize = 0;
            continue;
        }
        if (serializedFrameSize < MinimumSnapshotFrameSize)
        {
            // Clear the buffer and try again.
            bufferedDataSize = 0;
            continue;
        }

        const std::size_t frameSize = static_cast<std::size_t>(serializedFrameSize);
        if (bufferedDataSize >= frameSize)
        {
            ParseStreamingData(std::span<const std::uint8_t>(bufferedData.data(), frameSize));
            ConsumeBufferedData(bufferedData, bufferedDataSize, frameSize);
        }
    }
}


//------------------------------------------------------------------------------
// StreamingThreadMain
//------------------------------------------------------------------------------
//  Accept native socket connections and parse streamed allocation frames.
//------------------------------------------------------------------------------
void AllocationStreamReader::StreamingThreadMain(std::uintptr_t listenSocketValue)
{
    const NativeSocket listenSocket = static_cast<NativeSocket>(listenSocketValue);

    // Check to see if it's time to stop.
    while (mStreamingActive.load(std::memory_order_acquire))
    {
        // Wait for new connections.
        const NativeSocket clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == InvalidSocket)
        {
            // When stopping, mStreamingActive will be set to false and the
            // listen socket will get shut down.
            continue;
        }

        // Store the new socket so we can stop it by shutting down this socket.
        mStreamingClientSocket.store(static_cast<std::uintptr_t>(clientSocket), std::memory_order_release);

        // Read data until the socket fails to read (either due to an error,
        // the client disconnecting, or the thread stopping and the socket
        // being shut down).
        ReceiveSocketFrame(static_cast<std::uintptr_t>(clientSocket));

        const std::uintptr_t storedClientSocket =
            mStreamingClientSocket.exchange(static_cast<std::uintptr_t>(InvalidSocket), std::memory_order_acq_rel);
        if (storedClientSocket != static_cast<std::uintptr_t>(InvalidSocket))
        {
            CloseSocket(storedClientSocket);
        }
    }
}


//------------------------------------------------------------------------------
// ParseStreamingData
//------------------------------------------------------------------------------
//  Parse streamed heap allocation data into internal snapshot structures.
//------------------------------------------------------------------------------
void AllocationStreamReader::ParseStreamingData(std::span<const std::uint8_t> data)
{
    // Sanity check on the given data buffer.
    if ((data.size() < SnapshotMagic.size()) ||
        (std::memcmp(data.data(), SnapshotMagic.data(), SnapshotMagic.size()) != 0))
    {
        // Invalid input!
        return;
    }

    // Skip the magic header.
    std::size_t offset = SnapshotMagic.size();

    // Extract the size of the entire frame and validate the size of the buffer.
    std::uint64_t serializedFrameSize = 0;
    if ((!ReadUInt64(data, offset, serializedFrameSize)) ||
        (serializedFrameSize != static_cast<std::uint64_t>(data.size())))
    {
        // Invalid size!
        return;
    }


    // Extract the various counts and page size.
    StreamedHeapSnapshot snapshot;
    if ((!ReadUInt64(data, offset, snapshot.mAllocationCount)) ||
        (!ReadUInt64(data, offset, snapshot.mAllocationBytes)) ||
        (!ReadUInt64(data, offset, snapshot.mPageSize)) ||
        (!ReadUInt64(data, offset, snapshot.mTrackedAllocations)) ||
        (!ReadUInt64(data, offset, snapshot.mTrackedDeallocations)) ||
        (!ReadUInt64(data, offset, snapshot.mUntrackedAllocations)) ||
        (!ReadUInt64(data, offset, snapshot.mUntrackedDeallocations)))
    {
        return;
    }

    // Track the page size stats.
    if (snapshot.mPageSize > 0)
    {
        snapshot.mHasPageSize = true;
    }
    else
    {
        snapshot.mHasPageSize = false;
        snapshot.mPageSize = DefaultAllocationPageSize;
    }

    // Extract the number of each type of allocation.
    for (std::size_t typeIndex = 0; typeIndex < StreamedTrackedAllocationTypeCount; typeIndex++)
    {
        std::uint64_t typeCount = 0;
        if (!ReadUInt64(data, offset, typeCount))
        {
            return;
        }
        snapshot.mTrackedAllocationsByType[typeIndex] = static_cast<std::uint64_t>(typeCount);
    }

    // Extract the number of each type of deallocation.
    for (std::size_t typeIndex = 0; typeIndex < StreamedTrackedDeallocationTypeCount; typeIndex++)
    {
        std::uint64_t typeCount = 0;
        if (!ReadUInt64(data, offset, typeCount))
        {
            return;
        }
        snapshot.mTrackedDeallocationsByType[typeIndex] = static_cast<std::uint64_t>(typeCount);
    }

    // Extract every live allocation.
    // Reserve the total number of allocations so the push backs don't have to
    // grow the vector.
    snapshot.mAllocations.reserve(snapshot.mAllocationCount);
    std::uint64_t address = 0;
    std::uint64_t size = 0;
    std::uint32_t typeValue = 0;
    for (std::uint64_t allocationIndex = 0; allocationIndex < snapshot.mAllocationCount; allocationIndex++)
    {
        if ((!ReadUInt64(data, offset, address)) ||
            (!ReadUInt64(data, offset, size)) ||
            (!ReadUInt32(data, offset, typeValue)))
        {
            return;
        }
        // Sanity check on the allocation type.
        const auto type = static_cast<HeapVisualizer::AllocationHooks::TrackedAllocationType>(typeValue);
        if (!IsValidTrackedAllocationType(type))
        {
            return;
        }
        // Add this entry to the list of allocations.
        snapshot.mAllocations.push_back({static_cast<std::uintptr_t>(address), static_cast<std::size_t>(size), type});
    }

    // Make sure all the data was consumed correctly.
    if ((data.size() - offset != SnapshotFooter.size()) ||
        (std::memcmp(data.data() + offset, SnapshotFooter.data(), SnapshotFooter.size()) != 0))
    {
        return;
    }

    // Convert the snapshot into a summary we can display.
    StreamedAllocationSummary summary = BuildAllocationSummary(snapshot);
    summary.mStreamPayloadSize = data.size();

    {
        std::lock_guard lock(mStreamedSnapshotsMutex);
        mStreamedSnapshots.push_back(std::move(snapshot));
    }

    if (mSummaryHandler)
    {
        mSummaryHandler(summary);
    }
}


//------------------------------------------------------------------------------
// UpdateAddressRange
//------------------------------------------------------------------------------
//  Fold a streamed allocation into an address range summary.
//------------------------------------------------------------------------------
void AllocationStreamReader::UpdateAddressRange(StreamedAddressRange& range, const StreamedAllocation& allocation) const
{
    const std::uintptr_t allocationEndAddress = allocation.mAddress + allocation.mSize;

    if (!range.mHasAllocations)
    {
        range.mHasAllocations = true;
        range.mLowestAddress = allocation.mAddress;
        range.mHighestAddress = allocationEndAddress;
    }
    else
    {
        if (allocation.mAddress < range.mLowestAddress)
        {
            range.mLowestAddress = allocation.mAddress;
        }
        if (allocationEndAddress > range.mHighestAddress)
        {
            range.mHighestAddress = allocationEndAddress;
        }
    }
}


//------------------------------------------------------------------------------
// AddAllocationToBin
//------------------------------------------------------------------------------
//  Add a streamed allocation to the matching power-of-two size bin.
//------------------------------------------------------------------------------
void AllocationStreamReader::AddAllocationToBin(std::vector<StreamedAllocationBin>& bins,
                                                const StreamedAllocation& allocation) const
{
    // Make sure the bins vector has a bin for the current allocation.
    while (bins.empty() || (bins.back().mMaximumSize < allocation.mSize))
    {
        // Grow the number of bin entries until a bin that fits the current
        // allocation exists.
        // Binning starts with the range 0-64 bytes.
        // After that, it goes up by powers of 2.
        // So, 65-128, 129-256, etc.
        const std::size_t nextMinimumSize = bins.empty() ? 0 : bins.back().mMaximumSize + 1;
        std::size_t nextMaximumSize = bins.empty() ? 64 :
            (bins.back().mMaximumSize > (std::numeric_limits<std::size_t>::max() / 2)) ? std::numeric_limits<std::size_t>::max() : bins.back().mMaximumSize * 2;

        bins.push_back({nextMinimumSize, nextMaximumSize, 0, 0, 0, 0, false, {}});
    }

    // There should be a bin that fits our allocation.
    const auto binIterator = std::find_if(
        bins.begin(),
        bins.end(),
        [allocation](const StreamedAllocationBin& bin)
        {
            return (bin.mMinimumSize <= allocation.mSize) && (bin.mMaximumSize >= allocation.mSize);
        });

    // Track the largest and smallest allocation within the bin.
    if (binIterator->mAllocationCount == 0)
    {
        binIterator->mMinimumAllocationSize = allocation.mSize;
        binIterator->mMaximumAllocationSize = allocation.mSize;
    }
    else
    {
        binIterator->mMinimumAllocationSize = std::min(binIterator->mMinimumAllocationSize, allocation.mSize);
        binIterator->mMaximumAllocationSize = std::max(binIterator->mMaximumAllocationSize, allocation.mSize);
    }

    // Track the number of allocations and total byte count within the bin.
    binIterator->mAllocationCount++;
    binIterator->mAllocationBytes += allocation.mSize;
    binIterator->mAllocationSizes.push_back(allocation.mSize);
}


//------------------------------------------------------------------------------
// BuildAllocationSummary
//------------------------------------------------------------------------------
//  Aggregate streamed allocations and pool counters into the combined summary.
//------------------------------------------------------------------------------
StreamedAllocationSummary AllocationStreamReader::BuildAllocationSummary(const StreamedHeapSnapshot& snapshot) const
{
    StreamedAllocationSummary summary;

    summary.mAllocationCount = snapshot.mAllocationCount;
    summary.mAllocationBytes = snapshot.mAllocationBytes;
    summary.mPageSize = snapshot.mPageSize;
    summary.mHasPageSize = snapshot.mHasPageSize;
    summary.mTrackedAllocations = snapshot.mTrackedAllocations;
    summary.mTrackedDeallocations = snapshot.mTrackedDeallocations;
    summary.mUntrackedAllocations = snapshot.mUntrackedAllocations;
    summary.mUntrackedDeallocations = snapshot.mUntrackedDeallocations;
    summary.mTrackedAllocationsByType = snapshot.mTrackedAllocationsByType;
    summary.mTrackedDeallocationsByType = snapshot.mTrackedDeallocationsByType;

    // Track the range of pages covered by each allocation.
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> allocatedPageRanges;
    allocatedPageRanges.reserve(snapshot.mAllocations.size());
    const std::uintptr_t pageSize = static_cast<std::uintptr_t>(summary.mPageSize);
    for (const StreamedAllocation& allocation : snapshot.mAllocations)
    {
        if (allocation.mSize > 0)
        {
            const std::uintptr_t allocationEndAddress = allocation.mAddress + allocation.mSize - 1;
            allocatedPageRanges.emplace_back(allocation.mAddress / pageSize, allocationEndAddress / pageSize);
        }

        // Track the lowest and highest used memory addresses.
        UpdateAddressRange(summary.mAddressRange, allocation);
        AddAllocationToBin(summary.mBins, allocation);
    }

    std::sort(allocatedPageRanges.begin(), allocatedPageRanges.end());

    bool hasMergedPageRange = false;
    std::uintptr_t mergedPageRangeEnd = 0;

    for (const auto& pageRange : allocatedPageRanges)
    {
        if ((!hasMergedPageRange) ||
            (pageRange.first > mergedPageRangeEnd))
        {
            hasMergedPageRange = true;
            mergedPageRangeEnd = pageRange.second;
            summary.mAllocatedPages += static_cast<std::size_t>(pageRange.second - pageRange.first + 1);
            continue;
        }

        if (pageRange.second > mergedPageRangeEnd)
        {
            summary.mAllocatedPages += static_cast<std::size_t>(pageRange.second - mergedPageRangeEnd);
            mergedPageRangeEnd = pageRange.second;
        }
    }

    return summary;
}
