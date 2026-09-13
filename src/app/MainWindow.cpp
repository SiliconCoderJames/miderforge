// 主窗口实现
#include "app/MainWindow.h"
#include "app/AuditLogView.h"
#include "app/ChatWidgets.h"
#include "app/CommandPalette.h"
#include "app/FirstRunWizard.h"
#include "app/MemoryView.h"
#include "app/PreviewPane.h"
#include "app/ProviderPanel.h"
#include "app/SessionView.h"
#include "app/SettingsDialog.h"
#include "app/SidebarView.h"
#include "app/SkillView.h"
#include "app/TaskQueueView.h"
#include "app/Theme.h"
#include "core/AdjudicationService.h"
#include "core/AgentLoop.h"
#include "core/AppContext.h"
#include "core/SessionQueries.h"
#include "llm/ProviderManager.h"
#include "notify/EmailNotifier.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QShortcut>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QVBoxLayout>

namespace miderforge {

namespace {
// 导航项：文字 + 堆叠页索引（设置项为对话框，不走堆叠页）
constexpr int kNavCount = 7;
constexpr int kNavSettingsIndex = 6;
} // namespace

MainWindow::MainWindow(ProviderManager* pm, AgentLoop* loop, EventBus* events, Database* db,
                       MemoryManager* mem, SkillManager* skills, EmailNotifier* mail, QWidget* parent)
    : QMainWindow(parent), m_pm(pm), m_loop(loop), m_events(events), m_db(db), m_mem(mem),
      m_skills(skills), m_mail(mail) {
    // 矛盾扫描 v2：独立裁决服务（自带 ChatClient，不与 AgentLoop 主链路抢占）
    m_adjudicator = new AdjudicationService(pm, this);
    setWindowTitle(QStringLiteral("Miderforge"));
    resize(1440, 900);
    setMinimumSize(1024, 680);

    buildCentral();
    buildMenus();
    buildShortcuts();
    // 说明：原顶部 QToolBar 已拆除（对齐 Codex/ZCode 的极简顶栏）——
    // 新建会话在左栏、权限档/供应商/模型档内联在 Composer 底行、停止在 Composer 按钮组
    buildStatusBar();
    refreshStatusLabels();
    refreshL1Footer(); // 导航底栏 L1 占用用真实值，避免启动后恒显 0
    restoreLastSession(); // 接着上次的会话继续（与 Claude/Codex 一致）

    // 今日 token 统计联动
    connect(m_loop, &AgentLoop::tokensChanged, this, [this](long long) { refreshStatusLabels(); });

    // 任务收尾后记忆/摘要可能已更新 → 同步导航底栏 L1 占用
    connect(m_loop, &AgentLoop::loopFinished, this, [this](bool, const QString&) { refreshL1Footer(); });

    // 技能自沉淀确认（规格 7：弹确认；同名 merge 由 SkillManager 处理）
    connect(m_loop, &AgentLoop::skillProposed, this,
            [this](const QString& name, const QString& description, const QString& md) {
                const auto ret = QMessageBox::question(
                    this, QStringLiteral("沉淀新技能"),
                    QStringLiteral("本次任务成功且工具调用≥5次，建议沉淀为技能：\n\n%1\n\n%2\n\n"
                                   "保存后同类任务将自动在「可用技能」中出现。是否保存？")
                        .arg(name, description));
                if (ret == QMessageBox::Yes)
                    m_loop->acceptSkillProposal(name, description, md);
            });

    // ---- M4 系统托盘（规格 4.9）：常驻托盘 + 任务完成/失败/熔断气泡 + 邮件 ----
    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    m_tray->setToolTip(QStringLiteral("Miderforge"));
    auto* trayMenu = new QMenu(this);
    QAction* showAction = trayMenu->addAction(QStringLiteral("显示主窗口"));
    QAction* newTaskAction = trayMenu->addAction(QStringLiteral("新建任务"));
    trayMenu->addSeparator();
    QAction* quitAction = trayMenu->addAction(QStringLiteral("退出"));
    connect(showAction, &QAction::triggered, this, [this] {
        showNormal();
        activateWindow();
    });
    connect(newTaskAction, &QAction::triggered, this, [this] {
        showNormal();
        switchNav(1);
    });
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
    m_tray->setContextMenu(trayMenu);
    // 单/双击托盘图标 → 恢复主窗口（最小化/隐藏后的标准唤回方式）
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            showNormal();
            activateWindow();
            raise();
        }
    });
    m_tray->show();

    // 任务完成/失败/熔断 → 托盘气泡 + 邮件通知（规格 11 触发条件）
    connect(m_loop, &AgentLoop::loopFinished, this, [this](bool ok, const QString& summary) {
        if (m_tray)
            m_tray->showMessage(QStringLiteral("Miderforge"),
                                ok ? QStringLiteral("任务已完成：%1").arg(summary.left(80))
                                   : QStringLiteral("任务熔断/失败：%1").arg(summary.left(80)),
                                QSystemTrayIcon::Information, 5000);
        if (m_mail) {
            QString err;
            m_mail->send(ok ? QStringLiteral("Miderforge 任务完成")
                            : QStringLiteral("Miderforge 任务失败/熔断"),
                         QStringLiteral("<p>状态：%1</p><pre>%2</pre>")
                             .arg(ok ? QStringLiteral("succeeded") : QStringLiteral("failed"),
                                  summary.toHtmlEscaped()),
                         &err);
        }
    });
    // 供应商切换 → 状态灯刷新 + 托盘告警（探测恢复由供应商视图「测试连接」手动触发）
    connect(m_pm, &ProviderManager::providerChanged, this, [this] {
        refreshProviderCombo();
        refreshStatusLabels();
        if (m_pm->failedOver() && m_tray)
            m_tray->showMessage(QStringLiteral("Miderforge"),
                                QStringLiteral("供应商连续失败，已切换到故障转移备胎"),
                                QSystemTrayIcon::Warning, 5000);
    });
}

void MainWindow::buildCentral() {
    auto* central = new QWidget(this);
    auto* lay = new QHBoxLayout(central);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // ---- 中央区：只有会话视图（功能面板都收进设置对话框，见下方注释） ----
    m_stack = new QStackedWidget(central);
    m_sessionView = new SessionView(m_db, m_loop, m_pm, m_stack);
    m_stack->addWidget(m_sessionView); // 0 会话（唯一常驻页）

    // 1 内置查看器：HTML / Markdown / 文本（设置页在首次打开时才追加，故此处索引固定为 1）
    m_previewPage = new PreviewPane(m_stack);
    m_previewIndex = m_stack->addWidget(m_previewPage);

    // ---- 功能面板：依然构造出来，但归本窗口持有、由设置对话框借去当堆叠页 ----
    // 决策：原先 5 个面板平铺在左栏导航里（任务队列/技能库/记忆/供应商/审计日志），
    // 6 项文字常驻占了近 200px 且视觉重量压过会话列表。它们都是低频配置/监控页，
    // 收进设置（左导航 + 堆叠页）后左栏只剩"会话"这一件事。
    // 面板的父控件是本窗口而非对话框：对话框每次打开都新建，若让面板随它销毁，
    // 每次关闭设置都会重建一遍面板（丢状态、白耗一次 DB 查询）。
    m_taskQueuePage = new TaskQueueView(m_db, m_loop, this);
    m_skillPage = new SkillView(m_skills, this);
    m_memoryPage = new MemoryView(m_mem, m_adjudicator, this);
    m_providerPage = new ProviderPanel(m_pm, this);
    m_auditPage = new AuditLogView(m_events, this);
    // 上述面板创建时默认可见：先藏起来，挂进设置对话框后由堆叠页决定显示
    const QVector<QWidget*> panels = {m_taskQueuePage, m_skillPage, m_memoryPage,
                                      m_providerPage, m_auditPage};
    for (QWidget* w : panels)
        w->hide();

    // ---- 左栏：品牌 / 搜索 / 新建会话 / 最近会话 / 设置 + L1 ----
    m_sidebar = new SidebarView(central);
    connect(m_sidebar, &SidebarView::searchRequested, this, &MainWindow::openCommandPalette);
    connect(m_sidebar, &SidebarView::newSessionRequested, this, [this] {
        m_sessionView->newSession();
        switchNav(0);
    });
    connect(m_sidebar, &SidebarView::settingsRequested, this, [this] { openSettings(); });
    connect(m_sidebar, &SidebarView::sessionActivated, this, [this](qint64 id) {
        // 切换被拒（任务执行中）时把高亮回弹到真实当前会话
        if (!m_sessionView->requestSwitchSession(id))
            m_sidebar->setCurrentSession(m_sessionView->currentSessionId());
    });
    // 会话列表与高亮由左栏维护，SessionView 只广播状态变化
    connect(m_sessionView, &SessionView::sessionsChanged, this,
            [this] { m_sidebar->loadSessions(m_db); });
    connect(m_sessionView, &SessionView::currentSessionChanged, this,
            [this](qint64 id) { m_sidebar->setCurrentSession(id); });
    // 会话重命名 / 删除（右键菜单）：直接改库后刷新左栏
    connect(m_sidebar, &SidebarView::sessionRenameRequested, this,
            [this](qint64 id, const QString& title) {
                if (!m_db)
                    return;
                m_db->execute(sessionq::renameSql(), {title, id});
                m_sidebar->loadSessions(m_db);
                Toast::post(this, QStringLiteral("已重命名为「%1」").arg(title), Toast::Level::Success, 1800);
            });
    // 会话归档 / 取消归档：只改 archived_at，消息与 FTS 索引原样保留（可随时复原）
    connect(m_sidebar, &SidebarView::sessionArchiveRequested, this,
            [this](qint64 id, bool archive) {
                if (!m_db)
                    return;
                m_db->execute(archive ? sessionq::archiveSql() : sessionq::unarchiveSql(), {id});
                m_sidebar->loadSessions(m_db);
                Toast::post(this,
                            archive ? QStringLiteral("会话已归档（点左栏「显示已归档」可查看）")
                                    : QStringLiteral("已取消归档"),
                            Toast::Level::Info, 2200);
            });
    // 会话置顶 / 取消置顶：只改 pinned_at，列表按 pinned_at 排最前
    connect(m_sidebar, &SidebarView::sessionPinRequested, this, [this](qint64 id, bool pin) {
        if (!m_db)
            return;
        m_db->execute(pin ? sessionq::pinSql() : sessionq::unpinSql(), {id});
        m_sidebar->loadSessions(m_db);
        Toast::post(this, pin ? QStringLiteral("会话已置顶") : QStringLiteral("已取消置顶"),
                    Toast::Level::Info, 1800);
    });
    connect(m_sidebar, &SidebarView::sessionDeleteRequested, this, [this](qint64 id) {
        if (!m_db)
            return;
        if (QMessageBox::question(
                this, QStringLiteral("删除会话"),
                QStringLiteral("确定删除该会话及其全部消息？此操作不可撤销。"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        // messages 与 sessions 一并删除：只删 sessions 会留下孤儿消息，session_search 仍能搜到
        m_db->execute(QStringLiteral("DELETE FROM messages WHERE session_id=?"), {id});
        m_db->execute(QStringLiteral("DELETE FROM sessions WHERE id=?"), {id});
        if (m_sessionView->currentSessionId() == id)
            m_sessionView->newSession(); // 删的是当前会话：退回未建档状态
        m_sidebar->loadSessions(m_db);
        Toast::post(this, QStringLiteral("会话已删除"), Toast::Level::Info, 1800);
    });

    // 品牌行右侧 ⌄ 菜单：主题皮肤 / 设置 / 退出
    auto* menu = new QMenu(m_sidebar->menuButton());
    auto* themeGroup = new QActionGroup(menu);
    themeGroup->setExclusive(true);
    for (int k = 0; k < theme::palettes().size(); ++k) {
        const auto& pal = theme::palettes()[k];
        auto* act = menu->addAction(QString::fromUtf8(pal.zh));
        act->setCheckable(true);
        act->setChecked(k == theme::paletteIndex());
        themeGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, k] {
            theme::setPaletteIndex(k);
            Toast::post(this, QStringLiteral("已切换到「%1」，重启应用后完全生效")
                                  .arg(QString::fromUtf8(theme::palettes()[k].zh)),
                        Toast::Level::Success);
        });
    }
    menu->addSeparator();
    QAction* settingsAct = menu->addAction(QStringLiteral("设置…"));
    connect(settingsAct, &QAction::triggered, this, &MainWindow::openSettings);
    QAction* quitAct = menu->addAction(QStringLiteral("退出"));
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);
    m_sidebar->menuButton()->setMenu(menu);

    m_sidebar->loadSessions(m_db);

    // 侧栏与内容区之间的分隔线：显式 1px 竖线。
    // 侧栏样式表里虽写了 border-right，但裸 QWidget 不设 WA_StyledBackground 时样式表的
    // 边框根本不会绘制——只靠 QSS 的分隔线在实机上"看不见"（用户反馈缺分隔线）。
    auto* divider = new QFrame(central);
    divider->setFrameShape(QFrame::VLine);
    divider->setFrameShadow(QFrame::Plain);
    divider->setFixedWidth(1);
    divider->setStyleSheet(QStringLiteral("background-color:%1;border:none;")
                               .arg(theme::colors::line().name()));
    lay->addWidget(m_sidebar);
    lay->addWidget(divider);
    lay->addWidget(m_stack, 1);
    setCentralWidget(central);
}

void MainWindow::buildMenus() {
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
    QAction* newSession = fileMenu->addAction(QStringLiteral("新建会话"));
    newSession->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
    connect(newSession, &QAction::triggered, this, [this] {
        m_sessionView->newSession();
        switchNav(0);
    });
    fileMenu->addSeparator();
    QAction* quit = fileMenu->addAction(QStringLiteral("退出"));
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    QMenu* sessionMenu = menuBar()->addMenu(QStringLiteral("会话"));
    QAction* stopTask = sessionMenu->addAction(QStringLiteral("停止当前任务"));
    connect(stopTask, &QAction::triggered, m_loop, &AgentLoop::cancel);
    QAction* clearSession = sessionMenu->addAction(QStringLiteral("清空当前会话"));
    connect(clearSession, &QAction::triggered, m_sessionView, &SessionView::newSession);

    QMenu* taskMenu = menuBar()->addMenu(QStringLiteral("任务"));
    QAction* newTask = taskMenu->addAction(QStringLiteral("新建任务"));
    newTask->setEnabled(false);
    newTask->setToolTip(QStringLiteral("任务队列在 M2 里程碑实装"));

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("视图"));
    // 功能面板已收进设置对话框：这里改为「会话 + 6 个设置入口」，不再依赖 m_pageNames
    //（m_pageNames 已废弃；此前 for(i<7) 读空列表会越界崩溃）
    // page 序号对应 SettingsDialog 的堆叠页序（见其构造函数里 addNavEntry 的调用顺序）
    struct ViewEntry { const char* title; int page; };
    static const ViewEntry kViews[] = {
        {"会话", 0},
        {"设置 · 常规", 0},
        {"设置 · 外观与主题", 1},
        {"设置 · 模型与供应商", 2},
        {"设置 · 记忆与检索", 3},
        {"设置 · 任务队列", 7},
        {"设置 · 审计日志", 8},
    };
    int shortcut = 1;
    for (const auto& v : kViews) {
        QAction* go = viewMenu->addAction(QString::fromUtf8(v.title));
        go->setShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(shortcut++)));
        const int page = v.page;
        connect(go, &QAction::triggered, this, [this, page] {
            if (page == 0)
                switchNav(0);
            else
                openSettingsAt(page);
        });
    }

    viewMenu->addSeparator();
    // 内置查看器：HTML / Markdown / 文本预览（无 WebEngine 依赖，走富文本引擎）
    QAction* previewAct = viewMenu->addAction(QStringLiteral("查看器（HTML / Markdown）"));
    previewAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+8")));
    connect(previewAct, &QAction::triggered, this, &MainWindow::openPreview);

    QMenu* toolMenu = menuBar()->addMenu(QStringLiteral("工具"));
    QAction* wizard = toolMenu->addAction(QStringLiteral("设置…"));
    wizard->setToolTip(QStringLiteral("打开设置对话框（同 视图→设置 / Ctrl+7）"));
    connect(wizard, &QAction::triggered, this, &MainWindow::openSettings);

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
    QAction* about = helpMenu->addAction(QStringLiteral("关于 Miderforge"));
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::information(this, QStringLiteral("关于 Miderforge"),
                                 QStringLiteral("Miderforge %1\n云端大脑 + 本地身体的常驻编码 Agent。\n"
                                                "云端大脑：多个大模型 API\n本地身体：C++ 常驻进程，可操作本机文件、执行命令\n"
                                                "三大资产：记忆 / 技能库 / 数据库")
                                     .arg(QApplication::applicationVersion().isEmpty()
                                              ? QStringLiteral("0.1.0 (M0)")
                                              : QApplication::applicationVersion()));
    });
}

// 全局键盘交互：键盘优先，键盘上能做完主要动作（对齐 Codex/ZCode 的快捷键习惯）
void MainWindow::buildShortcuts() {
    auto add = [this](const QString& seq, auto fn) {
        auto* sc = new QShortcut(QKeySequence(seq), this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, fn);
    };
    add(QStringLiteral("Ctrl+K"), [this] { openCommandPalette(); });          // 命令面板
    add(QStringLiteral("Ctrl+L"), [this] { m_sessionView->focusInput(); });    // 聚焦输入框
    add(QStringLiteral("Ctrl+F"), [this] { m_sidebar->focusSessionSearch(); }); // 搜会话
    add(QStringLiteral("Ctrl+,"), [this] { openSettings(); });                 // 设置（同 mac 习惯）
    add(QStringLiteral("Ctrl+Shift+T"), [this] {                               // 切换主题皮肤
        const int n = theme::palettes().size();
        theme::setPaletteIndex((theme::paletteIndex() + 1) % n);
        // 换成浮层提示：切皮肤是低风险动作，弹模态框会打断手头操作
        Toast::post(this, QStringLiteral("已切换到「%1」，重启应用后完全生效")
                              .arg(QString::fromUtf8(theme::palettes()[theme::paletteIndex()].zh)),
                    Toast::Level::Success);
    });
}

// 命令面板（Ctrl+K）：会话 + 设置入口 + 动作
void MainWindow::openCommandPalette() {
    QVector<CommandPalette::Item> items;
    // 1) 设置入口：功能面板与配置都在设置里，面板项直接直达对应页
    const struct { const char* title; int page; } settingsEntries[] = {
        {"设置 · 常规", 0},
        {"设置 · 外观与主题", 1},
        {"设置 · 模型与供应商", 2},
        {"设置 · 记忆与检索", 3},
        {"设置 · 技能库", 4},
        {"设置 · 任务队列", 7},
        {"设置 · 审计日志", 8},
        {"设置 · 邮件通知", 9},
    };
    for (const auto& e : settingsEntries) {
        CommandPalette::Item it;
        it.kind = QStringLiteral("设置");
        it.title = QString::fromUtf8(e.title);
        it.actionId = 100 + e.page; // 100+ 段：设置页直达
        items.push_back(it);
    }
    // 2) 会话（最近 100 条，与左栏同源）
    if (m_db) {
        const auto rows = m_db->query(QStringLiteral(
            "SELECT id, title, updated_at FROM sessions ORDER BY updated_at DESC LIMIT 100"), {});
        for (const auto& r : rows) {
            CommandPalette::Item it;
            it.kind = QStringLiteral("会话");
            it.title = r.value("title").toString();
            it.hint = QDateTime::fromSecsSinceEpoch(r.value("updated_at").toLongLong())
                          .toString(QStringLiteral("MM-dd hh:mm"));
            it.sessionId = r.value("id").toLongLong();
            items.push_back(it);
        }
    }
    // 3) 动作
    const struct { const char* title; int id; } actions[] = {
        {"新建会话", 1}, {"打开设置", 2}, {"切换主题皮肤", 3},
        {"聚焦输入框", 4}, {"搜索会话", 5}, {"测试当前供应商连接", 6},
    };
    for (const auto& a : actions) {
        CommandPalette::Item it;
        it.kind = QStringLiteral("动作");
        it.title = QString::fromUtf8(a.title);
        it.actionId = a.id;
        items.push_back(it);
    }

    CommandPalette dlg(this);
    connect(&dlg, &CommandPalette::pageChosen, this, &MainWindow::switchNav);
    connect(&dlg, &CommandPalette::sessionChosen, this, [this](qint64 id) {
        switchNav(0);
        if (!m_sessionView->requestSwitchSession(id)) // 任务执行中会被拒，回弹高亮
            m_sidebar->setCurrentSession(m_sessionView->currentSessionId());
    });
    connect(&dlg, &CommandPalette::actionChosen, this, [this](int id) {
        // 100+ 段 = 直达设置页（page = id-100，-1 为设置默认页）
        if (id >= 100) {
            openSettingsAt(id - 100);
            return;
        }
        switch (id) {
        case 1: m_sessionView->newSession(); switchNav(0); break;
        case 2: openSettings(); break;
        case 3: theme::setPaletteIndex((theme::paletteIndex() + 1) % theme::palettes().size());
                Toast::post(this, QStringLiteral("已切换到「%1」，重启应用后完全生效")
                                      .arg(QString::fromUtf8(
                                          theme::palettes()[theme::paletteIndex()].zh)),
                            Toast::Level::Success); break;
        case 4: m_sessionView->focusInput(); break;
        case 5: m_sidebar->focusSessionSearch(); break;
        case 6: {
            const ProviderConfig* p = m_pm->activeProvider();
            QString err;
            int ms = 0;
            if (p && m_pm->testConnection(p->name, &err, &ms))
                QMessageBox::information(this, QStringLiteral("Miderforge"),
                                         QStringLiteral("连接正常：%1（%2 ms）").arg(p->name).arg(ms));
            else
                QMessageBox::warning(this, QStringLiteral("Miderforge"),
                                     QStringLiteral("连接失败：%1").arg(err.isEmpty() ? QStringLiteral("未知原因") : err));
            refreshStatusLabels();
            break;
        }
        default: break;
        }
    });
    dlg.setItems(items);
    dlg.exec();
}

// 启动即恢复最近一次会话：桌面常驻 Agent 通常是被反复打开的，
// 每次都停在空白新会话上会让人以为记录丢了。没有历史时保持空态引导卡。
void MainWindow::restoreLastSession() {
    if (!m_db || !m_sessionView || !m_sidebar)
        return;
    const auto rows = m_db->query(
        QStringLiteral("SELECT id FROM sessions ORDER BY updated_at DESC LIMIT 1"), {});
    if (rows.empty())
        return;
    const qint64 id = rows.front().value("id").toLongLong();
    if (m_sessionView->requestSwitchSession(id))
        m_sidebar->setCurrentSession(id);
}

void MainWindow::buildStatusBar() {
    // 品牌签名：版本 + 标语（左侧固定）
    auto* brand = new QLabel(QStringLiteral("Miderforge · %1").arg(theme::brandTagline()), this);
    brand->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;").arg(theme::colors::brand().name()));
    statusBar()->addWidget(brand);
    QLabel* sep = new QLabel(QStringLiteral("│"), this);
    sep->setStyleSheet(QStringLiteral("color:%1;").arg(theme::colors::line().name()));
    statusBar()->addWidget(sep);

    // 只留 Composer/左栏没有的信息：模型与权限档分别由 Composer 的供应商/模型标签和
    // 权限下拉承载，L1 占用在左栏底部——重复展示只会制造噪声
    m_statusTokens = new QLabel(this);
    m_statusQueue = new QLabel(this);
    for (auto* l : {m_statusTokens, m_statusQueue}) {
        l->setContentsMargins(8, 0, 8, 0);
        statusBar()->addWidget(l);
    }
    statusBar()->setStyleSheet(QStringLiteral("QStatusBar{background-color:%1;color:%2;}")
                                                      .arg(theme::colors::panel().name(), theme::colors::textDim().name()));
    connect(m_sessionView, &SessionView::queueCountChanged, this, [this](int n) {
        m_queueCount = n;
        m_statusQueue->setText(QStringLiteral("队列: %1").arg(n));
    });
}

void MainWindow::refreshProviderCombo() {
    // 供应商下拉归属 Composer（SessionView）；此处转发，供设置对话框关闭后刷新
    if (m_sessionView)
        m_sessionView->refreshProviderCombo();
}

void MainWindow::applyPermissionMode(int idx) {
    // 设置对话框改了权限档 → 同步到 Composer 输入区下拉（AppContext 由对方写入）
    m_sessionView->setPermissionMode(idx);
    refreshStatusLabels();
}

void MainWindow::refreshStatusLabels() {
    const ProviderConfig* active = m_pm ? m_pm->activeProvider() : nullptr;
    // 状态栏只显示 Composer/左栏没有的信息（模型由 Composer 的模型标签展示，权限档由权限下拉展示）。
    // 这里仍维护 AppContext::activeModelLabel，因为其他面板会读它。
    if (active && active->configured) {
        AppContext::instance().activeModelLabel =
            QStringLiteral("%1·%2").arg(active->name, m_pm->modelForTier(*active, QStringLiteral("main")));
    } else {
        AppContext::instance().activeModelLabel.clear();
    }
    m_statusTokens->setText(
        QStringLiteral("今日 tokens: %1").arg(AppContext::instance().todayTokens.load()));
    m_statusQueue->setText(QStringLiteral("队列: %1").arg(m_queueCount)); // 由 queueCountChanged 维护，勿硬写 0
}

void MainWindow::refreshL1Footer() {
    if (!m_mem || !m_sidebar)
        return;
    // L1 占用进度条已迁入左栏底部（原活动栏底部 36px 宽挤成一坨）
    m_sidebar->setL1Usage(m_mem->l1Tokens(), AppContext::instance().l1TokenLimit);
}

void MainWindow::switchNav(int index) {
    // 0 = 会话；>=0 的其余索引 = 设置里的某一页（设置已嵌入内容区，不再是独立窗口）
    if (index <= 0) {
        m_stack->setCurrentIndex(0);
        return;
    }
    openSettingsAt(index);
}

// 无参版本：满足菜单/快捷键槽连接（param 版改名以避免与槽重载产生歧义）
void MainWindow::openSettings() {
    openSettingsAt(-1);
}

// 打开设置对话框并按需直达某一页。
// 对话框本体由本窗口持有（m_settings）：它在构造时接收 5 个功能面板当堆叠页，
// 每次打开都新建的话会反复重挂面板、丢掉面板内部状态（滚动位置、筛选条件）。
// pageIndex < 0 = 打开默认页（通用）。
void MainWindow::openSettingsAt(int pageIndex) {
    if (!m_settings) {
        SettingsDialog::Panels panels;
        panels.taskQueue = m_taskQueuePage;
        panels.skills = m_skillPage;
        panels.memory = m_memoryPage;
        panels.providers = m_providerPage;
        panels.audit = m_auditPage;
        m_settings = new SettingsDialog(m_pm, m_mail, panels,
                                        [this](int idx) { applyPermissionMode(idx); }, m_stack);
        // 嵌入内容区（页 1）：原先用独立 QDialog + exec()，会脱离主窗口布局、
        // 多一个任务栏条目，也与 Codex/DSH 的"内容区切换"观感不一致
        m_settingsPageIndex = m_stack->addWidget(m_settings);
        connect(m_settings, &SettingsDialog::closeRequested, this, [this] { switchNav(0); });
        connect(m_settings, &SettingsDialog::saved, this, [this] {
            refreshStatusLabels();
            refreshProviderCombo();
            if (m_sessionView)
                m_sessionView->refreshProviderCombo();
            refreshL1Footer();
        });
    }
    if (pageIndex >= 0)
        m_settings->showPage(pageIndex);
    m_stack->setCurrentIndex(m_settingsPageIndex);
}

// 打开内置查看器：内容区切到查看器页，并按当前工作区刷新可查看文件列表
void MainWindow::openPreview() {
    if (!m_previewPage)
        return;
    m_previewPage->setWorkspaceRoot(AppContext::instance().workspaceRoot);
    m_stack->setCurrentIndex(m_previewIndex);
}

} // namespace miderforge
