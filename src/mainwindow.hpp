#pragma once

#include <QMainWindow>

class QAction;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void CreateMenus();
    void ShowAboutDialog();

    QAction* mExitAction = nullptr;
    QAction* mAboutAction = nullptr;
};
