// 主题系统：品牌「熔炉 · 铁灰炉火」默认 + codex/zcode/claude 三致敬皮肤（AgentHive 同款思路）。
// Fusion style + QPalette + token 色板；所有取色经 palette() 实时解析，
// 每控件样式串在构建时取色（切换主题写入 QSettings，重启应用完全生效）。
#pragma once
#include <QColor>
#include <QFont>
#include <QVector>

class QApplication;

namespace miderforge::theme {

struct Palette {
    const char* id;
    const char* zh;                    // 主题菜单显示名
    QColor window, panel, codeBg;      // 层级底色
    QColor text, textDim;              // 文本
    QColor accent, brand;              // 强调 / 品牌签名（渐变签名线、品牌标题）
    QColor success, warn, error;       // 语义色
    QColor line, btn, btnHover, altRow; // 描边/按钮/悬停/斑马行装饰色
};

// 内置色板：0=forge 品牌默认，其余为致敬皮肤
const QVector<Palette>& palettes();
int paletteIndex();                // QSettings 持久化，默认 0（forge）
void setPaletteIndex(int idx);     // 写 QSettings（重启应用完全生效）
inline const Palette& palette() { return palettes()[paletteIndex()]; }

namespace colors {
// 兼容历史调用点：函数形式实时取当前色板
inline QColor window()  { return palette().window; }
inline QColor panel()   { return palette().panel; }
inline QColor codeBg()  { return palette().codeBg; }
inline QColor text()    { return palette().text; }
inline QColor textDim() { return palette().textDim; }
inline QColor accent()  { return palette().accent; }
inline QColor brand()   { return palette().brand; }
inline QColor success() { return palette().success; }
inline QColor warn()    { return palette().warn; }
inline QColor error()   { return palette().error; }
inline QColor line()    { return palette().line; }
inline QColor btn()     { return palette().btn; }
inline QColor btnHover(){ return palette().btnHover; }
inline QColor altRow()  { return palette().altRow; }
} // namespace colors

// 设计刻度：把散落各处的魔法数字收敛成一套刻度（间距 4 的倍数、控件高度三档）。
// 用途：布局不再出现 28/30/32/36 混搭这类"看着就差一口气"的不一致。
namespace metrics {
inline constexpr int spaceXs = 4;
inline constexpr int spaceSm = 8;
inline constexpr int spaceMd = 12;
inline constexpr int spaceLg = 16;
inline constexpr int spaceXl = 20;
inline constexpr int rowCompact = 28; // 行内小控件（筛选框、标签行）
inline constexpr int rowDefault = 32; // 常规按钮 / 输入框
inline constexpr int rowPrimary = 38; // 主行动按钮（新建会话）
inline constexpr int radius = 6;
inline constexpr int sidebarWidth = 260;
inline constexpr int settingsNavWidth = 228;
} // namespace metrics

void apply(QApplication& app);
// 全局 QSS：输入框/按钮/列表/表格/Tab/滚动条/菜单统一观感（颜色全部来自当前色板）
QString globalStyleSheet();
QFont uiFont();   // 微软雅黑 UI 9pt
QFont monoFont(); // Consolas（代码/JSON/日志）

// 状态色圆点的富文本片段（●绿=正常/●红=故障/●灰=未配置）
QString coloredDot(const QColor& c);
// 品牌标语（左栏品牌行/空态主视觉/状态栏共用）
QString brandTagline();
// 品牌签名渐变（品牌色 → 强调色）的 QSS 片段：左栏品牌线、空态主视觉、聚焦态共用。
// 纯字符串构建，可脱离 QApplication 单测。x1/y1→x2/y2 取 0..1 表示方向。
QString brandGradient(qreal x1 = 0.0, qreal y1 = 0.0, qreal x2 = 1.0, qreal y2 = 0.0);

} // namespace miderforge::theme
