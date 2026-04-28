#include "mainwindow.hpp"

#include "build_info.hpp"

#include <QAction>
#include <QApplication>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QString>

#include <string>


//------------------------------------------------------------------------------
// MainWindow
//------------------------------------------------------------------------------
//  Construct the main application window and initialize its menus.
//------------------------------------------------------------------------------
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("HeapVisualizer"));
    resize(900, 600);
    CreateMenus();
}


//------------------------------------------------------------------------------
// CreateMenus
//------------------------------------------------------------------------------
//  Create the main window menu actions and connect them to their handlers.
//------------------------------------------------------------------------------
void MainWindow::CreateMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    exitAction = fileMenu->addAction(tr("E&xit"));
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, []
    {
        QApplication::quit();
    });

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    aboutAction = helpMenu->addAction(tr("&About"));
    connect(aboutAction, &QAction::triggered, this, [this]
    {
        ShowAboutDialog();
    });
}


//------------------------------------------------------------------------------
// ShowAboutDialog
//------------------------------------------------------------------------------
//  Show the application version and build date in the About dialog.
//------------------------------------------------------------------------------
void MainWindow::ShowAboutDialog()
{
    const std::string message = std::string("Version: ") + HeapVisualizer::BuildVersion +
                                "\nBuild date: " + HeapVisualizer::BuildDate;

    QMessageBox::about(this, tr("About HeapVisualizer"), QString::fromStdString(message));
}
