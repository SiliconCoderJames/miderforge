// 主题系统实现：四色板（品牌 forge + codex/zcode/claude 致敬皮肤）
#include "app/Theme.h"
#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>

namespace miderforge::theme {

const QVector<Palette>& palettes() {
    static const QVector<Palette> k = {
        // 品牌主题「熔炉」：铁灰炉膛 + 炉火橙（锻造意象：铁灰为体、炉火为魂）
        {"forge", "熔炉 · 铁灰炉火（默认）",
            {"#17181c"}, {"#22242a"}, {"#1d1f24"},
            {"#e8e6e3"}, {"#9aa0a6"},
            {"#f97316"}, {"#fb923c"},
            {"#57ab5a"}, {"#e0a458"}, {"#e06c60"},
            {"#35383d"}, {"#2b2e34"}, {"#35393f"}, {"#1c1e23"}},
        // 致敬皮肤：OpenAI Codex 石墨翡翠
        {"codex", "石墨 · Codex 风",
            {"#0d0d0d"}, {"#1b1b1b"}, {"#151515"},
            {"#ececec"}, {"#969696"},
            {"#10a37f"}, {"#10a37f"},
            {"#43a047"}, {"#e8a33d"}, {"#e2574c"},
            {"#2f2f2f"}, {"#262626"}, {"#2f2f2f"}, {"#131313"}},
        // 致敬皮肤：ZCode 经典 VS Dark+
        {"zcode", "经典 · ZCode 风",
            {"#1e1e1e"}, {"#252526"}, {"#202020"},
            {"#d4d4d4"}, {"#8a8a8a"},
            {"#0098ff"}, {"#cca700"},
            {"#4ec9b0"}, {"#cca700"}, {"#f14c4c"},
            {"#3c3c3c"}, {"#2d2d2d"}, {"#373737"}, {"#232323"}},
        // 致敬皮肤：Claude 暖沙陶橙
        {"claude", "暖沙 · Claude 风",
            {"#262624"}, {"#30302e"}, {"#2b2b28"},
            {"#f0ece1"}, {"#a8a29a"},
            {"#d97757"}, {"#d97757"},
            {"#8fbc72"}, {"#e0a458"}, {"#e05d5d"},
            {"#4a4944"}, {"#3a3a35"}, {"#45453e"}, {"#2a2a27"}},
    };
    return k;
}

namespace {
// 色板索引缓存：初值从 QSettings 惰性读取（避免每次取色都查注册表）。
// 必须可失效——setPaletteIndex 改的是 QSettings，若不刷新缓存，
// 进程内后续 paletteIndex() 会一直返回旧值，界面看起来"改了没生效"。
int& cachedPaletteIndex() {
    static int idx = -1;
    if (idx < 0) {
        QSettings s(QStringLiteral("Miderforge"), QStringLiteral("Miderforge"));
        const QString id = s.value(QStringLiteral("ui/palette")).toString();
        const auto& ps = palettes();
        idx = 0; // 默认品牌主题
        for (int k = 0; k < ps.size(); ++k) {
            if (id == ps[k].id) {
                idx = k;
                break;
            }
        }
    }
    return idx;
}
} // namespace

int paletteIndex() {
    return cachedPaletteIndex();
}

void setPaletteIndex(int idx) {
    const auto& ps = palettes();
    if (idx < 0 || idx >= ps.size())
        return;
    QSettings s(QStringLiteral("Miderforge"), QStringLiteral("Miderforge"));
    s.setValue(QStringLiteral("ui/palette"), QString::fromLatin1(ps[idx].id));
    s.sync();
    cachedPaletteIndex() = idx; // 立即生效（含测试与命令面板切换主题）
}

void apply(QApplication& app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    const Palette& p = palette();

    QPalette pal;
    pal.setColor(QPalette::Window, p.window);
    pal.setColor(QPalette::WindowText, p.text);
    pal.setColor(QPalette::Base, p.panel);
    pal.setColor(QPalette::AlternateBase, p.altRow);
    pal.setColor(QPalette::ToolTipBase, p.panel);
    pal.setColor(QPalette::ToolTipText, p.text);
    pal.setColor(QPalette::Text, p.text);
    pal.setColor(QPalette::Button, p.btn);
    pal.setColor(QPalette::ButtonText, p.text);
    pal.setColor(QPalette::BrightText, Qt::white);
    pal.setColor(QPalette::Link, p.accent);
    pal.setColor(QPalette::Highlight, p.accent);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    // 占位符显式给色：Fusion 在深色下默认派生过暗，接近黑底黑字
    pal.setColor(QPalette::PlaceholderText, p.textDim);
    pal.setColor(QPalette::Disabled, QPalette::Text, p.textDim);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, p.textDim);
    pal.setColor(QPalette::Disabled, QPalette::Window, p.window);
    app.setPalette(pal);

    app.setFont(uiFont());
    app.setStyleSheet(globalStyleSheet());
}

QString globalStyleSheet() {
    const Palette& p = palette();
    const QString accentHi = QColor(p.accent).lighter(112).name();
    const QString accentDeep = QColor(p.accent).darker(118).name();
    const QString disabled = QColor(p.textDim).darker(130).name();
    // 统一控件观感：圆角、聚焦描边、悬停/按下反馈、纤细滚动条（颜色全部 token 化）
    return QStringLiteral(R"(
* { outline: none; }

QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox, QDateTimeEdit {
    background-color: %panel;
    color: %text;
    border: 1px solid %line;
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
    color: %disabled;
    background-color: %window;
}
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView {
    background-color: %panel;
    color: %text;
    border: 1px solid %line;
    border-radius: 6px;
    selection-background-color: %accent;
    selection-color: white;
    outline: none;
}

QPushButton {
    background-color: %btn;
    color: %text;
    border: 1px solid %line;
    border-radius: 6px;
    padding: 5px 16px;
}
QPushButton:hover { background-color: %btnHover; border-color: %accent; }
QPushButton:pressed { background-color: %panel; }
QPushButton:disabled { color: %disabled; background-color: %panel; border-color: %line; }
QPushButton#primaryBtn {
    background-color: %accent;
    color: white;
    border: none;
    font-weight: bold;
}
QPushButton#primaryBtn:hover { background-color: %accentHi; }
QPushButton#primaryBtn:pressed { background-color: %accentDeep; }
QPushButton#primaryBtn:disabled { background-color: %btn; color: %disabled; }

QListWidget {
    background-color: %code;
    border: 1px solid %line;
    border-radius: 8px;
    padding: 4px;
    outline: none;
}
QListWidget::item { padding: 8px 10px; border-radius: 6px; margin: 1px 2px; }
QListWidget::item:hover { background-color: %btnHover; }
QListWidget::item:selected { background-color: %accent; color: white; }

QTableWidget, QTableView {
    alternate-background-color: %altRow;
    background-color: %code;
    gridline-color: %line;
    border: 1px solid %line;
    border-radius: 8px;
    selection-background-color: %accent;
    selection-color: white;
    outline: none;
}
QHeaderView::section {
    background-color: %panel;
    color: %dim;
    border: none;
    border-bottom: 1px solid %line;
    padding: 8px 6px;
    font-weight: bold;
}
QTableCornerButton::section { background-color: %panel; border: none; }

QTabWidget::pane { border: 1px solid %line; border-radius: 6px; }
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
QScrollBar::handle:vertical { background: %btnHover; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: %accent; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: %btnHover; border-radius: 5px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: %accent; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QMenu { background-color: %panel; border: 1px solid %line; border-radius: 8px; padding: 4px; }
QMenu::item { padding: 6px 28px 6px 16px; border-radius: 4px; }
QMenu::item:selected { background-color: %accent; color: white; }
QMenu::separator { height: 1px; background-color: %line; margin: 4px 8px; }

QToolTip {
    background-color: %panel;
    color: %text;
    border: 1px solid %line;
    border-radius: 4px;
    padding: 4px 8px;
}

QProgressBar { background-color: %panel; border: none; border-radius: 3px; }
QProgressBar::chunk { background-color: %accent; border-radius: 3px; }

QDialog { background-color: %window; }
)")
                         .replace("%panel", p.panel.name())
                         .replace("%window", p.window.name())
                         .replace("%text", p.text.name())
                         .replace("%dim", p.textDim.name())
                         .replace("%accentDeep", accentDeep)
                         .replace("%accentHi", accentHi)
                         .replace("%accent", p.accent.name())
                         .replace("%code", p.codeBg.name())
                         .replace("%altRow", p.altRow.name())
                         .replace("%btnHover", p.btnHover.name())
                         .replace("%btn", p.btn.name())
                         .replace("%line", p.line.name())
                         .replace("%disabled", disabled);
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

QString brandTagline() {
    return QStringLiteral("锻造云脑 · 常驻本机");
}

QString brandGradient(qreal x1, qreal y1, qreal x2, qreal y2) {
    // 品牌色 → 强调色。换肤后自动跟随当前色板（不写死 forge 橙）
    return QStringLiteral("qlineargradient(x1:%1,y1:%2,x2:%3,y2:%4,stop:0 %5,stop:1 %6)")
        .arg(x1).arg(y1).arg(x2).arg(y2)
        .arg(palette().brand.name(), palette().accent.name());
}

} // namespace miderforge::theme
