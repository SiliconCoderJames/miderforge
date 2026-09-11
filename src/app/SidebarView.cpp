// 左栏实现
#include "app/SidebarView.h"
#include "app/ChatWidgets.h"
#include "app/Theme.h"
#include "db/Database.h"
#include <QAction>
#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace miderforge {

namespace {
// 分组小标题（Codex 的 "Projects" / "Recents" 同款：小号、暗色、非交互）
QLabel* sectionLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;font-weight:bold;padding:8px 8px 2px 8px;")
                         .arg(theme::colors::textDim().name()));
    return l;
}
} // namespace

SidebarView::SidebarView(QWidget* parent) : QWidget(parent) {
    setFixedWidth(260);
    setStyleSheet(QStringLiteral("SidebarView{background-color:%1;border-right:1px solid %2;}")
                      .arg(theme::colors::panel().name(), theme::colors::line().name()));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 12);
    lay->setSpacing(3);

    // ---- 品牌区：🔨 Miderforge / 标语 / 渐变签名线（品牌色→强调色） ----
    auto* brandRow = new QWidget(this);
    auto* brandLay = new QHBoxLayout(brandRow);
    brandLay->setContentsMargins(8, 6, 0, 0);
    brandLay->setSpacing(6);
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
    m_menuBtn->setFixedSize(26, 26);
    m_menuBtn->setStyleSheet(QStringLiteral(
        "QToolButton{border:none;background:transparent;color:%1;font-size:12pt;}"
        "QToolButton:hover{background-color:%2;border-radius:5px;}")
                                 .arg(theme::colors::textDim().name(), theme::colors::btnHover().name()));
    brandLay->addWidget(m_menuBtn);
    lay->addWidget(brandRow);

    auto* tagline = new QLabel(theme::brandTagline(), this);
    tagline->setContentsMargins(8, 0, 0, 0);
    tagline->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;letter-spacing:1px;")
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
    searchBtn->setFixedHeight(30);
    searchBtn->setCursor(Qt::PointingHandCursor);
    searchBtn->setToolTip(QStringLiteral("搜索功能页、会话与动作（Ctrl+K）"));
    searchBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:transparent;border:none;border-radius:6px;text-align:left;"
        "padding-left:10px;color:%1;font-size:9pt;}"
        "QPushButton:hover{background-color:%2;}"
        "QPushButton:focus{border:1px solid %3;}")
                                 .arg(theme::colors::text().name(), theme::colors::window().name(),
                                      theme::colors::accent().name()));
    connect(searchBtn, &QPushButton::clicked, this, &SidebarView::searchRequested);
    lay->addWidget(searchBtn);

    m_newBtn = new QPushButton(QStringLiteral("＋  新会话"), this);
    m_newBtn->setObjectName(QStringLiteral("primaryBtn"));
    m_newBtn->setFixedHeight(36);
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
    m_search->setFixedHeight(28);
    m_search->setStyleSheet(QStringLiteral(
        "QLineEdit{background-color:%1;border:1px solid %2;border-radius:6px;"
        "padding:2px 8px;font-size:8.5pt;color:%3;}"
        "QLineEdit:focus{border-color:%4;}")
                                .arg(theme::colors::window().name(), theme::colors::line().name(),
                                     theme::colors::text().name(), theme::colors::accent().name()));
    // 边打边过滤：不重建数据，只切换行可见性（会话数量级小，足够快且不打断当前高亮）
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& q) { filterSessions(q); });
    lay->addWidget(m_search);

    m_sessionEmpty = new QLabel(QStringLiteral("暂无会话"), this);
    m_sessionEmpty->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;padding:2px 8px;")
                                      .arg(theme::colors::textDim().name()));
    lay->addWidget(m_sessionEmpty);

    m_sessionList = new QListWidget(this);
    m_sessionList->setStyleSheet(QStringLiteral(
        "QListWidget{background:transparent;border:none;font-size:8pt;outline:none;}"
        "QListWidget::item{height:34px;border-radius:6px;padding-left:8px;color:%2;margin:1px 0;}"
        "QListWidget::item:hover{background-color:%1;}"
        "QListWidget::item:selected{background-color:%3;color:white;}")
                                     .arg(theme::colors::window().name(), theme::colors::textDim().name(),
                                          theme::colors::accent().name()));
    m_sessionList->setToolTip(QStringLiteral("历史会话（点击恢复消息流）"));
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
    settingsBtn->setFixedHeight(30);
    settingsBtn->setCursor(Qt::PointingHandCursor);
    settingsBtn->setToolTip(QStringLiteral("供应商 / 记忆 / 技能库 / 任务队列 / 审计日志（Ctrl+,）"));
    settingsBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:transparent;border:none;border-radius:6px;text-align:left;"
        "padding-left:10px;color:%1;font-size:9pt;}"
        "QPushButton:hover{background-color:%2;}"
        "QPushButton:focus{border:1px solid %3;}")
                                   .arg(theme::colors::text().name(), theme::colors::window().name(),
                                        theme::colors::accent().name()));
    connect(settingsBtn, &QPushButton::clicked, this, &SidebarView::settingsRequested);
    lay->addWidget(settingsBtn);

    auto* l1Row = new QWidget(this);
    auto* l1Lay = new QVBoxLayout(l1Row);
    l1Lay->setContentsMargins(6, 6, 6, 0);
    l1Lay->setSpacing(3);
    m_l1Label = new QLabel(l1Row);
    m_l1Label->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;").arg(theme::colors::textDim().name()));
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
    // 右键菜单开着时不重建列表：clear() 会删掉菜单处理器持有的行，造成悬垂
    if (m_menuOpen)
        return;
    QSignalBlocker blocker(m_sessionList);
    m_sessionList->clear();
    const auto rows = db->query(QStringLiteral(
        "SELECT id, title, updated_at FROM sessions ORDER BY updated_at DESC LIMIT 100"), {});
    for (const auto& r : rows) {
        const QString title = r.value("title").toString();
        const QString time = QDateTime::fromSecsSinceEpoch(r.value("updated_at").toLongLong())
                                 .toString(QStringLiteral("MM-dd hh:mm"));
        auto* item = new QListWidgetItem(m_sessionList);
        item->setText(QStringLiteral("%1\n%2").arg(title, time));
        item->setToolTip(title);
        item->setData(Qt::UserRole, r.value("id").toLongLong());
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

    QMenu menu(this);
    QAction* rename = menu.addAction(QStringLiteral("重命名…"));
    QAction* copyTitle = menu.addAction(QStringLiteral("复制标题"));
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
        if (live)
            beginRenameSession(live);
        else
            Toast::post(window(), QStringLiteral("会话已不存在，无法重命名"), Toast::Level::Warn, 2000);
    } else if (chosen == copyTitle) {
        QGuiApplication::clipboard()->setText(fullTitle);
        Toast::post(window(), QStringLiteral("已复制会话标题"), Toast::Level::Success, 1600);
    } else if (chosen == del) {
        emit sessionDeleteRequested(id);
    }
}

// 原地重命名：把行切成可编辑状态，提交后回到普通行并通知上层写库
void SidebarView::beginRenameSession(QListWidgetItem* item) {
    if (!item)
        return;
    const qint64 id = item->data(Qt::UserRole).toLongLong();
    const QString oldTitle = item->toolTip();
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
