#pragma once

#include <QMainWindow>
#include <QWidget>

#include "AllocationStreamReader.hpp"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

class QAction;
class QCheckBox;
class QLabel;
class QPaintEvent;
class QResizeEvent;
class QTableWidget;
class QTabWidget;

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
    void CreateMenus();
    void StartStreaming();
    void StopStreaming();
    void ToggleStreaming();
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
    AllocationStreamReader mAllocationStreamReader;
    std::vector<StreamedAllocationBin> mCurrentBins;
    std::set<std::pair<std::size_t, std::size_t>> mSuppressedBins;
};
