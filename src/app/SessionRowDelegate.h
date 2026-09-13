// 会话行委托：两行布局（标题 / 时间·状态）+ 右侧**行内直操作图标**（钉住、归档）。
// 设计参照主流 Agent 的会话列表：动作就在行上，鼠标悬停即出现——不用先右键再找菜单。
// 图标全部用 QPainter 矢量绘制（不用 emoji）：任何字号/缩放下都清晰，风格可控（细描边）。
// 几何计算是纯函数（pinRect/archiveRect/buttonAt），可单测——避免"点不中按钮"这类问题只能靠肉眼发现。
#pragma once
#include <QRect>
#include <QSize>
#include <QStyledItemDelegate>

namespace miderforge {

class SessionRowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    // 行数据角色（SidebarView::loadSessions 写入）
    enum Role { IdRole = Qt::UserRole, ArchivedRole, PinnedRole };

    static constexpr int kRowHeight = 48;
    static constexpr int kButtonSize = 26;
    static constexpr int kButtonGap = 4;
    static constexpr int kRightMargin = 6;

    explicit SessionRowDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    // 悬停状态：由 SidebarView 在鼠标事件里设置（行号 + 按钮：-1 无 / 0 钉住 / 1 归档）
    void setHover(int row, int button);

    // ---- 纯函数几何（可单测）----
    static QRect pinRect(const QRect& rowRect);
    static QRect archiveRect(const QRect& rowRect);
    // 命中测试：-1=不在按钮上，0=钉住，1=归档
    static int buttonAt(const QRect& rowRect, const QPoint& pos);

private:
    int m_hoverRow = -1;
    int m_hoverButton = -1;
};

} // namespace miderforge
