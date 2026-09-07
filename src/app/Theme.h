// 深色主题（规格 4.8）：Fusion style + 自定义 QPalette + 全局色值常量与字体
#pragma once
#include <QColor>
#include <QFont>

class QApplication;

namespace miderforge::theme {

// 规格定义的色板
namespace colors {
inline const QColor window  {0x1e, 0x1f, 0x22}; // 窗口底
inline const QColor panel   {0x2b, 0x2d, 0x30}; // 面板
inline const QColor text    {0xdc, 0xdf, 0xe4}; // 正文
inline const QColor textDim {0x9d, 0xa2, 0xa6}; // 次级文字
inline const QColor accent  {0x4a, 0x8c, 0xff}; // 强调
inline const QColor success {0x57, 0xab, 0x5a}; // 成功
inline const QColor warn    {0xe0, 0xc0, 0x76}; // 警告
inline const QColor error   {0xe0, 0x6c, 0x60}; // 错误
inline const QColor codeBg  {0x23, 0x25, 0x28}; // 代码块底
} // namespace colors

void apply(QApplication& app);
QFont uiFont();   // 微软雅黑 UI 9pt
QFont monoFont(); // Consolas（代码/JSON/日志）

// 状态色圆点的富文本片段（●绿=正常/●红=故障/●灰=未配置）
QString coloredDot(const QColor& c);

} // namespace miderforge::theme
