// 命令面板实现
#include "app/CommandPalette.h"
#include "app/Theme.h"
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>

namespace miderforge {

QVector<int> CommandPalette::rank(const QStringList& titles, const QString& query) {
    QVector<int> out;
    const QString q = query.trimmed();
    if (q.isEmpty()) {
        out.reserve(titles.size());
        for (int i = 0; i < titles.size(); ++i)
            out.push_back(i);
        return out;
    }
    struct Scored { int idx; int pos; int len; };
    QVector<Scored> hits;
    for (int i = 0; i < titles.size(); ++i) {
        const int pos = titles[i].indexOf(q, 0, Qt::CaseInsensitive);
        if (pos < 0)
            continue;
        hits.push_back({i, pos, int(titles[i].size())}); // QString::size() 是 qsizetype，显式收窄
    }
    std::sort(hits.begin(), hits.end(), [](const Scored& a, const Scored& b) {
        if (a.pos != b.pos)
            return a.pos < b.pos; // 前缀命中（pos=0）优先
        if (a.len != b.len)
            return a.len < b.len; // 同级时更短的标题更精确
        return a.idx < b.idx;     // 稳定序
    });
    out.reserve(hits.size());
    for (const auto& h : hits)
        out.push_back(h.idx);
    return out;
}

CommandPalette::CommandPalette(QWidget* parent) : QDialog(parent) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setModal(true);
    setFixedWidth(560);
    setStyleSheet(QStringLiteral("QDialog{background-color:%1;border:1px solid %2;border-radius:12px;}")
                      .arg(theme::colors::panel().name(), theme::colors::line().name()));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 8);
    lay->setSpacing(8);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(QStringLiteral("搜索功能页、会话，或输入动作…"));
    m_input->setFixedHeight(38);
    m_input->setStyleSheet(QStringLiteral(
        "QLineEdit{background-color:%1;border:1px solid %2;border-radius:8px;"
        "padding:4px 12px;font-size:11pt;color:%3;}"
        "QLineEdit:focus{border-color:%4;}")
                               .arg(theme::colors::window().name(), theme::colors::line().name(),
                                    theme::colors::text().name(), theme::colors::accent().name()));
    connect(m_input, &QLineEdit::textChanged, this, [this] { refilter(); });
    m_input->installEventFilter(this); // ↑↓ 从输入框驱动列表
    lay->addWidget(m_input);

    m_list = new QListWidget(this);
    m_list->setStyleSheet(QStringLiteral(
        "QListWidget{background:transparent;border:none;outline:none;font-size:11pt;}"
        "QListWidget::item{height:34px;border-radius:7px;padding-left:10px;color:%2;margin:1px 0;}"
        "QListWidget::item:selected{background-color:%1;color:white;}")
                              .arg(theme::colors::accent().name(), theme::colors::text().name()));
    m_list->setFixedHeight(320);
    connect(m_list, &QListWidget::itemActivated, this, [this] { acceptCurrent(); });
    connect(m_list, &QListWidget::itemClicked, this, [this] { acceptCurrent(); });
    lay->addWidget(m_list);

    m_hint = new QLabel(QStringLiteral("↑↓ 选择 · Enter 执行 · Esc 关闭"), this);
    m_hint->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;").arg(theme::colors::textDim().name()));
    lay->addWidget(m_hint);
}

void CommandPalette::setItems(const QVector<Item>& items) {
    m_items = items;
    m_input->clear();
    refilter();
}

void CommandPalette::refilter() {
    QStringList titles;
    titles.reserve(m_items.size());
    for (const auto& it : m_items)
        titles << it.title;
    const QVector<int> order = rank(titles, m_input->text());

    QSignalBlocker blocker(m_list);
    m_list->clear();
    for (int idx : order) {
        const auto& it = m_items[idx];
        auto* row = new QListWidgetItem(m_list);
        row->setText(it.hint.isEmpty() ? QStringLiteral("%1   %2").arg(it.kind, it.title)
                                       : QStringLiteral("%1   %2        %3").arg(it.kind, it.title, it.hint));
        row->setData(Qt::UserRole, idx); // 存回 m_items 下标，避免过滤后错位
    }
    if (m_list->count() > 0)
        m_list->setCurrentRow(0); // 默认选中第一条，Enter 直接可用
    m_hint->setText(m_list->count() == 0
                        ? QStringLiteral("没有匹配项")
                        : QStringLiteral("↑↓ 选择 · Enter 执行 · Esc 关闭"));
}

bool CommandPalette::eventFilter(QObject* obj, QEvent* ev) {
    // 焦点在输入框时也要能用 ↑↓/Enter 操作列表（否则必须 Tab 过去，键盘流会断）
    if (obj == m_input && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Down || ke->key() == Qt::Key_Up) {
            const int delta = ke->key() == Qt::Key_Down ? 1 : -1;
            const int n = m_list->count();
            if (n > 0)
                m_list->setCurrentRow((m_list->currentRow() + delta + n) % n);
            return true;
        }
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            acceptCurrent();
            return true;
        }
    }
    return QDialog::eventFilter(obj, ev);
}

void CommandPalette::keyPressEvent(QKeyEvent* ev) {
    if (ev->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QDialog::keyPressEvent(ev);
}

void CommandPalette::acceptCurrent() {
    auto* row = m_list->currentItem();
    if (!row)
        return;
    const int idx = row->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_items.size())
        return;
    const Item it = m_items[idx];
    accept(); // 先关闭再派发，避免调用方在面板存活期间改动列表
    if (it.pageIndex >= 0)
        emit pageChosen(it.pageIndex);
    else if (it.sessionId >= 0)
        emit sessionChosen(it.sessionId);
    else if (it.actionId >= 0)
        emit actionChosen(it.actionId);
}

} // namespace miderforge
