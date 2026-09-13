// 会话行委托几何单测：行内按钮必须"点得中"——这类问题的传统发现方式是肉眼盯着点，
// 有了纯函数几何就能在 CI 里断言（图标位置/命中区/边界不重叠）。
#include "app/SessionRowDelegate.h"
#include <doctest/doctest.h>

using miderforge::SessionRowDelegate;

TEST_CASE("会话行：钉住/归档按钮几何互不重叠，且都落在行内右侧") {
    const QRect row(0, 0, 260, SessionRowDelegate::kRowHeight);
    const QRect pin = SessionRowDelegate::pinRect(row);
    const QRect arch = SessionRowDelegate::archiveRect(row);

    CHECK(row.contains(pin));
    CHECK(row.contains(arch));
    CHECK_FALSE(pin.intersects(arch));                       // 两个命中区不得重叠
    CHECK(arch.right() <= row.right());                       // 不越出行右边界
    CHECK(pin.right() < arch.left());                         // 钉住在归档左侧
    CHECK(pin.width() == SessionRowDelegate::kButtonSize);
    CHECK(arch.height() == SessionRowDelegate::kButtonSize);
    // 垂直居中：QRect 是闭区间坐标，center() 对偶数高度存在 1px 取舍，故给 ±1 容差
    CHECK(qAbs(pin.center().y() - row.center().y()) <= 1);
    CHECK(qAbs(arch.center().y() - row.center().y()) <= 1);
}

TEST_CASE("会话行：命中测试覆盖整块按钮、按钮外返回 -1、边界点归属明确") {
    const QRect row(10, 20, 244, SessionRowDelegate::kRowHeight);
    const QRect pin = SessionRowDelegate::pinRect(row);
    const QRect arch = SessionRowDelegate::archiveRect(row);

    CHECK(SessionRowDelegate::buttonAt(row, pin.center()) == 0);
    CHECK(SessionRowDelegate::buttonAt(row, arch.center()) == 1);
    CHECK(SessionRowDelegate::buttonAt(row, pin.topLeft()) == 0);      // 左上角在内
    CHECK(SessionRowDelegate::buttonAt(row, arch.bottomRight()) == 1); // 右下角在内
    CHECK(SessionRowDelegate::buttonAt(row, QPoint(row.left() + 4, row.center().y())) == -1);
    CHECK(SessionRowDelegate::buttonAt(row, QPoint(row.center().x(), row.top() + 2)) == -1);
    // 两个按钮之间的间隙不属于任何一个按钮
    const QPoint gap(pin.right() + 1, pin.center().y());
    if (gap.x() < arch.left())
        CHECK(SessionRowDelegate::buttonAt(row, gap) == -1);
}

TEST_CASE("会话行：行高固定为 48（两行布局：标题 + 时间/状态）") {
    SessionRowDelegate d;
    QStyleOptionViewItem opt;
    QModelIndex idx;
    CHECK(d.sizeHint(opt, idx).height() == 48);
    CHECK(SessionRowDelegate::kRowHeight == 48);
}
