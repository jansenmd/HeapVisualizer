#include "mainwindow.hpp"

#include "build_info.hpp"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
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
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QString>
#include <QPushButton>
#include <QShortcut>
#include <QSizePolicy>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace
{

constexpr std::uint64_t HeatmapBytesPerPixel = 256;

//------------------------------------------------------------------------------
// AllocationOperationName
//------------------------------------------------------------------------------
//  Return the display name for a tracked allocation type.
//------------------------------------------------------------------------------
QString AllocationOperationName(HeapVisualizer::AllocationHooks::TrackedAllocationType type)
{
    using HeapVisualizer::AllocationHooks::TrackedAllocationType;

    switch (type)
    {
        case TrackedAllocationType::Malloc:
            return QStringLiteral("malloc");
        case TrackedAllocationType::Calloc:
            return QStringLiteral("calloc");
        case TrackedAllocationType::Realloc:
            return QStringLiteral("realloc");
        case TrackedAllocationType::ReallocArray:
            return QStringLiteral("reallocarray");
        case TrackedAllocationType::PosixMemalign:
            return QStringLiteral("posix_memalign");
        case TrackedAllocationType::AlignedAlloc:
            return QStringLiteral("aligned_alloc");
        case TrackedAllocationType::Memalign:
            return QStringLiteral("memalign");
        case TrackedAllocationType::Valloc:
            return QStringLiteral("valloc");
        case TrackedAllocationType::PValloc:
            return QStringLiteral("pvalloc");
        case TrackedAllocationType::Count:
        default:
            break;
    }

    return QStringLiteral("unknown");
}


//------------------------------------------------------------------------------
// DeallocationOperationName
//------------------------------------------------------------------------------
//  Return the display name for a tracked deallocation type.
//------------------------------------------------------------------------------
QString DeallocationOperationName(HeapVisualizer::AllocationHooks::TrackedDeallocationType type)
{
    using HeapVisualizer::AllocationHooks::TrackedDeallocationType;

    switch (type)
    {
        case TrackedDeallocationType::Free:
            return QStringLiteral("free");
        case TrackedDeallocationType::Realloc:
            return QStringLiteral("realloc");
        case TrackedDeallocationType::ReallocArray:
            return QStringLiteral("reallocarray");
        case TrackedDeallocationType::CFree:
            return QStringLiteral("cfree");
        case TrackedDeallocationType::Count:
        default:
            break;
    }

    return QStringLiteral("unknown");
}


//------------------------------------------------------------------------------
// TrackedAllocationTypeIndex
//------------------------------------------------------------------------------
//  Convert a tracked allocation type into its summary count-array index.
//------------------------------------------------------------------------------
std::size_t TrackedAllocationTypeIndex(HeapVisualizer::AllocationHooks::TrackedAllocationType type)
{
    return static_cast<std::size_t>(type);
}


//------------------------------------------------------------------------------
// TrackedDeallocationTypeIndex
//------------------------------------------------------------------------------
//  Convert a tracked deallocation type into its summary count-array index.
//------------------------------------------------------------------------------
std::size_t TrackedDeallocationTypeIndex(HeapVisualizer::AllocationHooks::TrackedDeallocationType type)
{
    return static_cast<std::size_t>(type);
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
// BuildDurationText
//------------------------------------------------------------------------------
//  Build compact text for an elapsed duration.
//------------------------------------------------------------------------------
std::string BuildDurationText(std::uint64_t microseconds)
{
    std::ostringstream output;
    if (microseconds < 1000)
    {
        output << std::setw(7) << microseconds << " us";
        return output.str();
    }

    output << std::fixed << std::setprecision(2) << std::setw(7)
           << (static_cast<double>(microseconds) / 1000.0) << " ms";
    return output.str();
}


//------------------------------------------------------------------------------
// BuildBusyPercentageText
//------------------------------------------------------------------------------
//  Build fixed-width text for the recent processing duty cycle.
//------------------------------------------------------------------------------
std::string BuildBusyPercentageText(double busyPercentage)
{
    std::ostringstream output;
    output << std::fixed << std::setprecision(1) << std::setw(5) << busyPercentage << "%";
    return output.str();
}


//------------------------------------------------------------------------------
// EnsureTableItem
//------------------------------------------------------------------------------
//  Return an existing table item or create it if this cell has not been used.
//------------------------------------------------------------------------------
QTableWidgetItem* EnsureTableItem(QTableWidget* tableWidget, int row, int column)
{
    QTableWidgetItem* item = tableWidget->item(row, column);
    if (item != nullptr)
    {
        return item;
    }

    item = new QTableWidgetItem();
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    tableWidget->setItem(row, column, item);
    return item;
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
// HeatmapImageWidget
//------------------------------------------------------------------------------
//  Construct a widget that paints heatmap pixels one-to-one.
//------------------------------------------------------------------------------
HeatmapImageWidget::HeatmapImageWidget(QWidget* parent)
    : QWidget(parent)
{
    QSizePolicy sizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sizePolicy.setHeightForWidth(true);
    setSizePolicy(sizePolicy);
    setMinimumHeight(1);
}


//------------------------------------------------------------------------------
// SetHeatmap
//------------------------------------------------------------------------------
//  Store packed ARGB heatmap pixels for later one-to-one layout and painting.
//------------------------------------------------------------------------------
void HeatmapImageWidget::SetHeatmap(const std::vector<std::uint32_t>& pixels)
{
    mPixels = pixels;
    mImage = QImage();
    mImagePixelsPerRow = 0;
    RebuildImageForWidth(width());
    UpdateDimensionsLabel();
    updateGeometry();
    update();
}


//------------------------------------------------------------------------------
// SetDimensionsLabel
//------------------------------------------------------------------------------
//  Set the label that describes the current heatmap image dimensions.
//------------------------------------------------------------------------------
void HeatmapImageWidget::SetDimensionsLabel(QLabel* label)
{
    mDimensionsLabel = label;
    UpdateDimensionsLabel();
}


//------------------------------------------------------------------------------
// SetRowBytesLabel
//------------------------------------------------------------------------------
//  Set the shared label that describes how much address space one image row covers.
//------------------------------------------------------------------------------
void HeatmapImageWidget::SetRowBytesLabel(QLabel* label)
{
    mRowBytesLabel = label;
    UpdateRowBytesLabel();
}


//------------------------------------------------------------------------------
// hasHeightForWidth
//------------------------------------------------------------------------------
//  Return whether the image height depends on the available display width.
//------------------------------------------------------------------------------
bool HeatmapImageWidget::hasHeightForWidth() const
{
    return true;
}


//------------------------------------------------------------------------------
// heightForWidth
//------------------------------------------------------------------------------
//  Return the number of one-to-one pixel rows needed for the current width.
//------------------------------------------------------------------------------
int HeatmapImageWidget::heightForWidth(int width) const
{
    if (mPixels.empty())
    {
        return 1;
    }

    const std::size_t pixelsPerRow = PixelsPerRowForWidth(width);
    return static_cast<int>((mPixels.size() + pixelsPerRow - 1) / pixelsPerRow);
}


//------------------------------------------------------------------------------
// sizeHint
//------------------------------------------------------------------------------
//  Return a default size for layout calculations.
//------------------------------------------------------------------------------
QSize HeatmapImageWidget::sizeHint() const
{
    constexpr int defaultWidth = 512;
    return QSize(defaultWidth, heightForWidth(defaultWidth));
}


//------------------------------------------------------------------------------
// paintEvent
//------------------------------------------------------------------------------
//  Paint the heatmap image without scaling pixels.
//------------------------------------------------------------------------------
void HeatmapImageWidget::paintEvent(QPaintEvent* event)
{
    static_cast<void>(event);

    RebuildImageForWidth(width());

    QPainter painter(this);
    painter.fillRect(rect(), QColor(QStringLiteral("#ffffff")));

    if (!mImage.isNull())
    {
        painter.drawImage(0, 0, mImage);
    }
}


//------------------------------------------------------------------------------
// resizeEvent
//------------------------------------------------------------------------------
//  Reflow the heatmap when the available pixel width crosses a power of two.
//------------------------------------------------------------------------------
void HeatmapImageWidget::resizeEvent(QResizeEvent* event)
{
    RebuildImageForWidth(event->size().width());
    UpdateDimensionsLabel();
    UpdateRowBytesLabel();
    updateGeometry();
    QWidget::resizeEvent(event);
}


//------------------------------------------------------------------------------
// PixelsPerRowForWidth
//------------------------------------------------------------------------------
//  Return the largest power-of-two row width that fits the available pixels.
//------------------------------------------------------------------------------
std::size_t HeatmapImageWidget::PixelsPerRowForWidth(int width) const
{
    std::size_t pixelsPerRow = 1;
    const std::size_t availableWidth = static_cast<std::size_t>(std::max(1, width));
    while ((pixelsPerRow * 2) <= availableWidth)
    {
        pixelsPerRow *= 2;
    }

    return pixelsPerRow;
}


//------------------------------------------------------------------------------
// UpdateDimensionsLabel
//------------------------------------------------------------------------------
//  Display the current reflowed image dimensions.
//------------------------------------------------------------------------------
void HeatmapImageWidget::UpdateDimensionsLabel()
{
    if (mDimensionsLabel == nullptr)
    {
        return;
    }

    if (mImage.isNull())
    {
        mDimensionsLabel->setText(QStringLiteral("Image 0 x 0 px"));
        return;
    }

    mDimensionsLabel->setText(
        QStringLiteral("Image %1 x %2 px")
            .arg(mImage.width())
            .arg(mImage.height()));
}


//------------------------------------------------------------------------------
// UpdateRowBytesLabel
//------------------------------------------------------------------------------
//  Display the current number of bytes represented by one image row.
//------------------------------------------------------------------------------
void HeatmapImageWidget::UpdateRowBytesLabel()
{
    if (mRowBytesLabel == nullptr)
    {
        return;
    }

    if (mImage.isNull())
    {
        mRowBytesLabel->setText(QStringLiteral("Bytes per row: 0"));
        return;
    }

    const std::uint64_t bytesPerRow =
        static_cast<std::uint64_t>(mImage.width()) * HeatmapBytesPerPixel;
    mRowBytesLabel->setText(
        QStringLiteral("Bytes per row: %1")
            .arg(QString::fromStdString(BuildBinLimitLabel(static_cast<std::size_t>(bytesPerRow)))));
}


//------------------------------------------------------------------------------
// RebuildImageForWidth
//------------------------------------------------------------------------------
//  Reflow heatmap pixels into a power-of-two-wide image for the current width.
//------------------------------------------------------------------------------
void HeatmapImageWidget::RebuildImageForWidth(int width)
{
    if (mPixels.empty())
    {
        mImage = QImage();
        mImagePixelsPerRow = 0;
        return;
    }

    const std::size_t pixelsPerRow = PixelsPerRowForWidth(width);
    if ((!mImage.isNull()) && (mImagePixelsPerRow == pixelsPerRow))
    {
        return;
    }

    const std::size_t imageHeight = (mPixels.size() + pixelsPerRow - 1) / pixelsPerRow;
    mImage = QImage(
        static_cast<int>(pixelsPerRow),
        static_cast<int>(imageHeight),
        QImage::Format_ARGB32);
    mImage.fill(0xffffffff);
    std::memcpy(mImage.bits(), mPixels.data(), mPixels.size() * sizeof(std::uint32_t));
    mImagePixelsPerRow = pixelsPerRow;
    UpdateRowBytesLabel();
}


//------------------------------------------------------------------------------
// BusyIndicatorBar
//------------------------------------------------------------------------------
//  Construct the compact processing duty-cycle bar.
//------------------------------------------------------------------------------
BusyIndicatorBar::BusyIndicatorBar(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(220, 12);
    setToolTip(QStringLiteral("Socket reader, parser, view update, and idle time since the previous summary."));
}


//------------------------------------------------------------------------------
// SetTimings
//------------------------------------------------------------------------------
//  Update the duration values rendered by the processing duty-cycle bar.
//------------------------------------------------------------------------------
void BusyIndicatorBar::SetTimings(std::uint64_t socketReadMicroseconds,
                                  std::uint64_t parserMicroseconds,
                                  std::uint64_t viewMicroseconds,
                                  std::uint64_t idleMicroseconds,
                                  bool hasTimingFrame)
{
    mSocketReadMicroseconds = socketReadMicroseconds;
    mParserMicroseconds = parserMicroseconds;
    mViewMicroseconds = viewMicroseconds;
    mIdleMicroseconds = idleMicroseconds;
    mHasTimingFrame = hasTimingFrame;
    update();
}


//------------------------------------------------------------------------------
// paintEvent
//------------------------------------------------------------------------------
//  Paint socket reader, parser, view, and idle time as proportional horizontal segments.
//------------------------------------------------------------------------------
void BusyIndicatorBar::paintEvent(QPaintEvent* event)
{
    static_cast<void>(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRect barRect = rect().adjusted(0, 2, 0, -2);
    painter.setPen(QColor(QStringLiteral("#cfd8e3")));
    painter.setBrush(QColor(QStringLiteral("#f8fafc")));
    painter.drawRoundedRect(barRect, 2, 2);

    if (!mHasTimingFrame)
    {
        return;
    }

    const std::uint64_t totalMicroseconds =
        mSocketReadMicroseconds + mParserMicroseconds + mViewMicroseconds + mIdleMicroseconds;
    if (totalMicroseconds == 0)
    {
        return;
    }

    const std::array segmentColors = {
        QColor(QStringLiteral("#2f80ed")),
        QColor(QStringLiteral("#ef4444")),
        QColor(QStringLiteral("#f59e0b")),
        QColor(QStringLiteral("#22c55e"))};
    const std::array segmentValues = {
        mSocketReadMicroseconds,
        mParserMicroseconds,
        mViewMicroseconds,
        mIdleMicroseconds};

    int segmentLeft = barRect.left() + 1;
    const int contentWidth = std::max(0, barRect.width() - 2);
    const int contentTop = barRect.top() + 1;
    const int contentHeight = std::max(0, barRect.height() - 2);
    int usedWidth = 0;

    painter.setPen(Qt::NoPen);
    for (std::size_t segmentIndex = 0; segmentIndex < segmentValues.size(); segmentIndex++)
    {
        int segmentWidth = 0;
        if (segmentIndex == (segmentValues.size() - 1))
        {
            segmentWidth = contentWidth - usedWidth;
        }
        else
        {
            segmentWidth = static_cast<int>(
                (static_cast<double>(segmentValues[segmentIndex]) / static_cast<double>(totalMicroseconds)) *
                static_cast<double>(contentWidth));
            if ((segmentValues[segmentIndex] > 0) && (segmentWidth == 0))
            {
                segmentWidth = 1;
            }
        }

        if (segmentWidth <= 0)
        {
            continue;
        }

        painter.setBrush(segmentColors[segmentIndex]);
        painter.drawRect(QRect(segmentLeft, contentTop, segmentWidth, contentHeight));
        segmentLeft += segmentWidth;
        usedWidth += segmentWidth;
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
            maximumAllocations = std::max(maximumAllocations, bin.mAllocationSizes.size());
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
        const std::size_t allocationCount = bin.mSuppressed ? 0 : bin.mAllocationSizes.size();
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
            bin.mAllocationSizes.empty()
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
            opacityEffect->setOpacity(bin.mAllocationSizes.empty() ? 0.45 : 1.0);
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
    mHeatmapScrollArea = new QScrollArea(heatmapWidget);
    mHeatmapScrollArea->setWidgetResizable(true);
    mHeatmapScrollArea->setFrameShape(QFrame::NoFrame);
    mHeatmapContentWidget = new QWidget(mHeatmapScrollArea);
    mHeatmapLayout = new QVBoxLayout(mHeatmapContentWidget);
    mHeatmapLayout->setContentsMargins(0, 0, 0, 0);
    mHeatmapLayout->setSpacing(12);
    mHeatmapScrollArea->setWidget(mHeatmapContentWidget);
    heatmapLayout->addWidget(mHeatmapScrollArea);

    mViewTabs->addTab(overviewWidget, tr("Overview"));
    mViewTabs->addTab(binsWidget, tr("Bins"));
    mViewTabs->addTab(heatmapWidget, tr("Heatmap"));

    layout->addWidget(mViewTabs, 1);
    setCentralWidget(centralWidget);

    mBusyIndicatorBar = new BusyIndicatorBar(this);
    statusBar()->addPermanentWidget(mBusyIndicatorBar);

    mBusyIndicatorLabel = new QLabel(this);
    mBusyIndicatorLabel->setObjectName(QStringLiteral("BusyIndicator"));
    mBusyIndicatorLabel->setTextFormat(Qt::RichText);
    mBusyIndicatorLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mBusyIndicatorLabel->setMinimumWidth(
        mBusyIndicatorLabel->fontMetrics().horizontalAdvance(
            QStringLiteral("Reader 999.99 ms | Parser 999.99 ms | Views 999.99 ms | Idle 999.99 ms | Busy 100.0%")));
    statusBar()->addPermanentWidget(mBusyIndicatorLabel);

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

    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << range.mLowestAddress
           << " - 0x" << range.mHighestAddress
           << std::dec << " (" << (range.mHighestAddress - range.mLowestAddress) << " bytes)";

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
    const auto viewUpdateStartTime = std::chrono::steady_clock::now();

    UpdateOverviewView(summary);
    UpdateBinsView(summary);
    UpdateHeatmapView(summary);

    const std::uint64_t viewUpdateMicroseconds =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - viewUpdateStartTime).count());
    UpdateBusyIndicator(summary, viewUpdateMicroseconds, viewUpdateStartTime);
}


//------------------------------------------------------------------------------
// UpdateBusyIndicator
//------------------------------------------------------------------------------
//  Show recent stream-reader and UI update timings in the status bar.
//------------------------------------------------------------------------------
void MainWindow::UpdateBusyIndicator(const StreamedAllocationSummary& summary,
                                     std::uint64_t viewUpdateMicroseconds,
                                     std::chrono::steady_clock::time_point updateStartTime)
{
    if (mBusyIndicatorLabel == nullptr)
    {
        return;
    }

    const std::uint64_t totalMicroseconds =
        summary.mSocketReadMicroseconds + summary.mParserProcessingMicroseconds + viewUpdateMicroseconds;
    std::uint64_t idleMicroseconds = 0;
    QString idleText = QStringLiteral("     -- ms");
    QString busyText = QStringLiteral(" --.-%");

    if (mHasBusyIndicatorUpdateTime)
    {
        const std::uint64_t elapsedMicroseconds =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    updateStartTime - mLastBusyIndicatorUpdateTime).count());
        idleMicroseconds = (elapsedMicroseconds > totalMicroseconds) ? elapsedMicroseconds - totalMicroseconds : 0;
        const double busyPercentage =
            (elapsedMicroseconds > 0)
                ? (static_cast<double>(totalMicroseconds) * 100.0) / static_cast<double>(elapsedMicroseconds)
                : 0.0;

        idleText = QString::fromStdString(BuildDurationText(idleMicroseconds));
        busyText = QString::fromStdString(BuildBusyPercentageText(std::min(busyPercentage, 999.9)));
    }

    if (mBusyIndicatorBar != nullptr)
    {
        mBusyIndicatorBar->SetTimings(
            summary.mSocketReadMicroseconds,
            summary.mParserProcessingMicroseconds,
            viewUpdateMicroseconds,
            idleMicroseconds,
            mHasBusyIndicatorUpdateTime);
    }

    mLastBusyIndicatorUpdateTime = updateStartTime;
    mHasBusyIndicatorUpdateTime = true;

    const auto durationHtml = [](std::uint64_t microseconds)
    {
        return QString::fromStdString(BuildDurationText(microseconds)).replace(QStringLiteral(" "), QStringLiteral("&nbsp;"));
    };
    const auto textHtml = [](QString text)
    {
        return text.replace(QStringLiteral(" "), QStringLiteral("&nbsp;"));
    };

    mBusyIndicatorLabel->setText(
        tr("<span style=\"color:#2f80ed;\">Reader</span> %1 | "
           "<span style=\"color:#ef4444;\">Parser</span> %2 | "
           "<span style=\"color:#f59e0b;\">Views</span> %3 | "
           "<span style=\"color:#22c55e;\">Idle</span> %4 | Busy %5")
            .arg(durationHtml(summary.mSocketReadMicroseconds))
            .arg(durationHtml(summary.mParserProcessingMicroseconds))
            .arg(durationHtml(viewUpdateMicroseconds))
            .arg(textHtml(idleText))
            .arg(textHtml(busyText)));
}


//------------------------------------------------------------------------------
// UpdateOverviewView
//------------------------------------------------------------------------------
//  Update the overview table with combined allocation counters.
//------------------------------------------------------------------------------
void MainWindow::UpdateOverviewView(const StreamedAllocationSummary& summary)
{
    using HeapVisualizer::AllocationHooks::TrackedAllocationType;
    using HeapVisualizer::AllocationHooks::TrackedDeallocationType;

    constexpr std::array allocationDisplayTypes = {
        TrackedAllocationType::Malloc,
        TrackedAllocationType::Calloc,
        TrackedAllocationType::Realloc,
        TrackedAllocationType::ReallocArray,
        TrackedAllocationType::AlignedAlloc,
        TrackedAllocationType::Memalign,
        TrackedAllocationType::PosixMemalign,
        TrackedAllocationType::Valloc,
        TrackedAllocationType::PValloc};
    constexpr std::array deallocationDisplayTypes = {
        TrackedDeallocationType::Free,
        TrackedDeallocationType::Realloc,
        TrackedDeallocationType::ReallocArray,
        TrackedDeallocationType::CFree};
    constexpr int baseRowCount = 12;
    const int rowCount = baseRowCount +
                         static_cast<int>(allocationDisplayTypes.size()) +
                         static_cast<int>(deallocationDisplayTypes.size());
    mOverviewTable->setRowCount(rowCount);

    const std::uintptr_t rangeStart = summary.mAddressRange.mLowestAddress;
    const std::uintptr_t rangeEnd = summary.mAddressRange.mHighestAddress;
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

    for (TrackedAllocationType type : allocationDisplayTypes)
    {
        const std::uint64_t operationCount = summary.mTrackedAllocationsByType[TrackedAllocationTypeIndex(type)];
        SetTableRow(
            mOverviewTable,
            row++,
            QStringLiteral("    ") + AllocationOperationName(type),
            QString::fromStdString(BuildCommaSeparatedIntegerText(operationCount)),
            false,
            operationCount == 0);
    }

    SetTableRow(mOverviewTable, row++, tr("Tracked Deallocations"), QString::fromStdString(BuildCommaSeparatedIntegerText(summary.mTrackedDeallocations)));

    for (TrackedDeallocationType type : deallocationDisplayTypes)
    {
        const std::uint64_t operationCount = summary.mTrackedDeallocationsByType[TrackedDeallocationTypeIndex(type)];
        SetTableRow(
            mOverviewTable,
            row++,
            QStringLiteral("    ") + DeallocationOperationName(type),
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
        const std::size_t allocationCount = bin.mAllocationSizes.size();

        std::ostringstream rangeText;
        rangeText << BuildBinLimitLabel(bin.mMinimumSize) << "-"
                  << BuildBinLimitLabel(bin.mMaximumSize);

        QTableWidgetItem* rangeItem = EnsureTableItem(mBinsTable, row, 0);
        QTableWidgetItem* countItem = EnsureTableItem(mBinsTable, row, 1);
        QTableWidgetItem* bytesItem = EnsureTableItem(mBinsTable, row, 2);
        QTableWidgetItem* minimumItem = EnsureTableItem(mBinsTable, row, 3);
        QTableWidgetItem* maximumItem = EnsureTableItem(mBinsTable, row, 4);
        QTableWidgetItem* meanItem = EnsureTableItem(mBinsTable, row, 5);
        QTableWidgetItem* medianItem = EnsureTableItem(mBinsTable, row, 6);

        rangeItem->setText(QString::fromStdString(rangeText.str()));
        countItem->setText(QString::fromStdString(BuildCommaSeparatedIntegerText(allocationCount)));
        bytesItem->setText(QString::fromStdString(BuildCommaSeparatedIntegerText(bin.mAllocationBytes)));
        minimumItem->setText(QString::fromStdString(BuildCommaSeparatedIntegerText(bin.mMinimumAllocationSize)));
        maximumItem->setText(QString::fromStdString(BuildCommaSeparatedIntegerText(bin.mMaximumAllocationSize)));
        meanItem->setText(QString::fromStdString(BuildCommaSeparatedIntegerText(bin.mMeanAllocationSize)));
        medianItem->setText(QString::fromStdString(BuildCommaSeparatedIntegerText(bin.mMedianAllocationSize)));

        const QBrush itemTextBrush =
            (allocationCount == 0)
                ? QBrush(QColor(QStringLiteral("#9aa6b2")))
                : mBinsTable->palette().brush(QPalette::Text);

        for (QTableWidgetItem* item : {rangeItem, countItem, bytesItem, minimumItem, maximumItem, meanItem, medianItem})
        {
            item->setForeground(itemTextBrush);
        }
    }

    mBinsTable->resizeRowsToContents();
}


//------------------------------------------------------------------------------
// UpdateHeatmapView
//------------------------------------------------------------------------------
//  Display each allocation heatmap as a labeled, width-responsive image.
//------------------------------------------------------------------------------
void MainWindow::UpdateHeatmapView(const StreamedAllocationSummary& summary)
{
    if (mHeatmapLayout == nullptr)
    {
        return;
    }

    while (QLayoutItem* item = mHeatmapLayout->takeAt(0))
    {
        if (QWidget* widget = item->widget())
        {
            delete widget;
        }
        delete item;
    }

    QLabel* rowBytesLabel = new QLabel(tr("Bytes per row: 0"), mHeatmapContentWidget);
    rowBytesLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mHeatmapLayout->addWidget(rowBytesLabel);

    const std::size_t heatmapCount = std::min(summary.mAllocationGroups.size(), summary.mHeatmaps.size());
    if (heatmapCount == 0)
    {
        QLabel* emptyLabel = new QLabel(tr("No heatmaps available."), mHeatmapContentWidget);
        emptyLabel->setAlignment(Qt::AlignCenter);
        mHeatmapLayout->addWidget(emptyLabel);
        mHeatmapLayout->addStretch(1);
        return;
    }

    for (std::size_t heatmapIndex = 0; heatmapIndex < heatmapCount; heatmapIndex++)
    {
        const StreamedAllocationGroup& group = summary.mAllocationGroups[heatmapIndex];
        const std::vector<std::uint32_t>& heatmap = summary.mHeatmaps[heatmapIndex];
        const std::uint64_t addressSpan = group.mEndAddress - group.mStartAddress;

        QFrame* heatmapFrame = new QFrame(mHeatmapContentWidget);
        heatmapFrame->setObjectName(QStringLiteral("HeatmapEntryFrame"));
        heatmapFrame->setFrameShape(QFrame::StyledPanel);
        heatmapFrame->setFrameShadow(QFrame::Plain);
        heatmapFrame->setLineWidth(1);
        heatmapFrame->setStyleSheet(QStringLiteral(
            "#HeatmapEntryFrame {"
            "  background: #ffffff;"
            "  border: 1px solid #cfd8e3;"
            "  border-radius: 4px;"
            "}"));
        QVBoxLayout* groupLayout = new QVBoxLayout(heatmapFrame);
        groupLayout->setContentsMargins(12, 10, 12, 12);
        groupLayout->setSpacing(6);

        QLabel* titleLabel = new QLabel(
            tr("Heatmap %1").arg(static_cast<unsigned long long>(heatmapIndex + 1)),
            heatmapFrame);
        QFont titleFont = titleLabel->font();
        titleFont.setWeight(QFont::DemiBold);
        titleLabel->setFont(titleFont);

        QLabel* label = new QLabel(heatmapFrame);
        label->setText(
            tr("%1 - %2 | Span %3 | Allocations %4")
                .arg(QString::fromStdString(BuildAddressText(group.mStartAddress, true)))
                .arg(QString::fromStdString(BuildAddressText(group.mEndAddress, true)))
                .arg(QString::fromStdString(BuildBinLimitLabel(static_cast<std::size_t>(addressSpan))))
                .arg(QString::fromStdString(BuildCommaSeparatedIntegerText(group.mAllocationCount))));
        label->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

        QLabel* dimensionsLabel = new QLabel(heatmapFrame);
        dimensionsLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

        HeatmapImageWidget* heatmapWidget = new HeatmapImageWidget(heatmapFrame);
        heatmapWidget->SetHeatmap(heatmap);
        heatmapWidget->SetDimensionsLabel(dimensionsLabel);
        heatmapWidget->SetRowBytesLabel(rowBytesLabel);

        groupLayout->addWidget(titleLabel);
        groupLayout->addWidget(label);
        groupLayout->addWidget(dimensionsLabel);
        groupLayout->addWidget(heatmapWidget);
        mHeatmapLayout->addWidget(heatmapFrame);
    }

    mHeatmapLayout->addStretch(1);
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
