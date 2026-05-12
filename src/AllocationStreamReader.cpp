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
constexpr std::size_t SocketReadChunkSize = 8192;
constexpr std::size_t DefaultAllocationPageSize = 4096;
constexpr std::size_t BytePatternNotFound = std::numeric_limits<std::size_t>::max();
constexpr std::size_t AllocationTypeCount =
    static_cast<std::size_t>(HeapVisualizer::AllocationHooks::AllocationType::Count);


//------------------------------------------------------------------------------
// OperationTypeIndex
//------------------------------------------------------------------------------
//  Convert an allocation operation type into a count-array index.
//------------------------------------------------------------------------------
std::size_t OperationTypeIndex(HeapVisualizer::AllocationHooks::AllocationType type)
{
    return static_cast<std::size_t>(type);
}


//------------------------------------------------------------------------------
// IsValidOperationType
//------------------------------------------------------------------------------
//  Return whether an allocation operation type is inside the known range.
//------------------------------------------------------------------------------
bool IsValidOperationType(HeapVisualizer::AllocationHooks::AllocationType type)
{
    return OperationTypeIndex(type) < AllocationTypeCount;
}


//------------------------------------------------------------------------------
// FindBytePattern
//------------------------------------------------------------------------------
//  Find a byte pattern in a byte buffer starting at the provided offset.
//------------------------------------------------------------------------------
std::size_t FindBytePattern(const std::vector<std::uint8_t>& data,
                            std::size_t validSize,
                            const std::array<std::uint8_t, 8>& pattern,
                            std::size_t offset)
{
    if ((validSize > data.size()) ||
        (offset > validSize) ||
        (pattern.size() > (validSize - offset)))
    {
        return BytePatternNotFound;
    }

    const auto searchBegin = data.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto searchEnd = data.begin() + static_cast<std::ptrdiff_t>(validSize);
    const auto patternIterator = std::search(searchBegin, searchEnd, pattern.begin(), pattern.end());
    if (patternIterator == searchEnd)
    {
        return BytePatternNotFound;
    }

    return static_cast<std::size_t>(std::distance(data.begin(), patternIterator));
}


//------------------------------------------------------------------------------
// ConsumeBufferedData
//------------------------------------------------------------------------------
//  Remove consumed bytes from the front of the valid buffered data range.
//------------------------------------------------------------------------------
void ConsumeBufferedData(std::vector<std::uint8_t>& bufferedData,
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
// ToNativeSocket
//------------------------------------------------------------------------------
//  Convert an integer storage value back to a native socket handle.
//------------------------------------------------------------------------------
NativeSocket ToNativeSocket(std::uintptr_t socketValue)
{
    return static_cast<NativeSocket>(socketValue);
}


//------------------------------------------------------------------------------
// FromNativeSocket
//------------------------------------------------------------------------------
//  Convert a native socket handle into integer storage.
//------------------------------------------------------------------------------
std::uintptr_t FromNativeSocket(NativeSocket socketValue)
{
    return static_cast<std::uintptr_t>(socketValue);
}


//------------------------------------------------------------------------------
// ReadUInt32
//------------------------------------------------------------------------------
//  Read a 32-bit little-endian unsigned integer from a binary buffer.
//------------------------------------------------------------------------------
bool ReadUInt32(std::span<const std::uint8_t> data, std::size_t& offset, std::uint32_t& value)
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
bool ReadUInt64(std::span<const std::uint8_t> data, std::size_t& offset, std::uint64_t& value)
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
std::string LastSocketError()
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
bool InitializeSockets(std::string& errorMessage)
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
void ShutdownSockets()
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
void CloseSocket(NativeSocket socketValue)
{
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
void ShutdownSocket(NativeSocket socketValue)
{
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
NativeSocket CreateListenSocket(const std::string& addressText, std::uint16_t port, std::string& errorMessage)
{
    NativeSocket listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket == InvalidSocket)
    {
        errorMessage = LastSocketError();
        return InvalidSocket;
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
        CloseSocket(listenSocket);
        return InvalidSocket;
    }

    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        errorMessage = LastSocketError();
        CloseSocket(listenSocket);
        return InvalidSocket;
    }

    if (listen(listenSocket, 8) != 0)
    {
        errorMessage = LastSocketError();
        CloseSocket(listenSocket);
        return InvalidSocket;
    }

    return listenSocket;
}


//------------------------------------------------------------------------------
// ReceiveSocketFrame
//------------------------------------------------------------------------------
//  Read one complete header/footer-delimited frame from a socket.
//  Returns false if the socket fails to read.
//------------------------------------------------------------------------------
bool ReceiveSocketFrame(NativeSocket socketValue,
                        std::vector<std::uint8_t>& bufferedData,
                        std::size_t& bufferedDataSize,
                        std::size_t& frameSize)
{
    frameSize = 0;

    while (true)
    {
        const std::size_t headerOffset = FindBytePattern(bufferedData, bufferedDataSize, SnapshotMagic, 0);
        if (headerOffset != BytePatternNotFound)
        {
            if (headerOffset != 0)
            {
                ConsumeBufferedData(bufferedData, bufferedDataSize, headerOffset);
            }

            const std::size_t footerOffset = FindBytePattern(
                bufferedData,
                bufferedDataSize,
                SnapshotFooter,
                SnapshotMagic.size());
            if (footerOffset != BytePatternNotFound)
            {
                frameSize = footerOffset + SnapshotFooter.size();
                return true;
            }
        }
        else if (bufferedDataSize > SnapshotMagic.size())
        {
            const std::size_t bytesToKeep = SnapshotMagic.size() - 1;
            std::move(
                bufferedData.begin() + static_cast<std::ptrdiff_t>(bufferedDataSize - bytesToKeep),
                bufferedData.begin() + static_cast<std::ptrdiff_t>(bufferedDataSize),
                bufferedData.begin());
            bufferedDataSize = bytesToKeep;
        }

        if ((bufferedData.size() - bufferedDataSize) < SocketReadChunkSize)
        {
            bufferedData.resize(bufferedDataSize + SocketReadChunkSize);
        }

#ifdef _WIN32
        const int bytesRead = recv(
            socketValue,
            reinterpret_cast<char*>(bufferedData.data() + bufferedDataSize),
            static_cast<int>(SocketReadChunkSize),
            0);
#else
        const ssize_t bytesRead = recv(socketValue, bufferedData.data() + bufferedDataSize, SocketReadChunkSize, 0);
#endif
        // Something is wrong with the socket, time to return.
        if (bytesRead <= 0)
        {
            return false;
        }

        bufferedDataSize += static_cast<std::size_t>(bytesRead);
    }
}

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
    const NativeSocket listenSocket = CreateListenSocket(address, port, errorMessage);
    if (listenSocket == InvalidSocket)
    {
        ShutdownSockets();
        return false;
    }
    mStreamingListenSocket.store(FromNativeSocket(listenSocket), std::memory_order_release);
    mStreamingClientSocket.store(FromNativeSocket(InvalidSocket), std::memory_order_release);

    // Record that we're now streaming.
    mStreamingActive.store(true, std::memory_order_release);

    // Create the thread to read from the socket and parse the data.
    mStreamingThread = std::thread([this, listenSocket]
    {
        StreamingThreadMain(FromNativeSocket(listenSocket));
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
    const NativeSocket listenSocket = ToNativeSocket(mStreamingListenSocket.exchange(FromNativeSocket(InvalidSocket), std::memory_order_acq_rel));
    ShutdownSocket(listenSocket);
    CloseSocket(listenSocket);

    const NativeSocket clientSocket = ToNativeSocket(mStreamingClientSocket.exchange(FromNativeSocket(InvalidSocket), std::memory_order_acq_rel));
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
// StreamingThreadMain
//------------------------------------------------------------------------------
//  Accept native socket connections and parse streamed allocation frames.
//------------------------------------------------------------------------------
void AllocationStreamReader::StreamingThreadMain(std::uintptr_t listenSocketValue)
{
    const NativeSocket listenSocket = ToNativeSocket(listenSocketValue);

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
        mStreamingClientSocket.store(FromNativeSocket(clientSocket), std::memory_order_release);
        std::vector<std::uint8_t> bufferedData;
        std::size_t bufferedDataSize = 0;
        // Read data until the socket fails to read (either due to an error,
        // the client disconnecting, or the thread stopping and the socket
        // being shut down).
        while (true)
        {
            std::size_t frameSize = 0;
            // Returns false if the socket cannot be read from.
            if (!ReceiveSocketFrame(clientSocket, bufferedData, bufferedDataSize, frameSize))
            {
                break;
            }

            if (frameSize > 0)
            {
                ParseStreamingData(std::span<const std::uint8_t>(bufferedData.data(), frameSize));
                ConsumeBufferedData(bufferedData, bufferedDataSize, frameSize);
            }
        }

        const NativeSocket storedClientSocket = ToNativeSocket(
            mStreamingClientSocket.exchange(FromNativeSocket(InvalidSocket), std::memory_order_acq_rel));
        if (storedClientSocket != InvalidSocket)
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
    StreamedHeapSnapshot snapshot;
    std::size_t offset = 0;

    if ((data.size() < SnapshotMagic.size()) ||
        (std::memcmp(data.data(), SnapshotMagic.data(), SnapshotMagic.size()) != 0))
    {
        return;
    }

    offset += SnapshotMagic.size();

    std::uint64_t allocationCount = 0;
    std::uint64_t allocationBytes = 0;
    std::uint64_t pageSize = 0;
    std::uint64_t trackedAllocations = 0;
    std::uint64_t trackedDeallocations = 0;
    std::uint64_t untrackedAllocations = 0;
    std::uint64_t untrackedDeallocations = 0;

    if ((!ReadUInt64(data, offset, allocationCount)) ||
        (!ReadUInt64(data, offset, allocationBytes)) ||
        (!ReadUInt64(data, offset, pageSize)) ||
        (!ReadUInt64(data, offset, trackedAllocations)) ||
        (!ReadUInt64(data, offset, trackedDeallocations)) ||
        (!ReadUInt64(data, offset, untrackedAllocations)) ||
        (!ReadUInt64(data, offset, untrackedDeallocations)))
    {
        return;
    }

    snapshot.mAllocationCount = static_cast<std::size_t>(allocationCount);
    snapshot.mAllocationBytes = static_cast<std::size_t>(allocationBytes);
    snapshot.mHasPageSize = pageSize > 0;
    snapshot.mPageSize = (pageSize > 0)
                             ? static_cast<std::size_t>(pageSize)
                             : DefaultAllocationPageSize;
    snapshot.mTrackedAllocations = static_cast<std::uint64_t>(trackedAllocations);
    snapshot.mTrackedDeallocations = static_cast<std::uint64_t>(trackedDeallocations);
    snapshot.mUntrackedAllocations = static_cast<std::uint64_t>(untrackedAllocations);
    snapshot.mUntrackedDeallocations = static_cast<std::uint64_t>(untrackedDeallocations);

    for (std::size_t typeIndex = 0; typeIndex < AllocationTypeCount; typeIndex++)
    {
        std::uint64_t typeCount = 0;
        if (!ReadUInt64(data, offset, typeCount))
        {
            return;
        }

        snapshot.mTrackedAllocationsByType[typeIndex] = static_cast<std::uint64_t>(typeCount);
    }

    for (std::size_t typeIndex = 0; typeIndex < AllocationTypeCount; typeIndex++)
    {
        std::uint64_t typeCount = 0;
        if (!ReadUInt64(data, offset, typeCount))
        {
            return;
        }

        snapshot.mTrackedDeallocationsByType[typeIndex] = static_cast<std::uint64_t>(typeCount);
    }

    for (std::uint64_t allocationIndex = 0; allocationIndex < allocationCount; allocationIndex++)
    {
        std::uint64_t address = 0;
        std::uint64_t size = 0;
        std::uint32_t typeValue = 0;

        if ((!ReadUInt64(data, offset, address)) ||
            (!ReadUInt64(data, offset, size)) ||
            (!ReadUInt32(data, offset, typeValue)))
        {
            return;
        }

        const auto type = static_cast<HeapVisualizer::AllocationHooks::AllocationType>(typeValue);
        if (!IsValidOperationType(type))
        {
            return;
        }

        snapshot.mAllocations.push_back({static_cast<std::uintptr_t>(address), static_cast<std::size_t>(size), type});
    }

    if ((data.size() - offset != SnapshotFooter.size()) ||
        (std::memcmp(data.data() + offset, SnapshotFooter.data(), SnapshotFooter.size()) != 0))
    {
        return;
    }

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
    if (!range.mHasAllocations)
    {
        range.mHasAllocations = true;
        range.mLowestAddress = allocation.mAddress;
        range.mHighestAddress = allocation.mAddress;
        range.mHighestAddressAllocationSize = allocation.mSize;
        return;
    }

    if (allocation.mAddress < range.mLowestAddress)
    {
        range.mLowestAddress = allocation.mAddress;
    }

    if (allocation.mAddress > range.mHighestAddress)
    {
        range.mHighestAddress = allocation.mAddress;
        range.mHighestAddressAllocationSize = allocation.mSize;
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
    std::size_t minimumSize = 0;
    std::size_t maximumSize = 64;

    while ((allocation.mSize > maximumSize) &&
           (maximumSize <= (std::numeric_limits<std::size_t>::max() / 2)))
    {
        minimumSize = maximumSize + 1;
        maximumSize *= 2;
    }

    if (allocation.mSize > maximumSize)
    {
        minimumSize = maximumSize + 1;
        maximumSize = std::numeric_limits<std::size_t>::max();
    }

    while (bins.empty() || (bins.back().mMaximumSize < maximumSize))
    {
        const std::size_t nextMinimumSize = bins.empty() ? 0 : bins.back().mMaximumSize + 1;
        std::size_t nextMaximumSize = 64;

        if (!bins.empty())
        {
            if (bins.back().mMaximumSize > (std::numeric_limits<std::size_t>::max() / 2))
            {
                nextMaximumSize = std::numeric_limits<std::size_t>::max();
            }
            else
            {
                nextMaximumSize = bins.back().mMaximumSize * 2;
            }
        }

        bins.push_back({nextMinimumSize, nextMaximumSize, 0, 0, 0, 0, false, {}});
    }

    const auto binIterator = std::find_if(
        bins.begin(),
        bins.end(),
        [minimumSize, maximumSize](const StreamedAllocationBin& bin)
        {
            return (bin.mMinimumSize == minimumSize) && (bin.mMaximumSize == maximumSize);
        });

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
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> allocatedPageRanges;

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

    for (const StreamedAllocation& allocation : snapshot.mAllocations)
    {
        if (snapshot.mAllocationCount == 0)
        {
            summary.mAllocationCount++;
            summary.mAllocationBytes += allocation.mSize;
        }

        if (allocation.mSize > 0)
        {
            const std::uintptr_t maximumAddress = std::numeric_limits<std::uintptr_t>::max();
            const std::uintptr_t pageSize = static_cast<std::uintptr_t>(summary.mPageSize);
            const std::uintptr_t allocationEndAddress =
                (allocation.mSize - 1 <= maximumAddress - allocation.mAddress)
                    ? allocation.mAddress + allocation.mSize - 1
                    : maximumAddress;
            allocatedPageRanges.emplace_back(
                allocation.mAddress / pageSize,
                allocationEndAddress / pageSize);
        }

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
