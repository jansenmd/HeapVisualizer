#pragma once

#include <QMainWindow>
#include <QWidget>

#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class QAction;
class QLabel;
class QPaintEvent;
class QTableWidget;
class QTabWidget;

struct StreamedAllocationBin
{
    std::size_t mMinimumSize = 0;
    std::size_t mMaximumSize = 0;
    std::size_t mAllocationCount = 0;
    std::size_t mAllocationBytes = 0;
    std::size_t mMinimumAllocationSize = 0;
    std::size_t mMaximumAllocationSize = 0;
};

class BinGraphWidget : public QWidget
{
public:
    explicit BinGraphWidget(QWidget* parent = nullptr);

    void SetBins(const std::vector<StreamedAllocationBin>& bins);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<StreamedAllocationBin> mBins;
};

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
        std::size_t mAllocationCount = 0;
        std::size_t mAllocationBytes = 0;
        unsigned long long mTrackedAllocations = 0;
        unsigned long long mTrackedDeallocations = 0;
        unsigned long long mUntrackedAllocations = 0;
        unsigned long long mUntrackedDeallocations = 0;
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
        StreamedAddressRange mAddressRange;
        unsigned long long mTrackedAllocations = 0;
        unsigned long long mTrackedDeallocations = 0;
        unsigned long long mUntrackedAllocations = 0;
        unsigned long long mUntrackedDeallocations = 0;
        std::vector<StreamedAllocationBin> mBins;
    };

    void CreateMenus();
    void StartStreaming();
    void StopStreaming();
    void ToggleStreaming();
    [[nodiscard]] bool OpenStreamingListener(const std::string& address, std::uint16_t port, std::string& errorMessage);
    void StreamingThreadMain(std::uintptr_t listenSocket);
    void ParseStreamingData(const std::string& data);
    void UpdateAddressRange(StreamedAddressRange& range, const StreamedAllocation& allocation) const;
    void AddAllocationToBin(std::vector<StreamedAllocationBin>& bins, const StreamedAllocation& allocation) const;
    [[nodiscard]] StreamedAllocationSummary BuildAllocationSummary(const StreamedHeapSnapshot& snapshot) const;
    [[nodiscard]] std::uintptr_t CalculateAddressRangeEnd(const StreamedAddressRange& range) const;
    [[nodiscard]] std::string BuildAddressRangeText(const StreamedAddressRange& range) const;
    [[nodiscard]] std::string BuildAddressText(std::uintptr_t address, bool hasAddress) const;
    [[nodiscard]] std::string BuildHexValueText(std::size_t value, bool hasValue) const;
    [[nodiscard]] std::string BuildByteCountText(std::size_t byteCount) const;
    void UpdateStreamingSummary(const StreamedAllocationSummary& summary);
    void UpdateOverviewView(const StreamedAllocationSummary& summary);
    void UpdateBinsView(const StreamedAllocationSummary& summary);
    void ShowAboutDialog();

    QAction* mExitAction = nullptr;
    QAction* mStreamingAction = nullptr;
    QAction* mAboutAction = nullptr;
    QTabWidget* mViewTabs = nullptr;
    QTableWidget* mOverviewTable = nullptr;
    QTableWidget* mBinsTable = nullptr;
    BinGraphWidget* mBinsGraphWidget = nullptr;
    QLabel* mHeatmapLabel = nullptr;
    std::atomic_bool mStreamingActive = false;
    std::uintptr_t mStreamingListenSocket = 0;
    std::thread mStreamingThread;
    std::mutex mStreamedSnapshotsMutex;
    std::vector<StreamedHeapSnapshot> mStreamedSnapshots;
};
