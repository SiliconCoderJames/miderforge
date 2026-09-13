// 左栏实现
#include "app/SidebarView.h"
#include "app/ChatWidgets.h"
#include "app/SessionRowDelegate.h"
#include "app/Theme.h"
#include "core/SessionQueries.h"
#include "db/Database.h"
#include <QAction>
#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace miderforge {

namespace {
// 分组小标题（Codex 的 "Projects" / "Recents" 同款：小号、暗色、非交互）
QLabel* sectionLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;font-weight:bold;padding:8px 8px 2px 8px;")
                         .arg(theme::colors::textDim().name()));
    return l;
}
} // namespace

SidebarView::SidebarView(QWidget* parent) : QWidget(parent) {
    setFixedWidth(theme::metrics::sidebarWidth);
    // 裸 QWidget 必须开 WA_StyledBackground，样式表里的 background/border 才会绘制；
    // 否则侧栏底色与右描边都不会出现（分隔线另由 MainWindow 的显式 QFrame 兜底）
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("SidebarView{background-color:%1;border-right:1px solid %2;}")
                      .arg(theme::colors::panel().name(), theme::colors::line().name()));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 12);
    lay->setSpacing(4);

    // ---- 品牌区：🔨 Miderforge / 标语 / 渐变签名线（品牌色→强调色） ----
    auto* brandRow = new QWidget(this);
    auto* brandLay = new QHBoxLayout(brandRow);
    brandLay->setContentsMargins(8, 4, 0, 0);
    brandLay->setSpacing(4);
    auto* brand = new QLabel(QStringLiteral("🔨 Miderforge"), brandRow);
    brand->setStyleSheet(QStringLiteral("color:%1;font-size:13pt;font-weight:bold;")
                             .arg(theme::colors::brand().name()));
    brand->setToolTip(QStringLiteral("Miderforge · %1").arg(theme::brandTagline()));
    brandLay->addWidget(brand);
    brandLay->addStretch(1);
    m_menuBtn = new QToolButton(brandRow);
    m_menuBtn->setText(QStringLiteral("⌄"));
    m_menuBtn->setToolTip(QStringLiteral("主题 / 设置 / 退出"));
    m_menuBtn->setPopupMode(QToolButton::InstantPopup);
    m_menuBtn->setFixedSize(28, 28); // 与三档控件高度同刻度（原 26 是刻度外的孤值）
    m_menuBtn->setStyleSheet(QStringLiteral(
        "QToolButton{border:none;background:transparent;color:%1;font-size:12pt;}"
        "QToolButton:hover{background-color:%2;border-radius:5px;}")
                                 .arg(theme::colors::textDim().name(), theme::colors::btnHover().name()));
    brandLay->addWidget(m_menuBtn);
    lay->addWidget(brandRow);

    auto* tagline = new QLabel(theme::brandTagline(), this);
    tagline->setContentsMargins(8, 0, 0, 0);
    tagline->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;letter-spacing:1px;")
                               .arg(theme::colors::textDim().name()));
    lay->addWidget(tagline);

    // 品牌签名线：品牌色→强调色渐变（品牌识别的主要视觉锚点）
    auto* brandLine = new QFrame(this);
    brandLine->setFixedHeight(2);
    brandLine->setStyleSheet(QStringLiteral("background:%1;border:none;border-radius:1px;")
                                 .arg(theme::brandGradient()));
    lay->addSpacing(6);
    lay->addWidget(brandLine);
    lay->addSpacing(10);

    // ---- 搜索 / 新建（Codex 的 "New chat / 搜索" 两条置顶行） ----
    // 搜索做成可见行而不只是 Ctrl+K：快捷键没有可见入口，新用户发现不了
    auto* searchBtn = new QPushButton(QStringLiteral("🔍  搜索"), this);
    searchBtn->setFixedHeight(theme::metrics::rowDefault);
    searchBtn->setCursor(Qt::PointingHandCursor);
    searchBtn->setToolTip(QStringLiteral("搜索功能页、会话与动作（Ctrl+K）"));
    searchBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:transparent;border:none;border-radius:6px;text-align:left;"
        "padding-left:10px;color:%1;font-size:10pt;}"
        "QPushButton:hover{background-color:%2;}"
        "QPushButton:focus{border:1px solid %3;}")
                                 .arg(theme::colors::text().name(), theme::colors::window().name(),
                                      theme::colors::accent().name()));
    connect(searchBtn, &QPushButton::clicked, this, &SidebarView::searchRequested);
    lay->addWidget(searchBtn);

    m_newBtn = new QPushButton(QStringLiteral("＋  新会话"), this);
    m_newBtn->setObjectName(QStringLiteral("primaryBtn"));
    m_newBtn->setFixedHeight(theme::metrics::rowPrimary);
    m_newBtn->setCursor(Qt::PointingHandCursor);
    m_newBtn->setToolTip(QStringLiteral("新建会话（Ctrl+N）"));
    connect(m_newBtn, &QPushButton::clicked, this, &SidebarView::newSessionRequested);
    lay->addWidget(m_newBtn);
    lay->addSpacing(6);

    // ---- 最近会话（Codex 的 "Recents"）：带搜索过滤 ----
    m_recentHeader = sectionLabel(QStringLiteral("最近会话"), this);
    lay->addWidget(m_recentHeader);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("搜索会话…"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedHeight(theme::metrics::rowDefault);
    m_search->setStyleSheet(QStringLiteral(
        "QLineEdit{background-color:%1;border:1px solid %2;border-radius:6px;"
        "padding:2px 8px;font-size:10pt;color:%3;}"
        "QLineEdit:focus{border-color:%4;}")
                                .arg(theme::colors::window().name(), theme::colors::line().name(),
                                     theme::colors::text().name(), theme::colors::accent().name()));
    // 边打边过滤：不重建数据，只切换行可见性（会话数量级小，足够快且不打断当前高亮）
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& q) { filterSessions(q); });
    lay->addWidget(m_search);

    // 会话整理行：「显示已归档」开关。给可见入口而不是埋在右键菜单里——
    // 会话被归档后"它去哪了"必须一眼能找到答案
    auto* toolsRow = new QWidget(this);
    auto* toolsLay = new QHBoxLayout(toolsRow);
    toolsLay->setContentsMargins(0, 0, 0, 0);
    toolsLay->setSpacing(4);
    toolsLay->addStretch(1);
    m_archivedToggle = new QToolButton(toolsRow);
    // 标签必须写清是"显示"而不是"执行归档"——原标签「📦 归档」被误读成归档按钮，
    // 点了没反应（它只切换列表是否显示归档项）。真正的归档动作在每行右侧的行内图标上。
    m_archivedToggle->setText(QStringLiteral("显示已归档"));
    m_archivedToggle->setCheckable(true);
    m_archivedToggle->setCursor(Qt::PointingHandCursor);
    m_archivedToggle->setToolTip(QStringLiteral("显示已归档会话（归档保留全部消息，可随时取消归档）"));
    m_archivedToggle->setStyleSheet(QStringLiteral(
        "QToolButton{background:transparent;border:none;border-radius:5px;padding:2px 8px;"
        "color:%1;font-size:9.5pt;}"
        "QToolButton:hover{background-color:%2;}"
        "QToolButton:checked{background-color:%3;color:white;}")
                                        .arg(theme::colors::textDim().name(),
                                             theme::colors::window().name(),
                                             theme::colors::accent().name()));
    connect(m_archivedToggle, &QToolButton::toggled, this, [this](bool on) {
        m_showArchived = on;
        loadSessions(m_db);
    });
    toolsLay->addWidget(m_archivedToggle);
    lay->addWidget(toolsRow);

    m_sessionEmpty = new QLabel(QStringLiteral("暂无会话"), this);
    m_sessionEmpty->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;padding:2px 8px;")
                                      .arg(theme::colors::textDim().name()));
    lay->addWidget(m_sessionEmpty);

    m_sessionList = new QListWidget(this);
    // 行外观由 SessionRowDelegate 自绘（标题/时间/置顶标记/悬停出现的钉住与归档图标），
    // 这里只给列表一个透明底——::item 的固定高度会与委托的 sizeHint 打架
    m_sessionList->setStyleSheet(QStringLiteral(
        "QListWidget{background:transparent;border:none;outline:none;}"
        "QListWidget::item{background:transparent;}"));
    m_sessionList->setItemDelegate(new SessionRowDelegate(m_sessionList));
    m_sessionList->setMouseTracking(true);
    m_sessionList->viewport()->setMouseTracking(true);
    m_sessionList->viewport()->installEventFilter(this);
    m_sessionList->setToolTip(QStringLiteral("历史会话（点击恢复消息流；行右侧图标可钉住/归档）"));
    connect(m_sessionList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* cur, QListWidgetItem*) {
                if (cur)
                    emit sessionActivated(cur->data(Qt::UserRole).toLongLong());
            },
            // ⚠️ 必须排队投递：sessionActivated 的下游会 emit sessionsChanged → loadSessions()
            // → m_sessionList->clear()，而 clear() 是 qDeleteAll(item)——同步投递时会在
            // QListWidget 自己处理 currentChanged 的过程中删掉它正在用的 QListWidgetItem。
            // 排队走到事件循环后，控件树已稳定，不存在这一层重入。
            Qt::QueuedConnection);
    // 右键菜单：重命名 / 删除（此前会话只能新建和切换，无法整理）
    m_sessionList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sessionList, &QListWidget::customContextMenuRequested, this,
            &SidebarView::showSessionMenu);
    // 双击标题行直接进入重命名
    connect(m_sessionList, &QListWidget::itemDoubleClicked, this,
            &SidebarView::beginRenameSession);
    lay->addWidget(m_sessionList, 1);

    // ---- 底部：设置 + L1 占用（会话列表拿满剩余高度） ----
    lay->addSpacing(4);
    auto* settingsBtn = new QPushButton(QStringLiteral("⚙  设置"), this);
    settingsBtn->setFixedHeight(theme::metrics::rowDefault);
    settingsBtn->setCursor(Qt::PointingHandCursor);
    settingsBtn->setToolTip(QStringLiteral("供应商 / 记忆 / 技能库 / 任务队列 / 审计日志（Ctrl+,）"));
    settingsBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:transparent;border:none;border-radius:6px;text-align:left;"
        "padding-left:10px;color:%1;font-size:10pt;}"
        "QPushButton:hover{background-color:%2;}"
        "QPushButton:focus{border:1px solid %3;}")
                                   .arg(theme::colors::text().name(), theme::colors::window().name(),
                                        theme::colors::accent().name()));
    connect(settingsBtn, &QPushButton::clicked, this, &SidebarView::settingsRequested);
    lay->addWidget(settingsBtn);

    auto* l1Row = new QWidget(this);
    auto* l1Lay = new QVBoxLayout(l1Row);
    l1Lay->setContentsMargins(4, 4, 4, 0);
    l1Lay->setSpacing(4);
    m_l1Label = new QLabel(l1Row);
    m_l1Label->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;").arg(theme::colors::textDim().name()));
    m_l1Bar = new QProgressBar(l1Row);
    m_l1Bar->setRange(0, 100);
    m_l1Bar->setValue(0);
    m_l1Bar->setTextVisible(false);
    m_l1Bar->setFixedHeight(6);
    l1Lay->addWidget(m_l1Label);
    l1Lay->addWidget(m_l1Bar);
    lay->addWidget(l1Row);
    setL1Usage(0, 4000);
}

void SidebarView::loadSessions(Database* db) {
    if (!db)
        return;
    m_db = db; // 重命名等操作要读权威标题
    // 右键菜单开着时不重建列表：clear() 会删掉菜单处理器持有的行，造成悬垂
    if (m_menuOpen)
        return;
    QSignalBlocker blocker(m_sessionList);
    m_sessionList->clear();
    // 查询走 core/SessionQueries.h 的契约 SQL（与验收测试同一条语句，杜绝假验证）
    const auto rows = db->query(sessionq::listSql(m_showArchived), {});
    for (const auto& r : rows) {
        const QString title = r.value("title").toString();
        // 相对时间（今天 14:32 / 昨天 / N 天前）：一行内把"新旧"讲清楚
        const QString time = sessionq::relativeTime(r.value("updated_at").toLongLong(),
                                                    QDateTime::currentSecsSinceEpoch());
        const bool archived = r.value("archived_at").toLongLong() > 0;
        const bool pinned = r.value("pinned_at").toLongLong() > 0;
        auto* item = new QListWidgetItem(m_sessionList);
        item->setText(title); // 搜索过滤按标题匹配（行外观由委托自绘）
        item->setToolTip(title);
        item->setData(SessionRowDelegate::IdRole, r.value("id").toLongLong());
        item->setData(SessionRowDelegate::ArchivedRole, archived);
        item->setData(SessionRowDelegate::PinnedRole, pinned);
        item->setData(Qt::UserRole + 10, time); // 委托绘制的第二行时间文案
        item->setSizeHint(QSize(0, SessionRowDelegate::kRowHeight));
    }
    // 重载后保持当前过滤条件（否则搜索着搜索着结果会突然全回来）
    filterSessions(m_search ? m_search->text() : QString());
    // 重建后原选中项已随 clear() 消失：用「记住的当前会话 id」把高亮补回来。
    // 不能依赖 QListWidget 自己记住选中——clear() 之后它没有任何当前项，
    // 而 loadSessions 在每次写消息时都会被调用，不补就会一直丢高亮。
    setCurrentSession(m_currentId);
}

// 记录"哪个会话应当高亮"（由 MainWindow 在 currentSessionChanged 时同步）
void SidebarView::setCurrentSession(qint64 id) {
    m_currentId = id;
    QSignalBlocker blocker(m_sessionList);
    for (int i = 0; i < m_sessionList->count(); ++i) {
        auto* item = m_sessionList->item(i);
        if (item->data(Qt::UserRole).toLongLong() == id) {
            m_sessionList->setCurrentItem(item);
            return;
        }
    }
    m_sessionList->setCurrentItem(nullptr); // 新会话：无对应历史行
}

void SidebarView::setSessionEmptyHint(const QString& text) {
    m_sessionEmpty->setText(text);
    m_sessionEmpty->setVisible(!text.isEmpty());
}

// 会话过滤：按标题子串（不区分大小写）切换行可见性。
// 全被过滤掉时给出明确反馈，而不是让列表空着让人以为没会话。
void SidebarView::filterSessions(const QString& query) {
    const QString q = query.trimmed();
    int visible = 0;
    for (int i = 0; i < m_sessionList->count(); ++i) {
        auto* item = m_sessionList->item(i);
        const bool hit = q.isEmpty() || item->text().contains(q, Qt::CaseInsensitive);
        item->setHidden(!hit);
        if (hit)
            ++visible;
    }
    if (!q.isEmpty() && visible == 0)
        setSessionEmptyHint(QStringLiteral("没有匹配「%1」的会话").arg(q));
    else if (m_sessionList->count() == 0)
        setSessionEmptyHint(QStringLiteral("暂无会话（发一条目标即建档）"));
    else
        setSessionEmptyHint(QString());
}

void SidebarView::focusSessionSearch() {
    m_search->setFocus();
    m_search->selectAll();
}

// 会话行右键菜单：重命名（原地编辑）/ 复制标题 / 删除
//
// ⚠️ 这里刻意**不调用 setCurrentItem**：它会同步触发 currentItemChanged → sessionActivated
// → switchToSession → sessionsChanged → loadSessions() → clear()，而 clear() 会 qDeleteAll
// 掉我们正准备使用的那个 QListWidgetItem（悬垂读取 / 崩溃）。
// 同时用 m_menuOpen 挡住菜单嵌套事件循环期间的列表重载——否则列表在菜单开着时被重建，
// 菜单返回后继续用 item 同样是悬垂。
void SidebarView::showSessionMenu(const QPoint& pos) {
    QListWidgetItem* item = m_sessionList->itemAt(pos);
    if (!item)
        return;
    // 先把需要的数据全部拷贝出来，之后不再触碰 item（除原地重命名那一项，见下）
    const qint64 id = item->data(Qt::UserRole).toLongLong();
    const QString fullTitle = item->toolTip();
    const bool wasArchived = item->data(Qt::UserRole + 1).toBool();

    QMenu menu(this);
    QAction* rename = menu.addAction(QStringLiteral("重命名…"));
    QAction* copyTitle = menu.addAction(QStringLiteral("复制标题"));
    const bool wasPinned = item->data(SessionRowDelegate::PinnedRole).toBool();
    QAction* pin = menu.addAction(wasPinned ? QStringLiteral("取消置顶")
                                            : QStringLiteral("置顶会话"));
    QAction* arch = menu.addAction(wasArchived ? QStringLiteral("取消归档")
                                               : QStringLiteral("归档会话"));
    arch->setToolTip(QStringLiteral("归档：从列表隐藏，消息与检索索引全部保留，可随时取消归档"));
    menu.addSeparator();
    QAction* del = menu.addAction(QStringLiteral("删除会话"));
    del->setToolTip(QStringLiteral("删除该会话及其全部消息（不可撤销）"));

    m_menuOpen = true;
    QAction* chosen = menu.exec(m_sessionList->mapToGlobal(pos));
    m_menuOpen = false;

    // 菜单期间列表可能已被重建（例如某个任务正好收尾）→ item 可能已失效。
    // 重新按 id 找回当前仍然存活的条目，找不到就只执行不依赖条目的动作。
    QListWidgetItem* live = nullptr;
    for (int i = 0; i < m_sessionList->count(); ++i) {
        if (m_sessionList->item(i)->data(Qt::UserRole).toLongLong() == id) {
            live = m_sessionList->item(i);
            break;
        }
    }

    if (chosen == rename) {
        beginRenameById(id); // 按 id 走库读，不依赖可能已失效的列表项
    } else if (chosen == pin) {
        emit sessionPinRequested(id, !wasPinned);
    } else if (chosen == arch) {
        emit sessionArchiveRequested(id, !wasArchived);
    } else if (chosen == copyTitle) {
        QGuiApplication::clipboard()->setText(fullTitle);
        Toast::post(window(), QStringLiteral("已复制会话标题"), Toast::Level::Success, 1600);
    } else if (chosen == del) {
        emit sessionDeleteRequested(id);
    }
}

// 会话列表视口事件：把"行内直操作"做实——鼠标移到行上出现钉住/归档图标，
// 点图标即执行（并吞掉这次点击，避免同时触发选中→切换会话）。
bool SidebarView::eventFilter(QObject* watched, QEvent* event) {
    if (!m_sessionList || watched != m_sessionList->viewport())
        return QWidget::eventFilter(watched, event);

    auto* delegate = qobject_cast<SessionRowDelegate*>(m_sessionList->itemDelegate());
    auto updateHover = [&](int row, int btn) {
        if (row == m_hoverRow && btn == m_hoverButton)
            return;
        m_hoverRow = row;
        m_hoverButton = btn;
        if (delegate)
            delegate->setHover(row, btn);
        m_sessionList->viewport()->update();
    };

    switch (event->type()) {
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(event);
        QListWidgetItem* item = m_sessionList->itemAt(me->pos());
        if (!item) {
            updateHover(-1, -1);
            break;
        }
        updateHover(m_sessionList->row(item),
                    SessionRowDelegate::buttonAt(m_sessionList->visualItemRect(item), me->pos()));
        break;
    }
    case QEvent::Leave:
        updateHover(-1, -1);
        break;
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton)
            break;
        QListWidgetItem* item = m_sessionList->itemAt(me->pos());
        if (!item)
            break;
        const int btn = SessionRowDelegate::buttonAt(m_sessionList->visualItemRect(item), me->pos());
        if (btn < 0)
            break; // 点的是行体：交给列表做选中/切换
        const qint64 id = item->data(SessionRowDelegate::IdRole).toLongLong();
        if (btn == 0)
            emit sessionPinRequested(id, !item->data(SessionRowDelegate::PinnedRole).toBool());
        else
            emit sessionArchiveRequested(id, !item->data(SessionRowDelegate::ArchivedRole).toBool());
        return true; // 吞掉事件：行内按钮点击不触发会话切换
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void SidebarView::beginRenameSession(QListWidgetItem* item) {
    if (!item)
        return;
    beginRenameById(item->data(Qt::UserRole).toLongLong());
}

// 重命名（双击标题行或右键菜单都走这里）：旧标题从数据库读取。
// 原实现直接读 item->toolTip()——但双击的第一击会切换会话 → sessionsChanged →
// loadSessions() → clear()（qDeleteAll），第二击拿到的 item 可能已经失效，
// 于是对话框显示乱码标题甚至什么都不发生。按 id 走库读才是稳的。
void SidebarView::beginRenameById(qint64 id) {
    if (id <= 0)
        return;
    QString oldTitle;
    if (m_db) {
        const auto rows = m_db->query(sessionq::titleSql(), {id});
        if (!rows.empty())
            oldTitle = rows.front().value(QStringLiteral("title")).toString();
    }
    if (oldTitle.isEmpty()) { // 库读兜底：从当前列表项找
        for (int i = 0; i < m_sessionList->count(); ++i) {
            if (m_sessionList->item(i)->data(Qt::UserRole).toLongLong() == id) {
                oldTitle = m_sessionList->item(i)->toolTip();
                break;
            }
        }
    }
    if (oldTitle.isEmpty()) {
        Toast::post(window(), QStringLiteral("会话已不存在，无法重命名"), Toast::Level::Warn, 2000);
        return;
    }
    bool ok = false;
    const QString newTitle = QInputDialog::getText(this, QStringLiteral("重命名会话"),
                                                   QStringLiteral("会话标题："), QLineEdit::Normal,
                                                   oldTitle, &ok)
                                 .trimmed();
    if (!ok || newTitle.isEmpty() || newTitle == oldTitle)
        return;
    emit sessionRenameRequested(id, newTitle);
}

void SidebarView::setL1Usage(long long used, long long cap) {
    if (cap <= 0)
        cap = 4000;
    // 显式 long long：混用 int/ll 会让 qBound 重载解析歧义（MSVC C2666）
    const int pct = static_cast<int>(qBound<long long>(0LL, used * 100 / cap, 100LL));
    const bool near = used > cap * 8 / 10; // >80% 变黄预警
    m_l1Label->setText(QStringLiteral("L1 记忆 %1 / %2 tokens").arg(used).arg(cap));
    m_l1Bar->setValue(pct);
    m_l1Bar->setStyleSheet(QStringLiteral(
        "QProgressBar{background-color:%1;border:none;border-radius:3px;}"
        "QProgressBar::chunk{background-color:%2;border-radius:3px;}")
                               .arg(theme::colors::window().name(),
                                    (near ? theme::colors::warn() : theme::colors::accent()).name()));
}

} // namespace miderforge
