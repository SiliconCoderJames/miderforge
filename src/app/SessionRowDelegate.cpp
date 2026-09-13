// 会话行委托实现
#include "app/SessionRowDelegate.h"

#include "app/Theme.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

namespace miderforge {

namespace {
constexpr qreal kPenWidth = 1.4;

// 图钉（thumbtack，45° 朝向）：头部圆 + 针身斜线。尺寸按按钮中心自适应。
void drawPin(QPainter* p, const QRectF& box, const QColor& color) {
    const QPointF c = box.center();
    QPen pen(color, kPenWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    const qreal r = box.width() * 0.20;
    const QPointF head(c.x() + r * 0.25, c.y() - r * 1.05);
    p->drawEllipse(head, r, r);
    p->drawLine(QPointF(head.x() - r * 0.55, head.y() + r * 0.75),
                QPointF(c.x() - r * 1.25, c.y() + r * 1.55));
}

// 归档盒：带盖的方盒 + 中间提手横杠（主流归档图标形状）
void drawArchiveBox(QPainter* p, const QRectF& box, const QColor& color, bool open) {
    const QPointF c = box.center();
    const qreal w = box.width() * 0.62;
    const qreal h = box.height() * 0.46;
    const QRectF body(c.x() - w / 2, c.y() - h / 2, w, h);
    QPen pen(color, kPenWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    p->drawRoundedRect(body, 2.0, 2.0);
    const qreal lidY = body.top() + body.height() * 0.34;
    p->drawLine(QPointF(body.left(), lidY), QPointF(body.right(), lidY));
    if (!open) // 已归档：盒内加一道横杠（"在盒子里"）
        p->drawLine(QPointF(c.x() - w * 0.16, c.y() + h * 0.16),
                    QPointF(c.x() + w * 0.16, c.y() + h * 0.16));
}
} // namespace

SessionRowDelegate::SessionRowDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

QSize SessionRowDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
    return QSize(0, kRowHeight);
}

QRect SessionRowDelegate::archiveRect(const QRect& rowRect) {
    return QRect(rowRect.right() - kRightMargin - kButtonSize,
                 rowRect.center().y() - kButtonSize / 2, kButtonSize, kButtonSize);
}

QRect SessionRowDelegate::pinRect(const QRect& rowRect) {
    const QRect a = archiveRect(rowRect);
    return QRect(a.left() - kButtonGap - kButtonSize, a.top(), kButtonSize, kButtonSize);
}

int SessionRowDelegate::buttonAt(const QRect& rowRect, const QPoint& pos) {
    if (pinRect(rowRect).contains(pos))
        return 0;
    if (archiveRect(rowRect).contains(pos))
        return 1;
    return -1;
}

void SessionRowDelegate::setHover(int row, int button) {
    m_hoverRow = row;
    m_hoverButton = button;
}

void SessionRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                               const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const bool archived = index.data(ArchivedRole).toBool();
    const bool pinned = index.data(PinnedRole).toBool();

    QRect row = option.rect.adjusted(2, 2, -2, -2);
    const qreal radius = 8.0;
    if (selected) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme::colors::accent());
        painter->drawRoundedRect(row, radius, radius);
    } else if (hovered) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme::colors::window());
        painter->drawRoundedRect(row, radius, radius);
    }

    // 文本区：左侧留 10px，右侧给两个按钮让位
    const int actionsW = kButtonSize * 2 + kButtonGap + kRightMargin + 6;
    const QRect textRect = row.adjusted(10, 7, -actionsW, -7);
    const QColor titleColor = selected ? QColor(Qt::white)
                                       : (archived ? theme::colors::textDim() : theme::colors::text());
    const QColor subColor = selected ? QColor(255, 255, 255, 190) : theme::colors::textDim();

    // 第一行：标题（置顶项左侧画一个小图钉，作为"已置顶"的静默标记）
    int titleLeft = textRect.left();
    if (pinned) {
        QRectF mark(titleLeft, textRect.top() + 1, 12, 12);
        drawPin(painter, mark, selected ? QColor(Qt::white) : theme::colors::accent());
        titleLeft += 15;
    }
    QFont titleFont = option.font;
    titleFont.setPointSizeF(10.5);
    titleFont.setBold(pinned);
    painter->setFont(titleFont);
    painter->setPen(titleColor);
    const QFontMetrics titleFm(titleFont);
    const QString title = titleFm.elidedText(index.data(Qt::DisplayRole).toString(),
                                             Qt::ElideMiddle, textRect.right() - titleLeft);
    painter->drawText(QRect(titleLeft, textRect.top(), textRect.right() - titleLeft,
                            titleFm.height()),
                      Qt::AlignVCenter | Qt::AlignLeft, title);

    // 第二行：时间 + 状态（已归档 / 已置顶）
    QString sub = index.data(Qt::UserRole + 10).toString(); // 时间文案由 SidebarView 预格式化
    if (archived)
        sub += QStringLiteral(" · 已归档");
    else if (pinned)
        sub += QStringLiteral(" · 已置顶");
    QFont subFont = option.font;
    subFont.setPointSizeF(9.0);
    painter->setFont(subFont);
    painter->setPen(subColor);
    const QFontMetrics subFm(subFont);
    painter->drawText(QRect(textRect.left() + (pinned ? 15 : 0),
                            textRect.top() + titleFm.height() + 2,
                            textRect.right() - textRect.left(), subFm.height()),
                      Qt::AlignVCenter | Qt::AlignLeft,
                      subFm.elidedText(sub, Qt::ElideRight, textRect.width()));

    // 右侧行内按钮：悬停该行时出现（选中行同样显示，避免"选中后动作消失"）
    if (m_hoverRow == index.row() || selected) {
        const QRect pr = pinRect(option.rect);
        const QRect ar = archiveRect(option.rect);
        struct Btn { QRect rect; int id; bool isPin; };
        const Btn btns[] = {{pr, 0, true}, {ar, 1, false}};
        for (const auto& b : btns) {
            const bool isHover = (m_hoverRow == index.row() && m_hoverButton == b.id);
            if (isHover) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(selected ? QColor(255, 255, 255, 46) : theme::colors::btnHover());
                painter->drawRoundedRect(b.rect.adjusted(1, 1, -1, -1), 6.0, 6.0);
            }
            const QColor iconColor =
                selected ? QColor(Qt::white)
                         : (b.isPin && pinned ? theme::colors::accent() : theme::colors::textDim());
            if (b.isPin)
                drawPin(painter, QRectF(b.rect), iconColor);
            else
                drawArchiveBox(painter, QRectF(b.rect), iconColor, archived);
        }
    }

    painter->restore();
}

} // namespace miderforge
