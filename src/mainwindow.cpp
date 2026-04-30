#include "mainwindow.hpp"

#include "build_info.hpp"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QFontDatabase>
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
#include <QString>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
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
// WaitForConnection
//------------------------------------------------------------------------------
//  Wait briefly for a listener socket to become readable.
//------------------------------------------------------------------------------
bool WaitForConnection(NativeSocket listenSocket)
{
#ifdef _WIN32
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenSocket, &readSet);

    timeval timeout = {};
    timeout.tv_usec = 200000;
    return select(0, &readSet, nullptr, nullptr, &timeout) > 0;
#else
    pollfd descriptor = {};
    descriptor.fd = listenSocket;
    descriptor.events = POLLIN;
    return poll(&descriptor, 1, 200) > 0 && (descriptor.revents & POLLIN) != 0;
#endif
}


//------------------------------------------------------------------------------
// ReceiveSocketData
//------------------------------------------------------------------------------
//  Read all stream data from a connected native socket.
//------------------------------------------------------------------------------
std::string ReceiveSocketData(NativeSocket socketValue)
{
    std::string data;
    std::array<char, 8192> buffer = {};

    while (true)
    {
#ifdef _WIN32
        const int bytesRead = recv(socketValue, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        const ssize_t bytesRead = recv(socketValue, buffer.data(), buffer.size(), 0);
#endif
        if (bytesRead <= 0)
        {
            break;
        }

        data.append(buffer.data(), static_cast<std::size_t>(bytesRead));
    }

    return data;
}


//------------------------------------------------------------------------------
// SetTableRow
//------------------------------------------------------------------------------
//  Set a two-column table row using non-editable cells.
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// BuildCommaSeparatedIntegerText
//------------------------------------------------------------------------------
//  Build display text for an integer with comma group separators.
//------------------------------------------------------------------------------
std::string BuildCommaSeparatedIntegerText(unsigned long long value)
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
// SetTableRow
//------------------------------------------------------------------------------
//  Set a two-column table row using non-editable cells.
//------------------------------------------------------------------------------
void SetTableRow(QTableWidget* tableWidget,
                 int row,
                 const QString& name,
                 const QString& value,
                 bool useMonospaceValue = false)
{
    QTableWidgetItem* nameItem = new QTableWidgetItem(name);
    QTableWidgetItem* valueItem = new QTableWidgetItem(value);
    nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
    valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);

    if (useMonospaceValue)
    {
        valueItem->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
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
    setMinimumHeight(220);
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
    update();
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

    QRect chartRect = rect().adjusted(82, 18, -82, -48);
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
        maximumBytes = std::max(maximumBytes, bin.mAllocationBytes);
        maximumAllocations = std::max(maximumAllocations, bin.mAllocationCount);
    }

    if (maximumBytes == 0)
    {
        maximumBytes = 1;
    }

    if (maximumAllocations == 0)
    {
        maximumAllocations = 1;
    }

    const int binCount = static_cast<int>(mBins.size());
    const int slotWidth = std::max(1, chartRect.width() / binCount);
    const int barWidth = std::max(3, slotWidth - 6);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#2f80ed")));

    for (int binIndex = 0; binIndex < binCount; ++binIndex)
    {
        const StreamedAllocationBin& bin = mBins[static_cast<std::size_t>(binIndex)];
        const double normalizedHeight = static_cast<double>(bin.mAllocationBytes) /
                                        static_cast<double>(maximumBytes);
        const int barHeight = static_cast<int>(normalizedHeight * static_cast<double>(chartRect.height()));
        const int x = chartRect.left() + (binIndex * slotWidth) + ((slotWidth - barWidth) / 2);
        const int y = chartRect.bottom() - barHeight + 1;
        painter.drawRect(QRect(x, y, barWidth, barHeight));
    }

    std::vector<QPointF> countPoints;
    countPoints.reserve(mBins.size());

    for (int binIndex = 0; binIndex < binCount; ++binIndex)
    {
        const StreamedAllocationBin& bin = mBins[static_cast<std::size_t>(binIndex)];
        const double normalizedHeight = static_cast<double>(bin.mAllocationCount) /
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

    painter.setPen(QColor(QStringLiteral("#536173")));
    QFont labelFont = painter.font();
    labelFont.setPointSize(std::max(7, labelFont.pointSize() - 2));
    painter.setFont(labelFont);

    painter.setPen(QColor(QStringLiteral("#dc2626")));
    QRect countScaleRect(rect().left() + 8, chartRect.top() - 2, chartRect.left() - rect().left() - 14, 22);
    painter.drawText(
        countScaleRect,
        Qt::AlignRight | Qt::AlignVCenter,
        QString::fromStdString(BuildCommaSeparatedIntegerText(maximumAllocations)));

    painter.setPen(QColor(QStringLiteral("#2f80ed")));
    QRect byteScaleRect(chartRect.right() + 8, chartRect.top() - 2, rect().right() - chartRect.right() - 14, 22);
    painter.drawText(
        byteScaleRect,
        Qt::AlignLeft | Qt::AlignVCenter,
        QString::fromStdString(BuildBinLimitLabel(maximumBytes)));

    painter.setPen(QColor(QStringLiteral("#536173")));
    for (int binIndex = 0; binIndex < binCount; ++binIndex)
    {
        const StreamedAllocationBin& bin = mBins[static_cast<std::size_t>(binIndex)];
        const int labelX = chartRect.left() + (binIndex * slotWidth);
        const QRect labelRect(labelX, chartRect.bottom() + 4, slotWidth, 18);
        painter.drawText(
            labelRect,
            Qt::AlignTop | Qt::AlignHCenter,
            QString::fromStdString(BuildBinLimitLabel(bin.mMaximumSize)));
    }
}


//------------------------------------------------------------------------------
// MainWindow
//------------------------------------------------------------------------------
//  Construct the main application window and initialize its menus.
//------------------------------------------------------------------------------
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("HeapVisualizer"));
    resize(900, 600);
    QWidget* centralWidget = new QWidget(this);
    centralWidget->setObjectName(QStringLiteral("MainSurface"));
    QVBoxLayout* layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(28, 24, 28, 28);
    layout->setSpacing(16);

    QLabel* titleLabel = new QLabel(tr("HeapVisualizer"), centralWidget);
    titleLabel->setObjectName(QStringLiteral("WindowTitle"));

    QLabel* subtitleLabel = new QLabel(tr("Live heap allocation stream summary"), centralWidget);
    subtitleLabel->setObjectName(QStringLiteral("WindowSubtitle"));

    mViewTabs = new QTabWidget(centralWidget);
    mViewTabs->setObjectName(QStringLiteral("MainTabs"));

    QWidget* overviewWidget = new QWidget(mViewTabs);
    QVBoxLayout* overviewLayout = new QVBoxLayout(overviewWidget);
    overviewLayout->setContentsMargins(0, 0, 0, 0);
    mOverviewTable = new QTableWidget(overviewWidget);
    mOverviewTable->setColumnCount(2);
    mOverviewTable->setHorizontalHeaderLabels({tr("Metric"), tr("Value")});
    mOverviewTable->horizontalHeader()->setStretchLastSection(true);
    mOverviewTable->verticalHeader()->setVisible(false);
    mOverviewTable->setSelectionMode(QAbstractItemView::NoSelection);
    mOverviewTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    overviewLayout->addWidget(mOverviewTable);

    QWidget* binsWidget = new QWidget(mViewTabs);
    QVBoxLayout* binsLayout = new QVBoxLayout(binsWidget);
    binsLayout->setContentsMargins(0, 0, 0, 0);
    binsLayout->setSpacing(12);
    mBinsGraphWidget = new BinGraphWidget(binsWidget);
    mBinsTable = new QTableWidget(binsWidget);
    mBinsTable->setColumnCount(6);
    mBinsTable->setHorizontalHeaderLabels(
        {tr("Range"), tr("Allocations"), tr("Bytes"), tr("Min"), tr("Max"), tr("Average")});
    mBinsTable->horizontalHeader()->setStretchLastSection(true);
    mBinsTable->verticalHeader()->setVisible(false);
    mBinsTable->setSelectionMode(QAbstractItemView::NoSelection);
    mBinsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    binsLayout->addWidget(mBinsGraphWidget);
    binsLayout->addWidget(mBinsTable, 1);

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

    layout->addWidget(titleLabel);
    layout->addWidget(subtitleLabel);
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
    if (!OpenStreamingListener(address, port, errorMessage))
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
    if (!mStreamingActive.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    CloseSocket(ToNativeSocket(mStreamingListenSocket));
    mStreamingListenSocket = 0;

    if (mStreamingThread.joinable())
    {
        mStreamingThread.join();
    }

    ShutdownSockets();
    mStreamingAction->setText(tr("Start Streaming"));
}


//------------------------------------------------------------------------------
// ToggleStreaming
//------------------------------------------------------------------------------
//  Start or stop heap allocation stream listening.
//------------------------------------------------------------------------------
void MainWindow::ToggleStreaming()
{
    if (mStreamingActive.load(std::memory_order_acquire))
    {
        StopStreaming();
        return;
    }

    StartStreaming();
}


//------------------------------------------------------------------------------
// OpenStreamingListener
//------------------------------------------------------------------------------
//  Open a native listener socket and start the streaming listener thread.
//------------------------------------------------------------------------------
bool MainWindow::OpenStreamingListener(const std::string& address, std::uint16_t port, std::string& errorMessage)
{
    if (!InitializeSockets(errorMessage))
    {
        return false;
    }

    const NativeSocket listenSocket = CreateListenSocket(address, port, errorMessage);
    if (listenSocket == InvalidSocket)
    {
        ShutdownSockets();
        return false;
    }

    mStreamingListenSocket = FromNativeSocket(listenSocket);
    mStreamingActive.store(true, std::memory_order_release);
    mStreamingThread = std::thread([this, listenSocket]
    {
        StreamingThreadMain(FromNativeSocket(listenSocket));
    });

    return true;
}


//------------------------------------------------------------------------------
// StreamingThreadMain
//------------------------------------------------------------------------------
//  Accept native socket connections and parse streamed allocation frames.
//------------------------------------------------------------------------------
void MainWindow::StreamingThreadMain(std::uintptr_t listenSocketValue)
{
    const NativeSocket listenSocket = ToNativeSocket(listenSocketValue);
    while (mStreamingActive.load(std::memory_order_acquire))
    {
        if (!WaitForConnection(listenSocket))
        {
            continue;
        }

        const NativeSocket clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == InvalidSocket)
        {
            continue;
        }

        std::string data = ReceiveSocketData(clientSocket);
        CloseSocket(clientSocket);

        if (!data.empty())
        {
            ParseStreamingData(data);
        }
    }
}


//------------------------------------------------------------------------------
// ParseStreamingData
//------------------------------------------------------------------------------
//  Parse streamed heap allocation data into internal snapshot structures.
//------------------------------------------------------------------------------
void MainWindow::ParseStreamingData(const std::string& data)
{
    std::istringstream input(data);
    std::string line;
    StreamedHeapSnapshot snapshot;

    while (std::getline(input, line))
    {
        std::istringstream lineInput(line);
        std::string token;
        lineInput >> token;

        if (token == "POOL")
        {
            std::string pool;
            std::size_t allocationCount = 0;
            std::size_t allocationBytes = 0;
            unsigned long long trackedAllocations = 0;
            unsigned long long trackedDeallocations = 0;
            unsigned long long untrackedAllocations = 0;
            unsigned long long untrackedDeallocations = 0;
            lineInput >> pool >> allocationCount >> allocationBytes >> trackedAllocations;

            if (lineInput.fail())
            {
                continue;
            }

            if (!(lineInput >> trackedDeallocations))
            {
                trackedDeallocations = 0;
                lineInput.clear();
            }

            if (!(lineInput >> untrackedAllocations))
            {
                untrackedAllocations = 0;
                lineInput.clear();
            }

            if (!(lineInput >> untrackedDeallocations))
            {
                untrackedDeallocations = 0;
                lineInput.clear();
            }

            snapshot.mAllocationCount += allocationCount;
            snapshot.mAllocationBytes += allocationBytes;
            snapshot.mTrackedAllocations += trackedAllocations;
            snapshot.mTrackedDeallocations += trackedDeallocations;
            snapshot.mUntrackedAllocations += untrackedAllocations;
            snapshot.mUntrackedDeallocations += untrackedDeallocations;
        }

        if (token == "ALLOC")
        {
            std::string pool;
            std::string addressText;
            std::size_t size = 0;
            lineInput >> pool >> addressText >> size;

            if (lineInput.fail())
            {
                continue;
            }

            std::uintptr_t address = 0;
            const char* begin = addressText.data();
            const char* end = begin + addressText.size();
            const std::from_chars_result result = std::from_chars(begin, end, address, 16);
            if (result.ec != std::errc())
            {
                continue;
            }

            snapshot.mAllocations.push_back({pool, address, size});
        }
    }

    StreamedAllocationSummary summary = BuildAllocationSummary(snapshot);
    summary.mStreamPayloadSize = data.size();

    std::lock_guard lock(mStreamedSnapshotsMutex);
    mStreamedSnapshots.push_back(std::move(snapshot));

    QMetaObject::invokeMethod(
        this,
        [this, summary]
        {
            UpdateStreamingSummary(summary);
        },
        Qt::QueuedConnection);
}


//------------------------------------------------------------------------------
// UpdateAddressRange
//------------------------------------------------------------------------------
//  Fold a streamed allocation into an address range summary.
//------------------------------------------------------------------------------
void MainWindow::UpdateAddressRange(StreamedAddressRange& range, const StreamedAllocation& allocation) const
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
void MainWindow::AddAllocationToBin(std::vector<StreamedAllocationBin>& bins,
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

        bins.push_back({nextMinimumSize, nextMaximumSize, 0, 0, 0, 0});
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

    ++binIterator->mAllocationCount;
    binIterator->mAllocationBytes += allocation.mSize;
}


//------------------------------------------------------------------------------
// BuildAllocationSummary
//------------------------------------------------------------------------------
//  Aggregate streamed allocations and pool counters into the combined summary.
//------------------------------------------------------------------------------
MainWindow::StreamedAllocationSummary MainWindow::BuildAllocationSummary(const StreamedHeapSnapshot& snapshot) const
{
    StreamedAllocationSummary summary;
    summary.mAllocationCount = snapshot.mAllocationCount;
    summary.mAllocationBytes = snapshot.mAllocationBytes;
    summary.mTrackedAllocations = snapshot.mTrackedAllocations;
    summary.mTrackedDeallocations = snapshot.mTrackedDeallocations;
    summary.mUntrackedAllocations = snapshot.mUntrackedAllocations;
    summary.mUntrackedDeallocations = snapshot.mUntrackedDeallocations;

    for (const StreamedAllocation& allocation : snapshot.mAllocations)
    {
        if (snapshot.mAllocationCount == 0)
        {
            ++summary.mAllocationCount;
            summary.mAllocationBytes += allocation.mSize;
        }

        UpdateAddressRange(summary.mAddressRange, allocation);
        AddAllocationToBin(summary.mBins, allocation);
    }

    return summary;
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
std::string MainWindow::BuildAddressText(std::uintptr_t address, bool hasAddress) const
{
    if (!hasAddress)
    {
        return "none";
    }

    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << address;
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
    constexpr int rowCount = 10;
    mOverviewTable->setRowCount(rowCount);

    const std::uintptr_t rangeStart = summary.mAddressRange.mLowestAddress;
    const std::uintptr_t rangeEnd = CalculateAddressRangeEnd(summary.mAddressRange);
    const std::size_t addressRange = summary.mAddressRange.mHasAllocations
                                         ? static_cast<std::size_t>(rangeEnd - rangeStart)
                                         : 0;

    SetTableRow(
        mOverviewTable,
        0,
        tr("Allocation count"),
        QString::fromStdString(BuildCommaSeparatedIntegerText(summary.mAllocationCount)));
    SetTableRow(mOverviewTable, 1, tr("Allocation bytes"), QString::fromStdString(BuildByteCountText(summary.mAllocationBytes)));
    SetTableRow(mOverviewTable, 2, tr("Start address"), QString::fromStdString(BuildAddressText(rangeStart, summary.mAddressRange.mHasAllocations)), true);
    SetTableRow(mOverviewTable, 3, tr("End address"), QString::fromStdString(BuildAddressText(rangeEnd, summary.mAddressRange.mHasAllocations)), true);
    SetTableRow(mOverviewTable, 4, tr("Address Range"), QString::fromStdString(BuildHexValueText(addressRange, summary.mAddressRange.mHasAllocations)), true);
    SetTableRow(mOverviewTable, 5, tr("Tracked allocations"), QString::fromStdString(BuildCommaSeparatedIntegerText(summary.mTrackedAllocations)));
    SetTableRow(mOverviewTable, 6, tr("Tracked deallocations"), QString::fromStdString(BuildCommaSeparatedIntegerText(summary.mTrackedDeallocations)));
    SetTableRow(mOverviewTable, 7, tr("Untracked allocations"), QString::fromStdString(BuildCommaSeparatedIntegerText(summary.mUntrackedAllocations)));
    SetTableRow(mOverviewTable, 8, tr("Untracked deallocations"), QString::fromStdString(BuildCommaSeparatedIntegerText(summary.mUntrackedDeallocations)));
    SetTableRow(mOverviewTable, 9, tr("Last stream payload size"), QString::fromStdString(BuildByteCountText(summary.mStreamPayloadSize)));

    mOverviewTable->resizeRowsToContents();
}


//------------------------------------------------------------------------------
// UpdateBinsView
//------------------------------------------------------------------------------
//  Update the bin table and graph with combined allocation records.
//------------------------------------------------------------------------------
void MainWindow::UpdateBinsView(const StreamedAllocationSummary& summary)
{
    mBinsGraphWidget->SetBins(summary.mBins);
    mBinsTable->setRowCount(static_cast<int>(summary.mBins.size()));

    for (int row = 0; row < static_cast<int>(summary.mBins.size()); ++row)
    {
        const StreamedAllocationBin& bin = summary.mBins[static_cast<std::size_t>(row)];
        const std::size_t averageBytes = bin.mAllocationCount == 0
                                             ? 0
                                             : bin.mAllocationBytes / bin.mAllocationCount;

        std::ostringstream rangeText;
        rangeText << BuildBinLimitLabel(bin.mMinimumSize) << "-"
                  << BuildBinLimitLabel(bin.mMaximumSize);

        QTableWidgetItem* rangeItem = new QTableWidgetItem(QString::fromStdString(rangeText.str()));
        QTableWidgetItem* countItem = new QTableWidgetItem(
            QString::fromStdString(BuildCommaSeparatedIntegerText(bin.mAllocationCount)));
        QTableWidgetItem* bytesItem = new QTableWidgetItem(QString::fromStdString(BuildByteCountText(bin.mAllocationBytes)));
        QTableWidgetItem* minimumItem = new QTableWidgetItem(
            QString::fromStdString(BuildByteCountText(bin.mMinimumAllocationSize)));
        QTableWidgetItem* maximumItem = new QTableWidgetItem(
            QString::fromStdString(BuildByteCountText(bin.mMaximumAllocationSize)));
        QTableWidgetItem* averageItem = new QTableWidgetItem(QString::fromStdString(BuildByteCountText(averageBytes)));

        rangeItem->setFlags(rangeItem->flags() & ~Qt::ItemIsEditable);
        countItem->setFlags(countItem->flags() & ~Qt::ItemIsEditable);
        bytesItem->setFlags(bytesItem->flags() & ~Qt::ItemIsEditable);
        minimumItem->setFlags(minimumItem->flags() & ~Qt::ItemIsEditable);
        maximumItem->setFlags(maximumItem->flags() & ~Qt::ItemIsEditable);
        averageItem->setFlags(averageItem->flags() & ~Qt::ItemIsEditable);

        mBinsTable->setItem(row, 0, rangeItem);
        mBinsTable->setItem(row, 1, countItem);
        mBinsTable->setItem(row, 2, bytesItem);
        mBinsTable->setItem(row, 3, minimumItem);
        mBinsTable->setItem(row, 4, maximumItem);
        mBinsTable->setItem(row, 5, averageItem);
    }

    mBinsTable->resizeRowsToContents();
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
