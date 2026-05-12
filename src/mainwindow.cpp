#include "mainwindow.hpp"

#include "build_info.hpp"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QFontDatabase>
#include <QGraphicsOpacityEffect>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPaintEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QString>
#include <QPushButton>
#include <QShortcut>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace
{

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
// AllocationOperationName
//------------------------------------------------------------------------------
//  Return the display name for an allocation operation type.
//------------------------------------------------------------------------------
QString AllocationOperationName(HeapVisualizer::AllocationHooks::AllocationType type)
{
    using HeapVisualizer::AllocationHooks::AllocationType;

    switch (type)
    {
        case AllocationType::Malloc:
            return QStringLiteral("malloc");
        case AllocationType::Calloc:
            return QStringLiteral("calloc");
        case AllocationType::Realloc:
            return QStringLiteral("realloc");
        case AllocationType::ReallocArray:
            return QStringLiteral("reallocarray");
        case AllocationType::Free:
            return QStringLiteral("free");
        case AllocationType::CFree:
            return QStringLiteral("cfree");
        case AllocationType::PosixMemalign:
            return QStringLiteral("posix_memalign");
        case AllocationType::AlignedAlloc:
            return QStringLiteral("aligned_alloc");
        case AllocationType::Memalign:
            return QStringLiteral("memalign");
        case AllocationType::Valloc:
            return QStringLiteral("valloc");
        case AllocationType::PValloc:
            return QStringLiteral("pvalloc");
        case AllocationType::Count:
        default:
            break;
    }

    return QStringLiteral("unknown");
}


//------------------------------------------------------------------------------
// CopyTableSelectionToClipboard
//------------------------------------------------------------------------------
//  Copy selected table cells, or the current cell, as tab-separated text.
//------------------------------------------------------------------------------
void CopyTableSelectionToClipboard(QTableWidget* tableWidget)
{
    if (tableWidget == nullptr)
    {
        return;
    }

    QList<QTableWidgetSelectionRange> selectedRanges = tableWidget->selectedRanges();
    if (selectedRanges.empty() && (tableWidget->currentItem() != nullptr))
    {
        const int currentRow = tableWidget->currentRow();
        const int currentColumn = tableWidget->currentColumn();
        selectedRanges.push_back(QTableWidgetSelectionRange(currentRow, currentColumn, currentRow, currentColumn));
    }

    if (selectedRanges.empty())
    {
        return;
    }

    QString text;
    const QTableWidgetSelectionRange range = selectedRanges.front();
    for (int row = range.topRow(); row <= range.bottomRow(); row++)
    {
        if (row > range.topRow())
        {
            text += QLatin1Char('\n');
        }

        for (int column = range.leftColumn(); column <= range.rightColumn(); column++)
        {
            if (column > range.leftColumn())
            {
                text += QLatin1Char('\t');
            }

            const QTableWidgetItem* item = tableWidget->item(row, column);
            if (item != nullptr)
            {
                text += item->text();
            }
        }
    }

    QApplication::clipboard()->setText(text);
}


//------------------------------------------------------------------------------
// ConfigureDataTableInteraction
//------------------------------------------------------------------------------
//  Configure table interaction for invisible selection and copy support.
//------------------------------------------------------------------------------
void ConfigureDataTableInteraction(QTableWidget* tableWidget)
{
    tableWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tableWidget->setSelectionBehavior(QAbstractItemView::SelectItems);
    tableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);

    QShortcut* copyShortcut = new QShortcut(QKeySequence::Copy, tableWidget);
    QObject::connect(
        copyShortcut,
        &QShortcut::activated,
        tableWidget,
        [tableWidget]
        {
            CopyTableSelectionToClipboard(tableWidget);
        });
}


//------------------------------------------------------------------------------
// BuildCommaSeparatedIntegerText
//------------------------------------------------------------------------------
//  Build display text for an integer with comma group separators.
//------------------------------------------------------------------------------
std::string BuildCommaSeparatedIntegerText(uint64_t value)
{
    std::string text = std::to_string(value);
    for (int insertPosition = static_cast<int>(text.size()) - 3;
         insertPosition > 0;
         insertPosition -= 3)
    {
        text.insert(static_cast<std::size_t>(insertPosition), ",");
    }

    return text;
}


//------------------------------------------------------------------------------
// BuildBinLimitLabel
//------------------------------------------------------------------------------
//  Build compact display text for a bin upper bound.
//------------------------------------------------------------------------------
std::string BuildBinLimitLabel(std::size_t byteCount)
{
    constexpr std::size_t kilobyte = 1024;
    constexpr std::size_t megabyte = kilobyte * 1024;
    constexpr std::size_t gigabyte = megabyte * 1024;

    const char* suffix = "B";
    std::size_t value = byteCount;

    if (byteCount >= gigabyte)
    {
        suffix = "GB";
        value /= gigabyte;
    }
    else if (byteCount >= megabyte)
    {
        suffix = "MB";
        value /= megabyte;
    }
    else if (byteCount >= kilobyte)
    {
        suffix = "KB";
        value /= kilobyte;
    }

    std::ostringstream output;
    output << value << suffix;
    return output.str();
}


//------------------------------------------------------------------------------
// BuildByteTickLabel
//------------------------------------------------------------------------------
//  Build display text for byte-axis tick marks with whole-number units.
//------------------------------------------------------------------------------
std::string BuildByteTickLabel(std::size_t byteCount)
{
    constexpr std::size_t kilobyte = 1024;
    constexpr std::size_t megabyte = kilobyte * 1024;
    constexpr std::size_t gigabyte = megabyte * 1024;

    const char* suffix = "B";
    std::size_t value = byteCount;

    if ((byteCount >= gigabyte) && ((byteCount % gigabyte) == 0))
    {
        suffix = "GB";
        value /= gigabyte;
    }
    else if ((byteCount >= megabyte) && ((byteCount % megabyte) == 0))
    {
        suffix = "MB";
        value /= megabyte;
    }
    else if ((byteCount >= kilobyte) && ((byteCount % kilobyte) == 0))
    {
        suffix = "KB";
        value /= kilobyte;
    }

    std::ostringstream output;
    output << value << suffix;
    return output.str();
}


//------------------------------------------------------------------------------
// CalculateMedianAllocationSize
//------------------------------------------------------------------------------
//  Calculate the median allocation size for a bin.
//------------------------------------------------------------------------------
std::size_t CalculateMedianAllocationSize(const std::vector<std::size_t>& allocationSizes)
{
    if (allocationSizes.empty())
    {
        return 0;
    }

    std::vector<std::size_t> sortedSizes = allocationSizes;
    std::sort(sortedSizes.begin(), sortedSizes.end());

    const std::size_t middleIndex = sortedSizes.size() / 2;
    if ((sortedSizes.size() % 2) != 0)
    {
        return sortedSizes[middleIndex];
    }

    const std::size_t lowerValue = sortedSizes[middleIndex - 1];
    const std::size_t upperValue = sortedSizes[middleIndex];
    return lowerValue + ((upperValue - lowerValue) / 2);
}


//------------------------------------------------------------------------------
// RoundUpToPowerOfTwo
//------------------------------------------------------------------------------
//  Round a value up to the next power of two.
//------------------------------------------------------------------------------
std::size_t RoundUpToPowerOfTwo(std::size_t value)
{
    if (value <= 1)
    {
        return 1;
    }

    std::size_t roundedValue = 1;
    while (roundedValue < value)
    {
        if (roundedValue > (std::numeric_limits<std::size_t>::max() / 2))
        {
            return std::numeric_limits<std::size_t>::max();
        }
        roundedValue *= 2;
    }

    return roundedValue;
}


//------------------------------------------------------------------------------
// HexDigitCount
//------------------------------------------------------------------------------
//  Return the number of hexadecimal digits needed to display a value.
//------------------------------------------------------------------------------
int HexDigitCount(std::uintptr_t value)
{
    int digitCount = 1;
    while (value >= 16)
    {
        value /= 16;
        digitCount++;
    }
    return digitCount;
}


//------------------------------------------------------------------------------
// BuildPercentageText
//------------------------------------------------------------------------------
//  Build display text for a ratio as a percentage.
//------------------------------------------------------------------------------
std::string BuildPercentageText(double ratio)
{
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << (ratio * 100.0) << "%";
    return output.str();
}


//------------------------------------------------------------------------------
// SetTableRow
//------------------------------------------------------------------------------
//  Set a two-column table row using non-editable cells.
//------------------------------------------------------------------------------
void SetTableRow(QTableWidget* tableWidget,
                 int row,
                 const QString& name,
                 const QString& value,
                 bool useMonospaceValue = false,
                 bool useMutedText = false)
{
    QTableWidgetItem* nameItem = new QTableWidgetItem(name);
    QTableWidgetItem* valueItem = new QTableWidgetItem(value);
    nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
    valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);

    if (useMonospaceValue)
    {
        valueItem->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    }

    if (useMutedText)
    {
        const QBrush mutedTextBrush(QColor(QStringLiteral("#9aa6b2")));
        nameItem->setForeground(mutedTextBrush);
        valueItem->setForeground(mutedTextBrush);
    }

    tableWidget->setItem(row, 0, nameItem);
    tableWidget->setItem(row, 1, valueItem);
}

}


//------------------------------------------------------------------------------
// BinGraphWidget
//------------------------------------------------------------------------------
//  Construct the allocation bin graph widget.
//------------------------------------------------------------------------------
BinGraphWidget::BinGraphWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(250);
    setObjectName(QStringLiteral("BinGraph"));
}


//------------------------------------------------------------------------------
// SetBins
//------------------------------------------------------------------------------
//  Replace the bin data displayed by the graph.
//------------------------------------------------------------------------------
void BinGraphWidget::SetBins(const std::vector<StreamedAllocationBin>& bins)
{
    mBins = bins;
    UpdateBinControls();
    update();
}


//------------------------------------------------------------------------------
// SetSuppressionChangedHandler
//------------------------------------------------------------------------------
//  Set the callback used when a graph bin checkbox is toggled.
//------------------------------------------------------------------------------
void BinGraphWidget::SetSuppressionChangedHandler(
    std::function<void(std::size_t minimumSize, std::size_t maximumSize, bool suppressed)> handler)
{
    mSuppressionChangedHandler = std::move(handler);
}


//------------------------------------------------------------------------------
// paintEvent
//------------------------------------------------------------------------------
//  Draw allocation bin byte totals as bars and allocation counts as a line.
//------------------------------------------------------------------------------
void BinGraphWidget::paintEvent(QPaintEvent* event)
{
    static_cast<void>(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(QStringLiteral("#ffffff")));

    QRect chartRect = rect().adjusted(52, 14, -52, -52);
    painter.setPen(QColor(QStringLiteral("#cfd8e3")));
    painter.drawRect(chartRect);

    if (mBins.empty())
    {
        painter.setPen(QColor(QStringLiteral("#536173")));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No allocations"));
        return;
    }

    std::size_t maximumBytes = 0;
    std::size_t maximumAllocations = 0;
    for (const StreamedAllocationBin& bin : mBins)
    {
        if (!bin.mSuppressed)
        {
            maximumBytes = std::max(maximumBytes, bin.mAllocationBytes);
            maximumAllocations = std::max(maximumAllocations, bin.mAllocationCount);
        }
    }

    if (maximumBytes == 0)
    {
        maximumBytes = 1;
    }
    else
    {
        maximumBytes = RoundUpToPowerOfTwo(maximumBytes);
    }

    if (maximumAllocations == 0)
    {
        maximumAllocations = 1;
    }

    painter.setPen(QColor(QStringLiteral("#536173")));
    QFont labelFont = painter.font();
    labelFont.setPointSize(std::max(7, labelFont.pointSize() - 2));
    labelFont.setWeight(QFont::DemiBold);
    painter.setFont(labelFont);

    constexpr int tickCount = 5;
    for (int tickIndex = 0; tickIndex < tickCount; tickIndex++)
    {
        const double scale = static_cast<double>(tickIndex) / static_cast<double>(tickCount - 1);
        const int y = chartRect.bottom() -
                      static_cast<int>(scale * static_cast<double>(chartRect.height()));
        const auto allocationTickValue = static_cast<unsigned long long>(
            (static_cast<double>(maximumAllocations) * scale) + 0.5);
        const std::size_t byteTickValue = static_cast<std::size_t>(
            (static_cast<double>(maximumBytes) * scale) + 0.5);

        painter.setPen(QColor(QStringLiteral("#e2e8f0")));
        painter.drawLine(chartRect.left(), y, chartRect.right(), y);

        painter.setPen(QColor(QStringLiteral("#dc2626")));
        painter.drawLine(chartRect.left() - 5, y, chartRect.left(), y);
        painter.drawText(
            QRect(rect().left() + 2, y - 8, chartRect.left() - rect().left() - 9, 16),
            Qt::AlignRight | Qt::AlignVCenter,
            QString::fromStdString(BuildCommaSeparatedIntegerText(allocationTickValue)));

        painter.setPen(QColor(QStringLiteral("#2f80ed")));
        painter.drawLine(chartRect.right(), y, chartRect.right() + 5, y);
        painter.drawText(
            QRect(chartRect.right() + 7, y - 8, rect().right() - chartRect.right() - 9, 16),
            Qt::AlignLeft | Qt::AlignVCenter,
            QString::fromStdString(BuildByteTickLabel(byteTickValue)));
    }

    const int binCount = static_cast<int>(mBins.size());
    const int slotWidth = std::max(1, chartRect.width() / binCount);
    const int barWidth = std::max(3, slotWidth - 6);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#2f80ed")));

    for (int binIndex = 0; binIndex < binCount; binIndex++)
    {
        const StreamedAllocationBin& bin = mBins[static_cast<std::size_t>(binIndex)];
        const std::size_t allocationBytes = bin.mSuppressed ? 0 : bin.mAllocationBytes;
        const double normalizedHeight = static_cast<double>(allocationBytes) /
                                        static_cast<double>(maximumBytes);
        const int barHeight = static_cast<int>(normalizedHeight * static_cast<double>(chartRect.height()));
        const int x = chartRect.left() + (binIndex * slotWidth) + ((slotWidth - barWidth) / 2);
        const int y = chartRect.bottom() - barHeight + 1;
        painter.drawRect(QRect(x, y, barWidth, barHeight));
    }

    std::vector<QPointF> countPoints;
    countPoints.reserve(mBins.size());

    for (int binIndex = 0; binIndex < binCount; binIndex++)
    {
        const StreamedAllocationBin& bin = mBins[static_cast<std::size_t>(binIndex)];
        const std::size_t allocationCount = bin.mSuppressed ? 0 : bin.mAllocationCount;
        const double normalizedHeight = static_cast<double>(allocationCount) /
                                        static_cast<double>(maximumAllocations);
        const int x = chartRect.left() + (binIndex * slotWidth) + (slotWidth / 2);
        const int y = chartRect.bottom() -
                      static_cast<int>(normalizedHeight * static_cast<double>(chartRect.height())) +
                      1;
        countPoints.push_back(QPointF(x, y));
    }

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(QStringLiteral("#dc2626")), 2));
    if (countPoints.size() == 1)
    {
        painter.setBrush(QColor(QStringLiteral("#dc2626")));
        painter.drawEllipse(countPoints.front(), 3.0, 3.0);
    }
    else
    {
        painter.drawPolyline(countPoints.data(), static_cast<int>(countPoints.size()));
    }

    QFont binLabelFont = painter.font();
    binLabelFont.setWeight(QFont::DemiBold);
    painter.setFont(binLabelFont);
    for (int binIndex = 0; binIndex < binCount; binIndex++)
    {
        const StreamedAllocationBin& bin = mBins[static_cast<std::size_t>(binIndex)];
        const int labelX = chartRect.left() + (binIndex * slotWidth);
        const QRect labelRect(labelX, chartRect.bottom() + 4, slotWidth, 18);
        painter.setPen(
            bin.mAllocationCount == 0
                ? QColor(QStringLiteral("#9aa6b2"))
                : QColor(QStringLiteral("#536173")));
        painter.drawText(
            labelRect,
            Qt::AlignTop | Qt::AlignHCenter,
            QString::fromStdString(BuildBinLimitLabel(bin.mMaximumSize)));
    }
}


//------------------------------------------------------------------------------
// resizeEvent
//------------------------------------------------------------------------------
//  Reposition bin graph checkboxes when the chart widget changes size.
//------------------------------------------------------------------------------
void BinGraphWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    UpdateBinControls();
}


//------------------------------------------------------------------------------
// UpdateBinControls
//------------------------------------------------------------------------------
//  Create and position graph visibility checkboxes beneath each bin column.
//------------------------------------------------------------------------------
void BinGraphWidget::UpdateBinControls()
{
    while (mBinCheckBoxes.size() < mBins.size())
    {
        QCheckBox* checkBox = new QCheckBox(this);
        checkBox->setText(QString());
        checkBox->setToolTip(QStringLiteral("Show this bin in the graph"));
        checkBox->setGraphicsEffect(new QGraphicsOpacityEffect(checkBox));
        connect(
            checkBox,
            &QCheckBox::toggled,
            this,
            [this, checkBox](bool checked)
            {
                if (mUpdatingCheckBoxes)
                {
                    return;
                }

                const int binIndex = checkBox->property("binIndex").toInt();
                if ((binIndex < 0) || (binIndex >= static_cast<int>(mBins.size())))
                {
                    return;
                }

                const StreamedAllocationBin& bin = mBins[static_cast<std::size_t>(binIndex)];
                if (mSuppressionChangedHandler)
                {
                    mSuppressionChangedHandler(bin.mMinimumSize, bin.mMaximumSize, !checked);
                }
            });
        mBinCheckBoxes.push_back(checkBox);
    }

    mUpdatingCheckBoxes = true;

    const QRect chartRect = rect().adjusted(52, 14, -52, -52);
    const int binCount = static_cast<int>(mBins.size());
    const int slotWidth = binCount == 0 ? 1 : std::max(1, chartRect.width() / binCount);

    for (std::size_t binIndex = 0; binIndex < mBinCheckBoxes.size(); binIndex++)
    {
        QCheckBox* checkBox = mBinCheckBoxes[binIndex];
        const bool binVisible = binIndex < mBins.size();
        checkBox->setVisible(binVisible);

        if (!binVisible)
        {
            continue;
        }

        const StreamedAllocationBin& bin = mBins[binIndex];
        const int x = chartRect.left() + (static_cast<int>(binIndex) * slotWidth) + ((slotWidth - 18) / 2);
        const int y = chartRect.bottom() + 22;
        checkBox->setProperty("binIndex", static_cast<int>(binIndex));
        checkBox->setChecked(!bin.mSuppressed);
        if (QGraphicsOpacityEffect* opacityEffect = qobject_cast<QGraphicsOpacityEffect*>(checkBox->graphicsEffect()))
        {
            opacityEffect->setOpacity(bin.mAllocationCount == 0 ? 0.45 : 1.0);
        }
        checkBox->setGeometry(x, y, 18, 18);
    }

    mUpdatingCheckBoxes = false;
}


//------------------------------------------------------------------------------
// MainWindow
//------------------------------------------------------------------------------
//  Construct the main application window and initialize its menus.
//------------------------------------------------------------------------------
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    mAllocationStreamReader.SetSummaryHandler(
        [this](const StreamedAllocationSummary& summary)
        {
            QMetaObject::invokeMethod(
                this,
                [this, summary]
                {
                    UpdateStreamingSummary(summary);
                },
                Qt::QueuedConnection);
        });

    setWindowTitle(QStringLiteral("HeapVisualizer"));
    resize(900, 600);
    QWidget* centralWidget = new QWidget(this);
    centralWidget->setObjectName(QStringLiteral("MainSurface"));
    QVBoxLayout* layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(14, 12, 14, 14);
    layout->setSpacing(10);

    mViewTabs = new QTabWidget(centralWidget);
    mViewTabs->setObjectName(QStringLiteral("MainTabs"));

    QWidget* overviewWidget = new QWidget(mViewTabs);
    QVBoxLayout* overviewLayout = new QVBoxLayout(overviewWidget);
    overviewLayout->setContentsMargins(0, 0, 0, 0);
    mOverviewTable = new QTableWidget(overviewWidget);
    mOverviewTable->setObjectName(QStringLiteral("OverviewTable"));
    mOverviewTable->setColumnCount(2);
    mOverviewTable->setHorizontalHeaderLabels({tr("Metric"), tr("Value")});
    mOverviewTable->horizontalHeader()->setStretchLastSection(true);
    mOverviewTable->setColumnWidth(0, 220);
    mOverviewTable->verticalHeader()->setVisible(false);
    ConfigureDataTableInteraction(mOverviewTable);
    mOverviewTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    overviewLayout->addWidget(mOverviewTable);

    QWidget* binsWidget = new QWidget(mViewTabs);
    QVBoxLayout* binsLayout = new QVBoxLayout(binsWidget);
    binsLayout->setContentsMargins(0, 0, 0, 0);
    binsLayout->setSpacing(0);
    QSplitter* binsSplitter = new QSplitter(Qt::Vertical, binsWidget);
    binsSplitter->setObjectName(QStringLiteral("BinsSplitter"));
    binsSplitter->setChildrenCollapsible(false);
    mBinsGraphWidget = new BinGraphWidget(binsSplitter);
    mBinsGraphWidget->SetSuppressionChangedHandler(
        [this](std::size_t minimumSize, std::size_t maximumSize, bool suppressed)
        {
            UpdateSuppressedBinState(minimumSize, maximumSize, suppressed);
        });
    mBinsTable = new QTableWidget(binsSplitter);
    mBinsTable->setObjectName(QStringLiteral("BinsTable"));
    mBinsTable->setColumnCount(7);
    mBinsTable->setHorizontalHeaderLabels(
        {tr("Range"), tr("Allocations"), tr("Bytes"), tr("Min"), tr("Max"), tr("Mean"), tr("Median")});
    mBinsTable->horizontalHeader()->setStretchLastSection(true);
    mBinsTable->verticalHeader()->setVisible(false);
    ConfigureDataTableInteraction(mBinsTable);
    mBinsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    binsSplitter->addWidget(mBinsGraphWidget);
    binsSplitter->addWidget(mBinsTable);
    binsSplitter->setStretchFactor(0, 3);
    binsSplitter->setStretchFactor(1, 2);
    binsSplitter->setSizes({360, 240});
    binsLayout->addWidget(binsSplitter);

    QWidget* heatmapWidget = new QWidget(mViewTabs);
    QVBoxLayout* heatmapLayout = new QVBoxLayout(heatmapWidget);
    heatmapLayout->setContentsMargins(0, 0, 0, 0);
    mHeatmapLabel = new QLabel(tr("Heatmap view coming later."), heatmapWidget);
    mHeatmapLabel->setObjectName(QStringLiteral("SummaryPanel"));
    mHeatmapLabel->setAlignment(Qt::AlignCenter);
    heatmapLayout->addWidget(mHeatmapLabel);

    mViewTabs->addTab(overviewWidget, tr("Overview"));
    mViewTabs->addTab(binsWidget, tr("Bins"));
    mViewTabs->addTab(heatmapWidget, tr("Heatmap"));

    layout->addWidget(mViewTabs, 1);
    setCentralWidget(centralWidget);
    UpdateStreamingSummary({});
    CreateMenus();
}


//------------------------------------------------------------------------------
// MainWindow
//------------------------------------------------------------------------------
//  Stop background streaming before destroying the main window.
//------------------------------------------------------------------------------
MainWindow::~MainWindow()
{
    StopStreaming();
}


//------------------------------------------------------------------------------
// CreateMenus
//------------------------------------------------------------------------------
//  Create the main window menu actions and connect them to their handlers.
//------------------------------------------------------------------------------
void MainWindow::CreateMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    mStreamingAction = fileMenu->addAction(tr("Start Streaming"));
    connect(mStreamingAction, &QAction::triggered, this, [this]
    {
        ToggleStreaming();
    });

    fileMenu->addSeparator();
    mExitAction = fileMenu->addAction(tr("E&xit"));
    mExitAction->setShortcut(QKeySequence::Quit);
    connect(mExitAction, &QAction::triggered, this, []
    {
        QApplication::quit();
    });

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    mAboutAction = helpMenu->addAction(tr("&About"));
    connect(mAboutAction, &QAction::triggered, this, [this]
    {
        ShowAboutDialog();
    });
}


//------------------------------------------------------------------------------
// StartStreaming
//------------------------------------------------------------------------------
//  Prompt for a stream endpoint and start listening for heap allocation data.
//------------------------------------------------------------------------------
void MainWindow::StartStreaming()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("StreamingDialog"));
    dialog.setWindowTitle(tr("Start Streaming"));
    dialog.setMinimumWidth(420);

    QVBoxLayout* dialogLayout = new QVBoxLayout(&dialog);
    dialogLayout->setContentsMargins(22, 20, 22, 18);
    dialogLayout->setSpacing(14);

    QLabel* titleLabel = new QLabel(tr("Start Streaming"), &dialog);
    titleLabel->setObjectName(QStringLiteral("DialogTitle"));

    QLabel* subtitleLabel = new QLabel(tr("Choose the endpoint where allocation telemetry will be received."), &dialog);
    subtitleLabel->setObjectName(QStringLiteral("DialogSubtitle"));
    subtitleLabel->setWordWrap(true);

    QFrame* panelFrame = new QFrame(&dialog);
    panelFrame->setObjectName(QStringLiteral("DialogPanel"));
    QFormLayout* formLayout = new QFormLayout(panelFrame);
    formLayout->setContentsMargins(12, 14, 12, 12);
    formLayout->setSpacing(10);

    QLineEdit* addressEdit = new QLineEdit(QStringLiteral("127.0.0.1"), &dialog);
    QSpinBox* portSpinBox = new QSpinBox(&dialog);
    portSpinBox->setRange(1, 65535);
    portSpinBox->setValue(47329);

    QDialogButtonBox* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        &dialog);

    formLayout->addRow(tr("IP address"), addressEdit);
    formLayout->addRow(tr("Port"), portSpinBox);
    dialogLayout->addWidget(titleLabel);
    dialogLayout->addWidget(subtitleLabel);
    dialogLayout->addWidget(panelFrame);
    dialogLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    std::string errorMessage;
    const std::string address = addressEdit->text().toStdString();
    const auto port = static_cast<std::uint16_t>(portSpinBox->value());
    if (!mAllocationStreamReader.Start(address, port, errorMessage))
    {
        QMessageBox::warning(this, tr("Start Streaming"), QString::fromStdString(errorMessage));
        return;
    }

    mStreamingAction->setText(tr("Stop Streaming"));
}


//------------------------------------------------------------------------------
// StopStreaming
//------------------------------------------------------------------------------
//  Stop listening for heap allocation stream data.
//------------------------------------------------------------------------------
void MainWindow::StopStreaming()
{
    if (!mAllocationStreamReader.IsActive())
    {
        return;
    }

    mAllocationStreamReader.Stop();
    mStreamingAction->setText(tr("Start Streaming"));
}


//------------------------------------------------------------------------------
// ToggleStreaming
//------------------------------------------------------------------------------
//  Start or stop heap allocation stream listening.
//------------------------------------------------------------------------------
void MainWindow::ToggleStreaming()
{
    if (mAllocationStreamReader.IsActive())
    {
        StopStreaming();
        return;
    }

    StartStreaming();
}


//------------------------------------------------------------------------------
// CalculateAddressRangeEnd
//------------------------------------------------------------------------------
//  Calculate the exclusive end address for a streamed allocation range.
//------------------------------------------------------------------------------
std::uintptr_t MainWindow::CalculateAddressRangeEnd(const StreamedAddressRange& range) const
{
    if (!range.mHasAllocations)
    {
        return 0;
    }

    const std::uintptr_t maximumAddress = std::numeric_limits<std::uintptr_t>::max();
    if (range.mHighestAddressAllocationSize <= maximumAddress - range.mHighestAddress)
    {
        return range.mHighestAddress + range.mHighestAddressAllocationSize;
    }

    return maximumAddress;
}


//------------------------------------------------------------------------------
// BuildAddressRangeText
//------------------------------------------------------------------------------
//  Build display text for a virtual memory address range.
//------------------------------------------------------------------------------
std::string MainWindow::BuildAddressRangeText(const StreamedAddressRange& range) const
{
    if (!range.mHasAllocations)
    {
        return "none";
    }

    const std::uintptr_t rangeEndAddress = CalculateAddressRangeEnd(range);

    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << range.mLowestAddress
           << " - 0x" << rangeEndAddress
           << std::dec << " (" << (rangeEndAddress - range.mLowestAddress) << " bytes)";

    return output.str();
}


//------------------------------------------------------------------------------
// BuildAddressText
//------------------------------------------------------------------------------
//  Build display text for a virtual memory address.
//------------------------------------------------------------------------------
std::string MainWindow::BuildAddressText(std::uintptr_t address, bool hasAddress, int minimumDigits) const
{
    if (!hasAddress)
    {
        return "none";
    }

    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex;
    if (minimumDigits > 0)
    {
        output << std::setfill('0') << std::setw(minimumDigits);
    }

    output << address;
    return output.str();
}


//------------------------------------------------------------------------------
// BuildHexValueText
//------------------------------------------------------------------------------
//  Build display text for a hexadecimal value.
//------------------------------------------------------------------------------
std::string MainWindow::BuildHexValueText(std::size_t value, bool hasValue) const
{
    if (!hasValue)
    {
        return "none";
    }

    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << value;
    return output.str();
}


//------------------------------------------------------------------------------
// BuildByteCountText
//------------------------------------------------------------------------------
//  Build display text for a byte count.
//------------------------------------------------------------------------------
std::string MainWindow::BuildByteCountText(std::size_t byteCount) const
{
    std::ostringstream output;
    output << BuildCommaSeparatedIntegerText(byteCount) << " bytes";

    return output.str();
}


//------------------------------------------------------------------------------
// UpdateStreamingSummary
//------------------------------------------------------------------------------
//  Display allocation count and size information from the latest snapshot.
//------------------------------------------------------------------------------
void MainWindow::UpdateStreamingSummary(const StreamedAllocationSummary& summary)
{
    UpdateOverviewView(summary);
    UpdateBinsView(summary);
}


//------------------------------------------------------------------------------
// UpdateOverviewView
//------------------------------------------------------------------------------
//  Update the overview table with combined allocation counters.
//------------------------------------------------------------------------------
void MainWindow::UpdateOverviewView(const StreamedAllocationSummary& summary)
{
    using HeapVisualizer::AllocationHooks::AllocationType;

    constexpr std::array allocationDisplayTypes = {
        AllocationType::Malloc,
        AllocationType::Calloc,
        AllocationType::Realloc,
        AllocationType::ReallocArray,
        AllocationType::AlignedAlloc,
        AllocationType::Memalign,
        AllocationType::PosixMemalign,
        AllocationType::Valloc,
        AllocationType::PValloc};
    constexpr std::array deallocationDisplayTypes = {
        AllocationType::Free,
        AllocationType::Realloc,
        AllocationType::ReallocArray,
        AllocationType::CFree};
    constexpr int baseRowCount = 12;
    const int rowCount = baseRowCount +
                         static_cast<int>(allocationDisplayTypes.size()) +
                         static_cast<int>(deallocationDisplayTypes.size());
    mOverviewTable->setRowCount(rowCount);

    const std::uintptr_t rangeStart = summary.mAddressRange.mLowestAddress;
    const std::uintptr_t rangeEnd = CalculateAddressRangeEnd(summary.mAddressRange);
    const int addressDigitCount = summary.mAddressRange.mHasAllocations
                                      ? std::max(HexDigitCount(rangeStart), HexDigitCount(rangeEnd))
                                      : 0;
    const std::size_t addressRange = summary.mAddressRange.mHasAllocations
                                         ? static_cast<std::size_t>(rangeEnd - rangeStart)
                                         : 0;
    const QString pageSizeText = summary.mHasPageSize
                                     ? QString::fromStdString(BuildByteTickLabel(summary.mPageSize))
                                     : QStringLiteral("?");
    const double pageEfficiency = (summary.mAllocatedPages > 0)
                                      ? static_cast<double>(summary.mAllocationBytes) /
                                            (static_cast<double>(summary.mAllocatedPages) *
                                             static_cast<double>(summary.mPageSize))
                                      : 0.0;

    int row = 0;
    SetTableRow(
        mOverviewTable,
        row++,
        tr("Allocation Count"),
        QString::fromStdString(BuildCommaSeparatedIntegerText(summary.mAllocationCount)));
    SetTableRow(mOverviewTable, row++, tr("Allocation Size"), QString::fromStdString(BuildByteCountText(summary.mAllocationBytes)));
    SetTableRow(mOverviewTable, row++, tr("Address Start"), QString::fromStdString(BuildAddressText(rangeStart, summary.mAddressRange.mHasAllocations, addressDigitCount)), true);
    SetTableRow(mOverviewTable, row++, tr("Address End"), QString::fromStdString(BuildAddressText(rangeEnd, summary.mAddressRange.mHasAllocations, addressDigitCount)), true);
    SetTableRow(mOverviewTable, row++, tr("Address Range"), QString::fromStdString(BuildHexValueText(addressRange, summary.mAddressRange.mHasAllocations)), true);
    SetTableRow(
        mOverviewTable,
        row++,
        tr("Allocated Pages [%1]").arg(pageSizeText),
        QString::fromStdString(BuildCommaSeparatedIntegerText(summary.mAllocatedPages)));
    SetTableRow(
        mOverviewTable,
        row++,
        QStringLiteral("    ") + tr("Page Efficiency"),
        QString::fromStdString(BuildPercentageText(pageEfficiency)),
        false,
        summary.mAllocatedPages == 0);
    SetTableRow(mOverviewTable, row++, tr("Tracked Allocations"), QString::fromStdString(BuildCommaSeparatedIntegerText(summary.mTrackedAllocations)));

    for (AllocationType type : allocationDisplayTypes)
    {
        const std::uint64_t operationCount = summary.mTrackedAllocationsByType[OperationTypeIndex(type)];
        SetTableRow(
            mOverviewTable,
            row++,
            QStringLiteral("    ") + AllocationOperationName(type),
            QString::fromStdString(BuildCommaSeparatedIntegerText(operationCount)),
            false,
            operationCount == 0);
    }

    SetTableRow(mOverviewTable, row++, tr("Tracked Deallocations"), QString::fromStdString(BuildCommaSeparatedIntegerText(summary.mTrackedDeallocations)));

    for (AllocationType type : deallocationDisplayTypes)
    {
        const std::uint64_t operationCount = summary.mTrackedDeallocationsByType[OperationTypeIndex(type)];
        SetTableRow(
            mOverviewTable,
            row++,
            QStringLiteral("    ") + AllocationOperationName(type),
            QString::fromStdString(BuildCommaSeparatedIntegerText(operationCount)),
            false,
            operationCount == 0);
    }

    SetTableRow(mOverviewTable, row++, tr("Untracked Allocations"), QString::fromStdString(BuildCommaSeparatedIntegerText(summary.mUntrackedAllocations)));
    SetTableRow(mOverviewTable, row++, tr("Untracked Deallocations"), QString::fromStdString(BuildCommaSeparatedIntegerText(summary.mUntrackedDeallocations)));
    SetTableRow(mOverviewTable, row++, tr("Last stream payload size"), QString::fromStdString(BuildByteCountText(summary.mStreamPayloadSize)));

    mOverviewTable->resizeRowsToContents();
}


//------------------------------------------------------------------------------
// UpdateBinsView
//------------------------------------------------------------------------------
//  Update the bin table and graph with combined allocation records.
//------------------------------------------------------------------------------
void MainWindow::UpdateBinsView(const StreamedAllocationSummary& summary)
{
    mCurrentBins = summary.mBins;
    for (StreamedAllocationBin& bin : mCurrentBins)
    {
        const auto binKey = std::make_pair(bin.mMinimumSize, bin.mMaximumSize);
        bin.mSuppressed = mSuppressedBins.contains(binKey);
    }

    mBinsGraphWidget->SetBins(mCurrentBins);
    mBinsTable->setRowCount(static_cast<int>(mCurrentBins.size()));

    for (int row = 0; row < static_cast<int>(mCurrentBins.size()); row++)
    {
        const StreamedAllocationBin& bin = mCurrentBins[static_cast<std::size_t>(row)];
        const std::size_t meanBytes = bin.mAllocationCount == 0
                                          ? 0
                                          : bin.mAllocationBytes / bin.mAllocationCount;
        const std::size_t medianBytes = CalculateMedianAllocationSize(bin.mAllocationSizes);

        std::ostringstream rangeText;
        rangeText << BuildBinLimitLabel(bin.mMinimumSize) << "-"
                  << BuildBinLimitLabel(bin.mMaximumSize);

        QTableWidgetItem* rangeItem = new QTableWidgetItem(QString::fromStdString(rangeText.str()));
        QTableWidgetItem* countItem = new QTableWidgetItem(
            QString::fromStdString(BuildCommaSeparatedIntegerText(bin.mAllocationCount)));
        QTableWidgetItem* bytesItem = new QTableWidgetItem(
            QString::fromStdString(BuildCommaSeparatedIntegerText(bin.mAllocationBytes)));
        QTableWidgetItem* minimumItem = new QTableWidgetItem(
            QString::fromStdString(BuildCommaSeparatedIntegerText(bin.mMinimumAllocationSize)));
        QTableWidgetItem* maximumItem = new QTableWidgetItem(
            QString::fromStdString(BuildCommaSeparatedIntegerText(bin.mMaximumAllocationSize)));
        QTableWidgetItem* meanItem = new QTableWidgetItem(
            QString::fromStdString(BuildCommaSeparatedIntegerText(meanBytes)));
        QTableWidgetItem* medianItem = new QTableWidgetItem(
            QString::fromStdString(BuildCommaSeparatedIntegerText(medianBytes)));

        rangeItem->setFlags(rangeItem->flags() & ~Qt::ItemIsEditable);
        countItem->setFlags(countItem->flags() & ~Qt::ItemIsEditable);
        bytesItem->setFlags(bytesItem->flags() & ~Qt::ItemIsEditable);
        minimumItem->setFlags(minimumItem->flags() & ~Qt::ItemIsEditable);
        maximumItem->setFlags(maximumItem->flags() & ~Qt::ItemIsEditable);
        meanItem->setFlags(meanItem->flags() & ~Qt::ItemIsEditable);
        medianItem->setFlags(medianItem->flags() & ~Qt::ItemIsEditable);

        if (bin.mAllocationCount == 0)
        {
            const QBrush emptyBinTextBrush(QColor(QStringLiteral("#9aa6b2")));
            rangeItem->setForeground(emptyBinTextBrush);
            countItem->setForeground(emptyBinTextBrush);
            bytesItem->setForeground(emptyBinTextBrush);
            minimumItem->setForeground(emptyBinTextBrush);
            maximumItem->setForeground(emptyBinTextBrush);
            meanItem->setForeground(emptyBinTextBrush);
            medianItem->setForeground(emptyBinTextBrush);
        }

        mBinsTable->setItem(row, 0, rangeItem);
        mBinsTable->setItem(row, 1, countItem);
        mBinsTable->setItem(row, 2, bytesItem);
        mBinsTable->setItem(row, 3, minimumItem);
        mBinsTable->setItem(row, 4, maximumItem);
        mBinsTable->setItem(row, 5, meanItem);
        mBinsTable->setItem(row, 6, medianItem);
    }

    mBinsTable->resizeRowsToContents();
}


//------------------------------------------------------------------------------
// UpdateSuppressedBinState
//------------------------------------------------------------------------------
//  Update whether a bin should be hidden from the chart scaling and rendering.
//------------------------------------------------------------------------------
void MainWindow::UpdateSuppressedBinState(std::size_t minimumSize, std::size_t maximumSize, bool suppressed)
{
    const auto binKey = std::make_pair(minimumSize, maximumSize);
    if (suppressed)
    {
        mSuppressedBins.insert(binKey);
    }
    else
    {
        mSuppressedBins.erase(binKey);
    }

    for (StreamedAllocationBin& bin : mCurrentBins)
    {
        if ((bin.mMinimumSize == minimumSize) && (bin.mMaximumSize == maximumSize))
        {
            bin.mSuppressed = suppressed;
            break;
        }
    }

    mBinsGraphWidget->SetBins(mCurrentBins);
}


//------------------------------------------------------------------------------
// ShowAboutDialog
//------------------------------------------------------------------------------
//  Show the application version and build date in the About dialog.
//------------------------------------------------------------------------------
void MainWindow::ShowAboutDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("About HeapVisualizer"));
    dialog.setMinimumWidth(360);

    QVBoxLayout* dialogLayout = new QVBoxLayout(&dialog);
    dialogLayout->setContentsMargins(22, 20, 22, 18);
    dialogLayout->setSpacing(14);

    QLabel* titleLabel = new QLabel(tr("HeapVisualizer"), &dialog);
    titleLabel->setObjectName(QStringLiteral("DialogTitle"));

    const std::string message = std::string("Version: ") + HeapVisualizer::BuildVersion +
                                "\nBuild date: " + HeapVisualizer::BuildDate;
    QLabel* detailsLabel = new QLabel(QString::fromStdString(message), &dialog);
    detailsLabel->setObjectName(QStringLiteral("DialogSubtitle"));

    QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    QPushButton* okButton = buttonBox->button(QDialogButtonBox::Ok);
    if (okButton != nullptr)
    {
        okButton->setDefault(true);
    }

    dialogLayout->addWidget(titleLabel);
    dialogLayout->addWidget(detailsLabel);
    dialogLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);

    dialog.exec();
}
