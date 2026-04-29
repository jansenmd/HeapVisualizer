#pragma once

#include <QMainWindow>

#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class QAction;
class QLabel;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private:
    struct StreamedAllocation
    {
        std::string mPool;
        std::uintptr_t mAddress = 0;
        std::size_t mSize = 0;
    };

    struct StreamedHeapSnapshot
    {
        std::vector<StreamedAllocation> mAllocations;
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
        std::size_t mApplicationAllocationCount = 0;
        std::size_t mApplicationTotalSize = 0;
        StreamedAddressRange mApplicationAddressRange;
        std::size_t mInternalAllocationCount = 0;
        std::size_t mInternalTotalSize = 0;
        StreamedAddressRange mInternalAddressRange;
        std::size_t mTotalAllocationCount = 0;
        std::size_t mTotalSize = 0;
        StreamedAddressRange mTotalAddressRange;
    };

    void CreateMenus();
    void StartStreaming();
    void StopStreaming();
    void ToggleStreaming();
    [[nodiscard]] bool OpenStreamingListener(const std::string& address, std::uint16_t port, std::string& errorMessage);
    void StreamingThreadMain(std::uintptr_t listenSocket);
    void ParseStreamingData(const std::string& data);
    void UpdateAddressRange(StreamedAddressRange& range, const StreamedAllocation& allocation) const;
    [[nodiscard]] StreamedAllocationSummary BuildAllocationSummary(const StreamedHeapSnapshot& snapshot) const;
    [[nodiscard]] std::string BuildAddressRangeText(const StreamedAddressRange& range) const;
    [[nodiscard]] std::string BuildStreamingSummaryText(const StreamedAllocationSummary& summary) const;
    void UpdateStreamingSummary(const StreamedAllocationSummary& summary);
    void ShowAboutDialog();

    QAction* mExitAction = nullptr;
    QAction* mStreamingAction = nullptr;
    QAction* mAboutAction = nullptr;
    QLabel* mStreamingSummaryLabel = nullptr;
    std::atomic_bool mStreamingActive = false;
    std::uintptr_t mStreamingListenSocket = 0;
    std::thread mStreamingThread;
    std::mutex mStreamedSnapshotsMutex;
    std::vector<StreamedHeapSnapshot> mStreamedSnapshots;
};
