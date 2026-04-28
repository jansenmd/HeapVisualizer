#pragma once

#include <QMainWindow>

#include <cstddef>
#include <memory>
#include <vector>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QWidget;

class AllocationTestWindow : public QMainWindow
{
public:
    explicit AllocationTestWindow(QWidget* parent = nullptr);

private:
    struct AllocationRecord
    {
        std::size_t mSizeInBytes = 0;
        std::unique_ptr<std::byte[]> mMemory;
    };

    void CreateMenus();
    void CreateControls();
    void AllocateMemory();
    void FreeSelectedAllocation();
    void ClearAllocations();
    void UpdateSummary();
    [[nodiscard]] std::size_t CalculateRequestedBytes() const;
    [[nodiscard]] std::string BuildAllocationLabel(std::size_t allocationIndex,
                                                   std::size_t sizeInBytes) const;
    [[nodiscard]] std::string BuildSummaryText() const;

    QAction* mExitAction = nullptr;
    QSpinBox* mAllocationSizeSpinBox = nullptr;
    QComboBox* mAllocationUnitComboBox = nullptr;
    QSpinBox* mAllocationCountSpinBox = nullptr;
    QCheckBox* mTouchMemoryCheckBox = nullptr;
    QPushButton* mAllocateButton = nullptr;
    QPushButton* mFreeSelectedButton = nullptr;
    QPushButton* mClearButton = nullptr;
    QListWidget* mAllocationListWidget = nullptr;
    QLabel* mSummaryLabel = nullptr;
    std::vector<AllocationRecord> mAllocations;
};
