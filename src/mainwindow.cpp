#include "mainwindow.hpp"

#include "build_info.hpp"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QSpinBox>
#include <QString>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
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

    mStreamingSummaryLabel = new QLabel(centralWidget);
    mStreamingSummaryLabel->setObjectName(QStringLiteral("SummaryPanel"));
    mStreamingSummaryLabel->setMinimumHeight(300);

    layout->addWidget(titleLabel);
    layout->addWidget(subtitleLabel);
    layout->addWidget(mStreamingSummaryLabel);
    layout->addStretch(1);
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
    std::string token;
    StreamedHeapSnapshot snapshot;

    while (input >> token)
    {
        if (token == "ALLOC")
        {
            std::string pool;
            std::string addressText;
            std::size_t size = 0;
            input >> pool >> addressText >> size;

            if (input.fail())
            {
                return;
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
// BuildAllocationSummary
//------------------------------------------------------------------------------
//  Aggregate streamed allocations into application, internal, and total buckets.
//------------------------------------------------------------------------------
MainWindow::StreamedAllocationSummary MainWindow::BuildAllocationSummary(const StreamedHeapSnapshot& snapshot) const
{
    StreamedAllocationSummary summary;

    for (const StreamedAllocation& allocation : snapshot.mAllocations)
    {
        ++summary.mTotalAllocationCount;
        summary.mTotalSize += allocation.mSize;
        UpdateAddressRange(summary.mTotalAddressRange, allocation);

        if (allocation.mPool == "internal")
        {
            ++summary.mInternalAllocationCount;
            summary.mInternalTotalSize += allocation.mSize;
            UpdateAddressRange(summary.mInternalAddressRange, allocation);
        }

        if (allocation.mPool == "main")
        {
            ++summary.mApplicationAllocationCount;
            summary.mApplicationTotalSize += allocation.mSize;
            UpdateAddressRange(summary.mApplicationAddressRange, allocation);
        }
    }

    return summary;
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

    const std::uintptr_t maximumAddress = std::numeric_limits<std::uintptr_t>::max();
    std::uintptr_t rangeEndAddress = maximumAddress;
    if (range.mHighestAddressAllocationSize <= maximumAddress - range.mHighestAddress)
    {
        rangeEndAddress = range.mHighestAddress + range.mHighestAddressAllocationSize;
    }

    std::ostringstream output;
    output << "0x" << std::hex << range.mLowestAddress
           << " - 0x" << rangeEndAddress
           << std::dec << " (" << (rangeEndAddress - range.mLowestAddress) << " bytes)";

    return output.str();
}


//------------------------------------------------------------------------------
// BuildStreamingSummaryText
//------------------------------------------------------------------------------
//  Build display text for the current streamed allocation summary.
//------------------------------------------------------------------------------
std::string MainWindow::BuildStreamingSummaryText(const StreamedAllocationSummary& summary) const
{
    std::ostringstream output;
    output << "Last stream payload size: " << summary.mStreamPayloadSize << " bytes"
           << "\n\nApplication allocations: " << summary.mApplicationAllocationCount
           << "\nApplication total size: " << summary.mApplicationTotalSize << " bytes"
           << "\nApplication address range: " << BuildAddressRangeText(summary.mApplicationAddressRange)
           << "\n\nInternal pool allocations: " << summary.mInternalAllocationCount
           << "\nInternal pool total size: " << summary.mInternalTotalSize << " bytes"
           << "\nInternal pool address range: " << BuildAddressRangeText(summary.mInternalAddressRange)
           << "\n\nAll allocations: " << summary.mTotalAllocationCount
           << "\nAll allocation total size: " << summary.mTotalSize << " bytes"
           << "\nAll allocation address range: " << BuildAddressRangeText(summary.mTotalAddressRange);

    return output.str();
}


//------------------------------------------------------------------------------
// UpdateStreamingSummary
//------------------------------------------------------------------------------
//  Display allocation count and size information from the latest snapshot.
//------------------------------------------------------------------------------
void MainWindow::UpdateStreamingSummary(const StreamedAllocationSummary& summary)
{
    const std::string summaryText = BuildStreamingSummaryText(summary);
    mStreamingSummaryLabel->setText(QString::fromStdString(summaryText));
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
