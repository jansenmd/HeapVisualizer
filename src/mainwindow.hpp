#pragma once

#include <QMainWindow>
#include <QWidget>

#include "Preload/TrackAllocations.h"

#include <array>
#include <cstdint>
#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class QAction;
class QCheckBox;
class QLabel;
class QPaintEvent;
class QResizeEvent;
class QTableWidget;
class QTabWidget;

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

class BinGraphWidget : public QWidget
{
public:
    explicit BinGraphWidget(QWidget* parent = nullptr);

    void SetBins(const std::vector<StreamedAllocationBin>& bins);
    void SetSuppressionChangedHandler(
        std::function<void(std::size_t minimumSize, std::size_t maximumSize, bool suppressed)> handler);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void UpdateBinControls();

    std::vector<StreamedAllocationBin> mBins;
    std::vector<QCheckBox*> mBinCheckBoxes;
    std::function<void(std::size_t minimumSize, std::size_t maximumSize, bool suppressed)> mSuppressionChangedHandler;
    bool mUpdatingCheckBoxes = false;
};

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private:
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
    [[nodiscard]] std::string BuildAddressText(std::uintptr_t address, bool hasAddress, int minimumDigits = 0) const;
    [[nodiscard]] std::string BuildHexValueText(std::size_t value, bool hasValue) const;
    [[nodiscard]] std::string BuildByteCountText(std::size_t byteCount) const;
    void UpdateStreamingSummary(const StreamedAllocationSummary& summary);
    void UpdateOverviewView(const StreamedAllocationSummary& summary);
    void UpdateBinsView(const StreamedAllocationSummary& summary);
    void UpdateSuppressedBinState(std::size_t minimumSize, std::size_t maximumSize, bool suppressed);
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
    std::vector<StreamedAllocationBin> mCurrentBins;
    std::set<std::pair<std::size_t, std::size_t>> mSuppressedBins;
};
