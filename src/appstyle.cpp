#include "appstyle.hpp"

#include <QApplication>
#include <QStyle>
#include <QStyleFactory>


namespace HeapVisualizer
{

//------------------------------------------------------------------------------
// ApplyApplicationStyle
//------------------------------------------------------------------------------
//  Apply shared Qt widget styling for HeapVisualizer applications.
//------------------------------------------------------------------------------
void ApplyApplicationStyle(QApplication& application)
{
    QStyle* fusionStyle = QStyleFactory::create(QStringLiteral("Fusion"));
    if (fusionStyle != nullptr)
    {
        application.setStyle(fusionStyle);
    }

    application.setStyleSheet(QStringLiteral(R"(
        QMainWindow
        {
            background: #edf1f5;
        }

        QWidget#MainSurface,
        QWidget#AllocationTestSurface
        {
            background: #edf1f5;
        }

        QLabel#WindowTitle
        {
            color: #162033;
            font-size: 22px;
            font-weight: 700;
        }

        QLabel#WindowSubtitle
        {
            color: #536173;
            font-size: 12px;
        }

        QLabel#SummaryPanel
        {
            background: #ffffff;
            border: 1px solid #cfd8e3;
            border-left: 5px solid #2f80ed;
            border-radius: 8px;
            color: #172033;
            font-size: 17px;
            font-weight: 600;
            padding: 18px 20px;
        }

        QTabWidget::pane
        {
            background: #ffffff;
            border: 1px solid #cfd8e3;
            border-radius: 8px;
            top: -1px;
        }

        QTabBar::tab
        {
            background: #e4eaf1;
            border: 1px solid #c6d0dc;
            border-bottom: none;
            border-top-left-radius: 6px;
            border-top-right-radius: 6px;
            color: #26364f;
            font-weight: 600;
            margin-right: 4px;
            min-width: 96px;
            padding: 8px 14px;
        }

        QTabBar::tab:selected
        {
            background: #ffffff;
            color: #0f4fa8;
        }

        QTableWidget
        {
            background: #ffffff;
            border: 1px solid #cfd8e3;
            border-radius: 8px;
            color: #172033;
            gridline-color: #e3e9f0;
        }

        QTableWidget::item:selected
        {
            background: transparent;
            color: #172033;
        }

        QTableWidget::item:focus
        {
            outline: none;
        }

        QTableWidget#OverviewTable::item:selected,
        QTableWidget#OverviewTable::item:selected:active,
        QTableWidget#OverviewTable::item:selected:!active,
        QTableWidget#BinsTable::item:selected,
        QTableWidget#BinsTable::item:selected:active,
        QTableWidget#BinsTable::item:selected:!active
        {
            background: #dbeaff;
            color: #172033;
        }

        QHeaderView::section
        {
            background: #edf3fa;
            border: none;
            border-right: 1px solid #d5dee9;
            color: #26364f;
            font-weight: 700;
            padding: 8px;
        }

        QSplitter#BinsSplitter::handle
        {
            background: #d5dee9;
            border-radius: 3px;
            margin: 5px 120px;
        }

        QSplitter#BinsSplitter::handle:hover
        {
            background: #8fb8ea;
        }

        QMenuBar
        {
            background: #172033;
            color: #f7fafc;
            padding: 3px;
        }

        QMenuBar::item
        {
            background: transparent;
            border-radius: 4px;
            padding: 6px 10px;
        }

        QMenuBar::item:selected
        {
            background: #2f80ed;
        }

        QMenu
        {
            background: #ffffff;
            border: 1px solid #c6d0dc;
            color: #172033;
            padding: 6px;
        }

        QMenu::item
        {
            border-radius: 4px;
            padding: 7px 28px 7px 20px;
        }

        QMenu::item:selected
        {
            background: #e7f0ff;
            color: #0f4fa8;
        }

        QDialog,
        QMessageBox
        {
            background: #f8fafc;
            color: #172033;
        }

        QLabel#DialogTitle
        {
            color: #162033;
            font-size: 18px;
            font-weight: 700;
        }

        QLabel#DialogSubtitle
        {
            color: #5c6878;
        }

        QFrame#DialogPanel,
        QGroupBox
        {
            background: #ffffff;
            border: 1px solid #cfd8e3;
            border-radius: 8px;
            margin-top: 12px;
            padding: 12px;
        }

        QGroupBox::title
        {
            color: #26364f;
            font-weight: 700;
            left: 12px;
            padding: 0 6px;
            subcontrol-origin: margin;
            subcontrol-position: top left;
        }

        QLineEdit,
        QSpinBox,
        QComboBox
        {
            background: #ffffff;
            border: 1px solid #b9c5d3;
            border-radius: 5px;
            color: #172033;
            padding: 6px 8px;
            selection-background-color: #2f80ed;
        }

        QLineEdit:focus,
        QSpinBox:focus,
        QComboBox:focus
        {
            border: 1px solid #2f80ed;
        }

        QPushButton
        {
            background: #ffffff;
            border: 1px solid #adbac9;
            border-radius: 5px;
            color: #172033;
            font-weight: 600;
            min-height: 30px;
            padding: 6px 14px;
        }

        QPushButton:hover
        {
            background: #eef5ff;
            border-color: #2f80ed;
        }

        QPushButton:pressed
        {
            background: #dbeaff;
        }

        QPushButton:default
        {
            background: #2f80ed;
            border-color: #1d63bd;
            color: #ffffff;
        }

        QListWidget
        {
            background: #ffffff;
            border: 1px solid #cfd8e3;
            border-radius: 8px;
            color: #172033;
            padding: 6px;
        }

        QListWidget::item
        {
            border-radius: 4px;
            padding: 7px;
        }

        QListWidget::item:selected
        {
            background: #dbeaff;
            color: #123d73;
        }

        QCheckBox
        {
            color: #253149;
            spacing: 8px;
        }
    )"));
}

}
