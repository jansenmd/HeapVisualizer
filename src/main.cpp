#include "build_info.hpp"
#include "mainwindow.hpp"

#include <QApplication>
#include <QString>


//------------------------------------------------------------------------------
// main
//------------------------------------------------------------------------------
//  Start the Qt application and show the main window.
//------------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("HeapVisualizer"));
    QApplication::setApplicationVersion(QString::fromUtf8(HeapVisualizer::BuildVersion));

    MainWindow window;
    window.show();

    return application.exec();
}
