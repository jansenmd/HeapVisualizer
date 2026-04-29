#include "allocationtestwindow.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstddef>
#include <new>
#include <numeric>
#include <sstream>
#include <string>


//------------------------------------------------------------------------------
// AllocationTestWindow
//------------------------------------------------------------------------------
//  Construct the heap allocation test window and initialize its controls.
//------------------------------------------------------------------------------
AllocationTestWindow::AllocationTestWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Heap Allocation Test"));
    resize(760, 520);
    CreateMenus();
    CreateControls();
    UpdateSummary();
}


//------------------------------------------------------------------------------
// CreateMenus
//------------------------------------------------------------------------------
//  Create the test application menu actions and connect them to their handlers.
//------------------------------------------------------------------------------
void AllocationTestWindow::CreateMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    mExitAction = fileMenu->addAction(tr("E&xit"));
    mExitAction->setShortcut(QKeySequence::Quit);
    connect(mExitAction, &QAction::triggered, this, []
    {
        QApplication::quit();
    });
}


//------------------------------------------------------------------------------
// CreateControls
//------------------------------------------------------------------------------
//  Create controls that allocate, release, and display heap allocations.
//------------------------------------------------------------------------------
void AllocationTestWindow::CreateControls()
{
    QWidget* centralWidget = new QWidget(this);
    centralWidget->setObjectName(QStringLiteral("AllocationTestSurface"));
    QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(24, 22, 24, 24);
    mainLayout->setSpacing(14);

    QLabel* titleLabel = new QLabel(tr("Heap Allocation Test"), centralWidget);
    titleLabel->setObjectName(QStringLiteral("WindowTitle"));

    QLabel* subtitleLabel = new QLabel(tr("Allocate and release memory blocks for stream testing"), centralWidget);
    subtitleLabel->setObjectName(QStringLiteral("WindowSubtitle"));

    QGroupBox* allocationGroupBox = new QGroupBox(tr("Allocation"), centralWidget);
    QGridLayout* allocationLayout = new QGridLayout(allocationGroupBox);
    allocationLayout->setContentsMargins(16, 24, 16, 16);
    allocationLayout->setHorizontalSpacing(12);
    allocationLayout->setVerticalSpacing(10);

    mAllocationSizeSpinBox = new QSpinBox(allocationGroupBox);
    mAllocationSizeSpinBox->setRange(1, 1024);
    mAllocationSizeSpinBox->setValue(1);

    mAllocationUnitComboBox = new QComboBox(allocationGroupBox);
    mAllocationUnitComboBox->addItem(tr("Bytes"));
    mAllocationUnitComboBox->addItem(tr("KiB"));
    mAllocationUnitComboBox->addItem(tr("MiB"));
    mAllocationUnitComboBox->setCurrentText(tr("MiB"));

    mAllocationCountSpinBox = new QSpinBox(allocationGroupBox);
    mAllocationCountSpinBox->setRange(1, 1000);
    mAllocationCountSpinBox->setValue(1);

    mTouchMemoryCheckBox = new QCheckBox(tr("Touch allocated memory"), allocationGroupBox);
    mTouchMemoryCheckBox->setChecked(true);

    mAllocateButton = new QPushButton(tr("Allocate"), allocationGroupBox);

    allocationLayout->addWidget(new QLabel(tr("Size"), allocationGroupBox), 0, 0);
    allocationLayout->addWidget(mAllocationSizeSpinBox, 0, 1);
    allocationLayout->addWidget(mAllocationUnitComboBox, 0, 2);
    allocationLayout->addWidget(new QLabel(tr("Count"), allocationGroupBox), 1, 0);
    allocationLayout->addWidget(mAllocationCountSpinBox, 1, 1);
    allocationLayout->addWidget(mTouchMemoryCheckBox, 2, 0, 1, 3);
    allocationLayout->addWidget(mAllocateButton, 3, 0, 1, 3);

    mAllocationListWidget = new QListWidget(centralWidget);

    mFreeSelectedButton = new QPushButton(tr("Free Selected"), centralWidget);
    mClearButton = new QPushButton(tr("Clear All"), centralWidget);
    mSummaryLabel = new QLabel(centralWidget);
    mSummaryLabel->setObjectName(QStringLiteral("SummaryPanel"));

    QGridLayout* buttonLayout = new QGridLayout();
    buttonLayout->setHorizontalSpacing(10);
    buttonLayout->addWidget(mFreeSelectedButton, 0, 0);
    buttonLayout->addWidget(mClearButton, 0, 1);

    mainLayout->addWidget(titleLabel);
    mainLayout->addWidget(subtitleLabel);
    mainLayout->addWidget(allocationGroupBox);
    mainLayout->addWidget(mAllocationListWidget, 1);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(mSummaryLabel);

    setCentralWidget(centralWidget);

    connect(mAllocateButton, &QPushButton::clicked, this, [this]
    {
        AllocateMemory();
    });

    connect(mFreeSelectedButton, &QPushButton::clicked, this, [this]
    {
        FreeSelectedAllocation();
    });

    connect(mClearButton, &QPushButton::clicked, this, [this]
    {
        ClearAllocations();
    });
}


//------------------------------------------------------------------------------
// AllocateMemory
//------------------------------------------------------------------------------
//  Allocate the requested number of heap memory blocks.
//------------------------------------------------------------------------------
void AllocationTestWindow::AllocateMemory()
{
    const std::size_t requestedBytes = CalculateRequestedBytes();
    const int allocationCount = mAllocationCountSpinBox->value();

    for (int allocationOffset = 0; allocationOffset < allocationCount; ++allocationOffset)
    {
        AllocationRecord record;
        record.mSizeInBytes = requestedBytes;
        record.mMemory = std::make_unique_for_overwrite<std::byte[]>(requestedBytes);

        if (mTouchMemoryCheckBox->isChecked())
        {
            std::fill_n(record.mMemory.get(), requestedBytes, static_cast<std::byte>(0xA5));
        }

        mAllocations.push_back(std::move(record));
        const std::size_t allocationIndex = mAllocations.size();
        const std::string label = BuildAllocationLabel(allocationIndex, requestedBytes);
        mAllocationListWidget->addItem(QString::fromStdString(label));
    }

    UpdateSummary();
}


//------------------------------------------------------------------------------
// FreeSelectedAllocation
//------------------------------------------------------------------------------
//  Release the selected heap allocation.
//------------------------------------------------------------------------------
void AllocationTestWindow::FreeSelectedAllocation()
{
    const int selectedRow = mAllocationListWidget->currentRow();
    if (selectedRow < 0)
    {
        return;
    }

    const auto allocationPosition = mAllocations.begin() + selectedRow;
    mAllocations.erase(allocationPosition);
    delete mAllocationListWidget->takeItem(selectedRow);
    UpdateSummary();
}


//------------------------------------------------------------------------------
// ClearAllocations
//------------------------------------------------------------------------------
//  Release all heap allocations owned by the test application.
//------------------------------------------------------------------------------
void AllocationTestWindow::ClearAllocations()
{
    mAllocations.clear();
    mAllocationListWidget->clear();
    UpdateSummary();
}


//------------------------------------------------------------------------------
// UpdateSummary
//------------------------------------------------------------------------------
//  Refresh the allocation count and total allocated byte summary.
//------------------------------------------------------------------------------
void AllocationTestWindow::UpdateSummary()
{
    const std::string summaryText = BuildSummaryText();
    mSummaryLabel->setText(QString::fromStdString(summaryText));
}


//------------------------------------------------------------------------------
// CalculateRequestedBytes
//------------------------------------------------------------------------------
//  Convert the selected allocation size and unit into bytes.
//------------------------------------------------------------------------------
std::size_t AllocationTestWindow::CalculateRequestedBytes() const
{
    const std::size_t requestedSize = static_cast<std::size_t>(mAllocationSizeSpinBox->value());
    const int unitIndex = mAllocationUnitComboBox->currentIndex();

    if (unitIndex == 1)
    {
        return requestedSize * 1024;
    }

    if (unitIndex == 2)
    {
        return requestedSize * 1024 * 1024;
    }

    return requestedSize;
}


//------------------------------------------------------------------------------
// BuildAllocationLabel
//------------------------------------------------------------------------------
//  Build display text for a single allocation record.
//------------------------------------------------------------------------------
std::string AllocationTestWindow::BuildAllocationLabel(std::size_t allocationIndex,
                                                       std::size_t sizeInBytes) const
{
    std::ostringstream output;
    output << "Allocation " << allocationIndex << ": " << sizeInBytes << " bytes";
    return output.str();
}


//------------------------------------------------------------------------------
// BuildSummaryText
//------------------------------------------------------------------------------
//  Build display text describing the current heap allocation total.
//------------------------------------------------------------------------------
std::string AllocationTestWindow::BuildSummaryText() const
{
    const std::size_t totalBytes = std::accumulate(
        mAllocations.begin(),
        mAllocations.end(),
        static_cast<std::size_t>(0),
        [](std::size_t total, const AllocationRecord& record)
        {
            return total + record.mSizeInBytes;
        });

    std::ostringstream output;
    output << "Allocations: " << mAllocations.size() << "    Total: " << totalBytes << " bytes";
    return output.str();
}
