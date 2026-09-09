// 主窗口实现
#include "app/MainWindow.h"
#include "app/AuditLogView.h"
#include "app/FirstRunWizard.h"
#include "app/MemoryView.h"
#include "app/ProviderPanel.h"
#include "app/SessionView.h"
#include "app/SettingsDialog.h"
#include "app/SkillView.h"
#include "app/TaskQueueView.h"
#include "app/Theme.h"
#include "core/AdjudicationService.h"
#include "core/AgentLoop.h"
#include "core/AppContext.h"
#include "llm/ProviderManager.h"
#include "notify/EmailNotifier.h"
#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QToolBar>
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
    buildToolbar();
    buildStatusBar();
    refreshProviderCombo();
    refreshStatusLabels();
    refreshL1Footer(); // 导航底栏 L1 占用用真实值，避免启动后恒显 0

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

    // ---- 左侧导航（260px，规格 4.2） ----
    m_nav = new QTreeWidget(central);
    m_nav->setFixedWidth(260);
    m_nav->setHeaderHidden(true);
    m_nav->setRootIsDecorated(false);
    m_nav->setStyleSheet(QStringLiteral(
        "QTreeWidget{background-color:%1;border:none;font-size:10pt;outline:none;}"
        "QTreeWidget::item{height:38px;border-radius:8px;margin:2px 10px;color:%3;}"
        "QTreeWidget::item:hover{background-color:%4;}"
        "QTreeWidget::item:selected{background-color:%2;color:white;font-weight:bold;}")
                             .arg(theme::colors::window.name(), theme::colors::accent.name(),
                                  theme::colors::textDim.name(), theme::colors::panel.name()));
    const QStringList navItems = {
        QStringLiteral("💬  会话"),
        QStringLiteral("📋  任务队列"),
        QStringLiteral("🧰  技能库"),
        QStringLiteral("🧠  记忆"),
        QStringLiteral("🔌  供应商"),
        QStringLiteral("📜  审计日志"),
        QStringLiteral("⚙️  设置"),
    };
    for (const QString& text : navItems)
        m_nav->addTopLevelItem(new QTreeWidgetItem({text}));
    m_nav->setCurrentItem(m_nav->topLevelItem(0));
    connect(m_nav, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
                if (cur)
                    switchNav(m_nav->indexOfTopLevelItem(cur));
            });

    // 导航底部：L1 记忆占用进度条（4000 token 上限，>80% 变黄；M2 接入真实数据）
    auto* navFooter = new QWidget(central);
    auto* footerLay = new QVBoxLayout(navFooter);
    footerLay->setContentsMargins(10, 4, 10, 8);
    m_l1BarLabel = new QLabel(QStringLiteral("L1 记忆：0 / 4000 tokens"), navFooter);
    m_l1BarLabel->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;").arg(theme::colors::textDim.name()));
    m_l1Bar = new QProgressBar(navFooter);
    m_l1Bar->setRange(0, 4000);
    m_l1Bar->setValue(0);
    m_l1Bar->setTextVisible(false);
    m_l1Bar->setFixedHeight(6);
    m_l1Bar->setStyleSheet(QStringLiteral(
        "QProgressBar{background-color:%1;border:none;border-radius:3px;}"
        "QProgressBar::chunk{background-color:%2;border-radius:3px;}")
                               .arg(theme::colors::panel.name(), theme::colors::accent.name()));
    footerLay->addWidget(m_l1BarLabel);
    footerLay->addWidget(m_l1Bar);

    auto* navColumn = new QWidget(central);
    auto* navLay = new QVBoxLayout(navColumn);
    navLay->setContentsMargins(0, 0, 0, 0);
    navLay->setSpacing(0);
    navLay->addWidget(m_nav, 1);
    navLay->addWidget(navFooter);
    navColumn->setStyleSheet(QStringLiteral("background-color:%1;").arg(theme::colors::window.name()));

    // ---- 中央堆叠区 ----
    m_stack = new QStackedWidget(central);
    m_sessionView = new SessionView(m_loop, m_stack);
    m_stack->addWidget(m_sessionView); // 0 会话
    m_stack->addWidget(new TaskQueueView(m_db, m_loop, m_stack)); // 1 任务队列（M2 实装）
    m_stack->addWidget(new SkillView(m_skills, m_stack)); // 2 技能库（M3 实装）
    m_stack->addWidget(new MemoryView(m_mem, m_adjudicator, m_stack)); // 3 记忆（M2 实装）
    m_stack->addWidget(new ProviderPanel(m_pm, m_stack)); // 4 供应商（M4 实装）
    m_stack->addWidget(new AuditLogView(m_events, m_stack)); // 5 审计日志（M1 实装）

    lay->addWidget(navColumn);
    lay->addWidget(m_stack, 1);
    setCentralWidget(central);
}

QWidget* MainWindow::makePlaceholder(const QString& text) const {
    auto* label = new QLabel(text, const_cast<MainWindow*>(this));
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color:%1;font-size:12pt;").arg(theme::colors::textDim.name()));
    return label;
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
    for (int i = 0; i < 6; ++i) {
        QAction* go = viewMenu->addAction(m_nav->topLevelItem(i)->text(0));
        go->setShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1)));
        connect(go, &QAction::triggered, this, [this, i] { switchNav(i); });
    }

    QMenu* toolMenu = menuBar()->addMenu(QStringLiteral("工具"));
    QAction* wizard = toolMenu->addAction(QStringLiteral("运行首次配置向导…"));
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

void MainWindow::buildToolbar() {
    QToolBar* bar = addToolBar(QStringLiteral("main"));
    bar->setMovable(false);
    bar->setIconSize(QSize(18, 18));

    QAction* newSession = bar->addAction(QStringLiteral("＋ 新建会话"));
    newSession->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
    connect(newSession, &QAction::triggered, this, [this] {
        m_sessionView->newSession();
        switchNav(0);
    });
    bar->addSeparator();

    bar->addWidget(new QLabel(QStringLiteral(" 权限模式: "), bar));
    auto* permCombo = new QComboBox(bar);
    permCombo->addItems({QStringLiteral("Suggest"), QStringLiteral("Auto Edit"), QStringLiteral("Full Access")});
    connect(permCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        applyPermissionMode(idx);
    });
    connect(m_sessionView, &SessionView::permissionModeChanged, this, [this, permCombo](int idx) {
        QSignalBlocker blocker(permCombo);
        permCombo->setCurrentIndex(idx);
        refreshStatusLabels();
    });
    bar->addWidget(permCombo);
    bar->addSeparator();

    bar->addWidget(new QLabel(QStringLiteral(" 供应商: "), bar));
    m_providerCombo = new QComboBox(bar);
    connect(m_providerCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onProviderComboChanged);
    bar->addWidget(m_providerCombo);
    bar->addSeparator();

    m_stopAction = bar->addAction(QStringLiteral("■ 停止"));
    connect(m_stopAction, &QAction::triggered, m_loop, &AgentLoop::cancel);
}

void MainWindow::buildStatusBar() {
    m_statusModel = new QLabel(this);
    m_statusTokens = new QLabel(this);
    m_statusMode = new QLabel(this);
    m_statusQueue = new QLabel(this);
    for (auto* l : {m_statusModel, m_statusTokens, m_statusMode, m_statusQueue}) {
        l->setContentsMargins(8, 0, 8, 0);
        statusBar()->addWidget(l);
    }
    statusBar()->setStyleSheet(QStringLiteral("QStatusBar{background-color:%1;color:%2;}")
                                                      .arg(theme::colors::panel.name(), theme::colors::textDim.name()));
    connect(m_sessionView, &SessionView::queueCountChanged, this, [this](int n) {
        m_statusQueue->setText(QStringLiteral("队列: %1").arg(n));
    });
}

void MainWindow::refreshProviderCombo() {
    QSignalBlocker blocker(m_providerCombo);
    m_providerCombo->clear();
    const ProviderConfig* active = m_pm ? m_pm->activeProvider() : nullptr;
    for (const auto& cfg : m_pm->all()) {
        // QComboBox 不渲染富文本：用纯文本圆点（状态色由状态栏的富文本圆点表达）
        const QChar dot = cfg.configured ? QChar(0x25CF) : QChar(0x25CB); // ● / ○
        m_providerCombo->addItem(QStringLiteral("%1 %2").arg(dot, cfg.name), cfg.name);
    }
    if (active) {
        const int idx = m_providerCombo->findData(active->name);
        if (idx >= 0)
            m_providerCombo->setCurrentIndex(idx);
    }
}

void MainWindow::applyPermissionMode(int idx) {
    AppContext::instance().permissionMode = static_cast<PermissionMode>(idx);
    m_sessionView->setPermissionMode(idx); // 与输入区下拉框双向同步
    refreshStatusLabels();
}

void MainWindow::refreshStatusLabels() {
    const ProviderConfig* active = m_pm ? m_pm->activeProvider() : nullptr;
    const QColor dotColor = !active ? theme::colors::textDim
                                    : (active->configured ? theme::colors::success : theme::colors::error);
    // 决策: M0 状态栏固定展示 main 档模型；M4 三档路由接入后由 Router 维护该标签
    if (active && active->configured) {
        AppContext::instance().activeModelLabel =
            QStringLiteral("%1·%2").arg(active->name, m_pm->modelForTier(*active, QStringLiteral("main")));
    } else if (!active || !active->configured) {
        AppContext::instance().activeModelLabel.clear();
    }
    m_statusModel->setText(
        QStringLiteral("%1 %2").arg(theme::coloredDot(dotColor),
                                    AppContext::instance().activeModelLabel.isEmpty()
                                        ? QStringLiteral("未配置供应商")
                                        : AppContext::instance().activeModelLabel));
    m_statusTokens->setText(
        QStringLiteral("今日 tokens: %1").arg(AppContext::instance().todayTokens.load()));
    m_statusMode->setText(
        QStringLiteral("模式: %1").arg(permissionModeName(AppContext::instance().permissionMode)));
    m_statusQueue->setText(QStringLiteral("队列: 0"));
}

void MainWindow::refreshL1Footer() {
    if (!m_mem)
        return;
    const qint64 used = m_mem->l1Tokens();
    m_l1BarLabel->setText(QStringLiteral("L1 记忆：%1 / 4000 tokens").arg(used));
    m_l1Bar->setValue(static_cast<int>(qBound<qint64>(qint64(0), used, qint64(4000))));
    // >80% 变黄（规格：L1 接近上限预警）
    m_l1Bar->setStyleSheet(used > 4000 * 8 / 10
                               ? QStringLiteral("QProgressBar{background-color:%1;border:none;border-radius:3px;}"
                                                "QProgressBar::chunk{background-color:%2;border-radius:3px;}")
                                     .arg(theme::colors::panel.name(), theme::colors::warn.name())
                               : QStringLiteral("QProgressBar{background-color:%1;border:none;border-radius:3px;}"
                                                "QProgressBar::chunk{background-color:%2;border-radius:3px;}")
                                     .arg(theme::colors::panel.name(), theme::colors::accent.name()));
}

void MainWindow::switchNav(int index) {
    if (index == kNavSettingsIndex) {
        openSettings();
        // 设置是对话框：导航选框回弹到会话页
        m_nav->setCurrentItem(m_nav->topLevelItem(0));
        return;
    }
    m_stack->setCurrentIndex(index);
}

void MainWindow::openSettings() {
    SettingsDialog dialog(m_pm, m_mail,
                          [this](int idx) { applyPermissionMode(idx); }, this);
    if (dialog.exec() == QDialog::Accepted) {
        refreshProviderCombo();
        refreshStatusLabels();
    }
}

void MainWindow::onProviderComboChanged(int index) {
    if (index < 0 || !m_pm)
        return;
    const QString name = m_providerCombo->itemData(index).toString();
    m_pm->setActive(name);
    refreshStatusLabels();
}

} // namespace miderforge
