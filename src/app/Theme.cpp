// 深色主题实现
#include "app/Theme.h"
#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace miderforge::theme {

void apply(QApplication& app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette pal;
    pal.setColor(QPalette::Window, colors::window);
    pal.setColor(QPalette::WindowText, colors::text);
    pal.setColor(QPalette::Base, colors::panel);
    pal.setColor(QPalette::AlternateBase, colors::window);
    pal.setColor(QPalette::ToolTipBase, colors::panel);
    pal.setColor(QPalette::ToolTipText, colors::text);
    pal.setColor(QPalette::Text, colors::text);
    pal.setColor(QPalette::Button, colors::panel);
    pal.setColor(QPalette::ButtonText, colors::text);
    pal.setColor(QPalette::BrightText, Qt::white);
    pal.setColor(QPalette::Link, colors::accent);
    pal.setColor(QPalette::Highlight, colors::accent);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    // 占位符显式给色：Fusion 在深色下默认派生过暗，接近黑底黑字
    pal.setColor(QPalette::PlaceholderText, colors::textDim);
    pal.setColor(QPalette::Disabled, QPalette::Text, colors::textDim);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, colors::textDim);
    pal.setColor(QPalette::Disabled, QPalette::Window, colors::window);
    app.setPalette(pal);

    app.setFont(uiFont());
    app.setStyleSheet(globalStyleSheet());
}

QString globalStyleSheet() {
    // 统一深色控件观感：圆角、聚焦描边、悬停/按下反馈、纤细滚动条
    return QStringLiteral(R"(
* { outline: none; }

QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox, QDateTimeEdit {
    background-color: %panel;
    color: %text;
    border: 1px solid #3d4045;
    border-radius: 6px;
    padding: 4px 8px;
    selection-background-color: %accent;
    selection-color: white;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus,
QSpinBox:focus, QDoubleSpinBox:focus, QDateTimeEdit:focus {
    border: 1px solid %accent;
}
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled {
    color: #5a5f64;
    background-color: %window;
}
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView {
    background-color: %panel;
    color: %text;
    border: 1px solid #3d4045;
    border-radius: 6px;
    selection-background-color: %accent;
    selection-color: white;
    outline: none;
}

QPushButton {
    background-color: #33363b;
    color: %text;
    border: 1px solid #3d4045;
    border-radius: 6px;
    padding: 5px 16px;
}
QPushButton:hover { background-color: #3c4046; border-color: %accent; }
QPushButton:pressed { background-color: %panel; }
QPushButton:disabled { color: #5a5f64; background-color: %panel; border-color: #35383d; }
QPushButton#primaryBtn {
    background-color: %accent;
    color: white;
    border: none;
    font-weight: bold;
}
QPushButton#primaryBtn:hover { background-color: #5d99ff; }
QPushButton#primaryBtn:pressed { background-color: #3d78e0; }
QPushButton#primaryBtn:disabled { background-color: #35455e; color: #7a8899; }

QListWidget {
    background-color: %code;
    border: 1px solid #35383d;
    border-radius: 8px;
    padding: 4px;
    outline: none;
}
QListWidget::item { padding: 8px 10px; border-radius: 6px; margin: 1px 2px; }
QListWidget::item:hover { background-color: #2e3135; }
QListWidget::item:selected { background-color: %accent; color: white; }

QTableWidget, QTableView {
    alternate-background-color: #27292d;
    background-color: %code;
    gridline-color: #313438;
    border: 1px solid #35383d;
    border-radius: 8px;
    selection-background-color: %accent;
    selection-color: white;
    outline: none;
}
QHeaderView::section {
    background-color: %panel;
    color: %dim;
    border: none;
    border-bottom: 1px solid #3d4045;
    padding: 8px 6px;
    font-weight: bold;
}
QTableCornerButton::section { background-color: %panel; border: none; }

QTabWidget::pane { border: 1px solid #35383d; border-radius: 6px; }
QTabBar::tab {
    background: transparent;
    color: %dim;
    padding: 7px 16px;
    border: none;
    border-bottom: 2px solid transparent;
    margin-right: 2px;
}
QTabBar::tab:selected { color: %text; border-bottom: 2px solid %accent; }
QTabBar::tab:hover:!selected { color: %text; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #3d4045; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: %accent; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: #3d4045; border-radius: 5px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: %accent; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QMenu { background-color: %panel; border: 1px solid #3d4045; border-radius: 8px; padding: 4px; }
QMenu::item { padding: 6px 28px 6px 16px; border-radius: 4px; }
QMenu::item:selected { background-color: %accent; color: white; }
QMenu::separator { height: 1px; background-color: #3d4045; margin: 4px 8px; }

QToolTip {
    background-color: %panel;
    color: %text;
    border: 1px solid #3d4045;
    border-radius: 4px;
    padding: 4px 8px;
}

QProgressBar { background-color: %panel; border: none; border-radius: 3px; }
QProgressBar::chunk { background-color: %accent; border-radius: 3px; }

QDialog { background-color: %window; }
)")
                         .replace("%panel", colors::panel.name())
                         .replace("%window", colors::window.name())
                         .replace("%text", colors::text.name())
                         .replace("%dim", colors::textDim.name())
                         .replace("%accent", colors::accent.name())
                         .replace("%code", colors::codeBg.name());
}

QFont uiFont() {
    return QFont(QStringLiteral("Microsoft YaHei UI"), 9);
}

QFont monoFont() {
    return QFont(QStringLiteral("Consolas"), 9);
}

QString coloredDot(const QColor& c) {
    return QStringLiteral("<span style=\"color:%1\">●</span>").arg(c.name());
}

} // namespace miderforge::theme
