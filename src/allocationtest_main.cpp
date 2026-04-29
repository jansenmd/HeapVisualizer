#include "allocationtestwindow.hpp"
#include "appstyle.hpp"
#include "build_info.hpp"

#include <QApplication>
#include <QString>
#include <QTimer>

#include <cstdlib>


//------------------------------------------------------------------------------
// main
//------------------------------------------------------------------------------
//  Start the heap allocation test application and show its main window.
//------------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("HeapAllocationTest"));
    QApplication::setApplicationVersion(QString::fromUtf8(HeapVisualizer::BuildVersion));
    HeapVisualizer::ApplyApplicationStyle(application);

    AllocationTestWindow window;
    window.show();

    if (const char* autoQuitMilliseconds = std::getenv("HEAP_ALLOCATION_TEST_AUTO_QUIT_MS");
       autoQuitMilliseconds != nullptr)
    {
        const int delayMilliseconds = std::atoi(autoQuitMilliseconds);
        if (delayMilliseconds >= 0)
        {
            QTimer::singleShot(delayMilliseconds, &application, &QApplication::quit);
        }
    }

    return application.exec();
}
