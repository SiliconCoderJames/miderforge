// 会话视图实现
#include "app/SessionView.h"
#include "app/Theme.h"
#include "core/AppContext.h"
#include "db/Database.h"
#include "llm/ProviderManager.h"
#include <QBoxLayout>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QToolButton>
#include <algorithm>

namespace miderforge {

SessionView::SessionView(Database* db, AgentLoop* loop, ProviderManager* pm, QWidget* parent)
    : QWidget(parent), m_loop(loop), m_db(db), m_pm(pm) {
    // 主列由 MainWindow 与左栏 SidebarView 并列排布；这里只负责中栏内容
    auto* rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(8, 6, 8, 8);
    rootLay->setSpacing(6);

    rootLay->addWidget(buildTopStrip());

    // 中部消息流：QScrollArea 包 QVBoxLayout，新消息自动滚动到底（自底向上滚动）
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setStyleSheet(QStringLiteral("QScrollArea{background:%1;}").arg(theme::colors::window().name()));
    m_feedHost = new QWidget(m_scroll);
    m_feedLay = new QVBoxLayout(m_feedHost);
    m_feedLay->setContentsMargins(4, 4, 4, 4);
    m_feedLay->setSpacing(8);
    m_scroll->setWidget(m_feedHost);
    rootLay->addWidget(m_scroll, 1);
    buildEmptyState(); // 空态引导卡（此时是布局里唯一的 widget）
    // 末尾再放 stretch：消息自底向上堆叠，空态卡落在其上方，两者天然互斥
    m_feedLay->addStretch(1);
    buildScrollToBottom(); // 悬浮在消息流右下角（不进布局，随滚动显隐）

    rootLay->addWidget(buildInputArea());

    // ---- AgentLoop 事件接线（全部经信号回 UI 线程） ----
    connect(m_loop, &AgentLoop::taskStarted, this, &SessionView::onTaskStarted);
    connect(m_loop, &AgentLoop::stateChanged, this, &SessionView::onStateChanged);
    connect(m_loop, &AgentLoop::toolAwaitingConfirm, this, &SessionView::onToolAwaitingConfirm);
    connect(m_loop, &AgentLoop::roundChanged, this, &SessionView::onRoundChanged);
    connect(m_loop, &AgentLoop::thinkingDelta, this, [this](const QString& d) {
        if (!m_curAssistant) {
            m_curAssistant = new AssistantBlock(m_feedHost); // 懒创建，仅在创建时入布局
            m_feedLay->addWidget(m_curAssistant);
        }
        m_curAssistant->appendThinking(d);
        scrollToEnd();
    });
    connect(m_loop, &AgentLoop::contentDelta, this, [this](const QString& d) {
        if (!m_curAssistant) {
            m_curAssistant = new AssistantBlock(m_feedHost);
            m_feedLay->addWidget(m_curAssistant);
        }
        m_curAssistant->appendContent(d);
        scrollToEnd();
    });
    connect(m_loop, &AgentLoop::toolCallDelta, this, &SessionView::onToolCallDelta);
    connect(m_loop, &AgentLoop::toolCallStarted, this, &SessionView::onToolCallStarted);
    connect(m_loop, &AgentLoop::toolCallFinished, this, &SessionView::onToolCallFinished);
    connect(m_loop, &AgentLoop::streamRetrying, this, &SessionView::onStreamRetrying);
    connect(m_loop, &AgentLoop::loopFinished, this, &SessionView::onLoopFinished);
    connect(m_loop, &AgentLoop::loopFailed, this, &SessionView::onLoopFailed);
    connect(m_loop, &AgentLoop::tokensChanged, this, [this](long long total) {
        m_tokensLabel->setText(QStringLiteral("tokens: %1").arg(total));
    });

    // 已用时节拍器
    connect(&m_elapsedTimer, &QTimer::timeout, this, [this] {
        const qint64 s = m_taskClock.elapsed() / 1000;
        m_elapsedLabel->setText(QStringLiteral("已用时 %1:%2")
                                    .arg(s / 60, 2, 10, QLatin1Char('0'))
                                    .arg(s % 60, 2, 10, QLatin1Char('0')));
    });

    setStateLabel(QStringLiteral("空闲"), theme::colors::textDim());
}

// 变更审查条重建：m_changes（path → 增删/diff）→ ChangeSummaryBar。
// 用列表而非 QHash 迭代，保证顺序稳定（按文件路径排序，视图刷新不跳动）
void SessionView::refreshChangesBar() {
    QStringList paths = m_changes.keys();
    std::sort(paths.begin(), paths.end());
    QVector<int> added;
    QVector<int> removed;
    added.reserve(paths.size());
    removed.reserve(paths.size());
    for (const QString& p : paths) {
        const auto it = m_changes.constFind(p);
        added.push_back(it == m_changes.constEnd() ? 0 : it->added);
        removed.push_back(it == m_changes.constEnd() ? 0 : it->removed);
    }
    m_changesBar->setChanges(paths, added, removed);
}

void SessionView::onChangedFileClicked(const QString& path) {
    const auto it = m_changes.constFind(path);
    if (it == m_changes.constEnd())
        return;
    // 记住本次要展示的内容，供「复制 diff」使用
    const QString diffText = it->diff;
    const QString title = QStringLiteral("变更审查 · %1（+%2/−%3）")
                              .arg(path).arg(it->added).arg(it->removed);
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.resize(860, 620);
    auto* lay = new QVBoxLayout(&dlg);
    auto* view = new QPlainTextEdit(&dlg);
    view->setReadOnly(true);
    view->setFont(theme::monoFont());
    view->setPlainText(diffText);
    lay->addWidget(view);
    // 复制按钮：之前只能手动全选，这里补上并给出浮层回执
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    auto* copyBtn = new QPushButton(QStringLiteral("复制 diff"), &dlg);
    copyBtn->setCursor(Qt::PointingHandCursor);
    connect(copyBtn, &QPushButton::clicked, &dlg, [this, diffText] {
        QGuiApplication::clipboard()->setText(diffText);
        Toast::post(window(), QStringLiteral("已复制 diff 到剪贴板"), Toast::Level::Success, 1800);
    });
    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), &dlg);
    closeBtn->setObjectName(QStringLiteral("primaryBtn"));
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    btnRow->addWidget(copyBtn);
    btnRow->addWidget(closeBtn);
    lay->addLayout(btnRow);
    dlg.exec();
}

void SessionView::onPickAttachment() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择上下文文件（路径将随目标一并发给 Agent）"),
        AppContext::instance().workspaceRoot);
    for (const QString& f : files) {
        if (!m_attachedFiles.contains(f)) {
            m_attachedFiles.append(f);
            auto* chip = new QPushButton(QStringLiteral("✕ ") + QFileInfo(f).fileName(), m_chipsHost);
            chip->setFlat(true);
            chip->setToolTip(f);
            chip->setStyleSheet(QStringLiteral(
                "QPushButton{background-color:%1;border:1px solid %2;border-radius:10px;"
                "padding:2px 10px;font-size:8pt;color:%3;}")
                                    .arg(theme::colors::codeBg().name(), theme::colors::line().name(),
                                         theme::colors::textDim().name()));
            const QString path = f;
            connect(chip, &QPushButton::clicked, this, [this, path, chip] {
                m_attachedFiles.removeAll(path);
                chip->deleteLater();
                m_chipsHost->setVisible(!m_attachedFiles.isEmpty());
            });
            m_chipsLay->addWidget(chip);
        }
    }
    m_chipsHost->setVisible(!m_attachedFiles.isEmpty());
}

// 顶部一行：变更审查条（左，有变更才显示）+ 任务进度指标（右）。
// 进度从 Composer 底行上移到这里：轮次/tokens/已用时属于「任务状态」，
// 和权限/供应商这类「全局设置」混在输入框旁会显得拥挤（Codex/ZCode 都不这么做）。
QWidget* SessionView::buildTopStrip() {
    auto* strip = new QFrame(this);
    m_topStrip = strip;
    // 空闲：无容器（指标只是淡字）｜运行中：品牌色描边卡片（一眼看出任务在跑）
    strip->setStyleSheet(QStringLiteral("QFrame{background:transparent;border:none;}"));
    auto* lay = new QHBoxLayout(strip);
    lay->setContentsMargins(2, 0, 2, 0);
    lay->setSpacing(10);

    m_changesBar = new ChangeSummaryBar(strip);
    connect(m_changesBar, &ChangeSummaryBar::fileActivated, this, &SessionView::onChangedFileClicked);
    lay->addWidget(m_changesBar, 1);

    m_goalLabel = new QLabel(strip);
    m_goalLabel->setVisible(false); // 目标原文由用户气泡承载，此处仅保留既有赋值有效
    m_roundLabel = new QLabel(QStringLiteral("第 0/25 轮"), strip);
    m_tokensLabel = new QLabel(QStringLiteral("tokens: 0"), strip);
    m_elapsedLabel = new QLabel(QStringLiteral("已用时 00:00"), strip);
    m_stateLabel = new QLabel(strip);
    for (auto* l : {m_roundLabel, m_tokensLabel, m_elapsedLabel}) {
        l->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;").arg(theme::colors::textDim().name()));
        lay->addWidget(l);
    }
    m_stateLabel->setStyleSheet(QStringLiteral("font-weight:bold;font-size:8pt;"));
    lay->addWidget(m_stateLabel);
    return strip;
}

// 空态主视觉（品牌门面）：品牌徽标 + 标语 + 主标题 + 4 张能力卡。
// 这是新用户打开软件看到的第一屏，要让品牌和「能干什么」同时立住。
void SessionView::buildEmptyState() {
    m_emptyState = new QWidget(m_scroll);
    auto* lay = new QVBoxLayout(m_emptyState);
    lay->setContentsMargins(0, 0, 0, 24);
    lay->setSpacing(0);

    // 顶部弹簧 + 底部弹簧 → 整块内容垂直居中（不是贴在顶上）
    lay->addStretch(3);

    // 徽标：渐变描边方块（品牌色→强调色），内嵌 🔨
    auto* logo = new QLabel(QStringLiteral("🔨"), m_emptyState);
    logo->setAlignment(Qt::AlignCenter);
    logo->setFixedSize(64, 64);
    logo->setStyleSheet(QStringLiteral(
        "QLabel{background:%1;border-radius:16px;font-size:26pt;}")
                            .arg(theme::brandGradient(0.0, 0.0, 1.0, 1.0)));
    auto* logoRow = new QHBoxLayout();
    logoRow->addStretch(1);
    logoRow->addWidget(logo);
    logoRow->addStretch(1);
    lay->addLayout(logoRow);
    lay->addSpacing(16);

    // 主标题
    auto* title = new QLabel(QStringLiteral("要锻造什么？"), m_emptyState);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("color:%1;font-size:24pt;font-weight:bold;")
                             .arg(theme::colors::text().name()));
    lay->addWidget(title);
    lay->addSpacing(8);

    // 品牌标语（品牌色，字距拉开）
    auto* tagline = new QLabel(theme::brandTagline(), m_emptyState);
    tagline->setAlignment(Qt::AlignCenter);
    tagline->setStyleSheet(QStringLiteral("color:%1;font-size:9pt;letter-spacing:2px;")
                               .arg(theme::colors::brand().name()));
    lay->addWidget(tagline);
    lay->addSpacing(12);

    // 品牌签名短线（居中，主标题下方的视觉锚点）
    auto* sigLine = new QFrame(m_emptyState);
    sigLine->setFixedSize(72, 2);
    sigLine->setStyleSheet(QStringLiteral("background:%1;border:none;border-radius:1px;")
                               .arg(theme::brandGradient()));
    auto* sigRow = new QHBoxLayout();
    sigRow->addStretch(1);
    sigRow->addWidget(sigLine);
    sigRow->addStretch(1);
    lay->addLayout(sigRow);
    lay->addSpacing(14);

    // 一句说明：讲清工作方式与成长性（不是营销话术，是行为描述）
    auto* hint = new QLabel(
        QStringLiteral("用中文下达目标，它会自主规划 → 执行 → 观察 → 反思，直到完成。\n"
                       "每次结果都沉淀进分层记忆与技能库——干得越多，越懂你的项目。"),
        m_emptyState);
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:%1;font-size:9pt;line-height:160%;")
                            .arg(theme::colors::textDim().name()));
    lay->addWidget(hint);
    lay->addSpacing(26);

    // 四张能力卡：标题 + 一句目标描述（点击填入输入框）
    struct Card { const char* title; const char* desc; const char* goal; };
    static const Card cards[] = {
        {"理解代码", "梳理构建流程与模块职责", "梳理这个仓库的构建流程，说明各模块职责与关键调用链"},
        {"新建功能", "实现功能并补单元测试", "为当前项目新增一个功能并补充对应单元测试"},
        {"审查改动", "检查改动风险与可改进处", "检查当前未提交的改动，指出风险与可改进之处"},
        {"修复失败", "跑测试定位并修复失败", "跑一遍测试，定位失败原因并修复"},
    };
    auto* grid = new QGridLayout();
    grid->setSpacing(12);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    auto* cardWrap = new QWidget(m_emptyState);
    cardWrap->setLayout(grid);
    // 必须给固定宽度：只设 maximumWidth 时父布局会让它塌缩到 sizeHint，
    // 卡片被压成不到 100px 宽、标题被截断（实测「理解代码」只剩「理解代」）
    cardWrap->setFixedWidth(660);
    int i = 0;
    for (const auto& c : cards) {
        // 卡片内用富文本排版：粗体标题 + 暗色描述（Codex 空态同款信息层级）
        auto* card = new QPushButton(cardWrap);
        auto* cardLay = new QVBoxLayout(card);
        cardLay->setContentsMargins(16, 12, 16, 12);
        cardLay->setSpacing(2);
        auto* ct = new QLabel(QString::fromUtf8(c.title), card);
        ct->setStyleSheet(QStringLiteral("color:%1;font-size:10.5pt;font-weight:bold;border:none;")
                              .arg(theme::colors::text().name()));
        auto* cd = new QLabel(QString::fromUtf8(c.desc), card);
        cd->setStyleSheet(QStringLiteral("color:%1;font-size:8.5pt;border:none;")
                              .arg(theme::colors::textDim().name()));
        cardLay->addWidget(ct);
        cardLay->addWidget(cd);
        card->setMinimumHeight(76);
        card->setCursor(Qt::PointingHandCursor);
        card->setToolTip(QString::fromUtf8(c.goal));
        card->setFocusPolicy(Qt::StrongFocus); // 可 Tab 到，键盘用户也能选
        card->setStyleSheet(QStringLiteral(
            "QPushButton{background-color:%1;border:1px solid %2;border-radius:10px;text-align:left;}"
            "QPushButton:hover{border-color:%3;background-color:%4;}"
            "QPushButton:focus{border:1px solid %3;}")
                                .arg(theme::colors::panel().name(), theme::colors::line().name(),
                                     theme::colors::accent().name(), theme::colors::btnHover().name()));
        // 子标签不吃鼠标事件，点击统一由卡片按钮接收
        ct->setAttribute(Qt::WA_TransparentForMouseEvents);
        cd->setAttribute(Qt::WA_TransparentForMouseEvents);
        const QString goal = QString::fromUtf8(c.goal);
        connect(card, &QPushButton::clicked, this, [this, goal] { fillInput(goal); });
        grid->addWidget(card, i / 2, i % 2);
        ++i;
    }
    auto* cardRow = new QHBoxLayout();
    cardRow->addStretch(1);
    cardRow->addWidget(cardWrap);
    cardRow->addStretch(1);
    lay->addLayout(cardRow);

    lay->addStretch(4); // 居中：上 3 下 4（视觉重心略偏上更稳）
    m_feedLay->addWidget(m_emptyState);
}

// 「回到底部」悬浮按钮：长对话里往上翻之后，新消息在底部而看不到。
// 做成 m_scroll 的子控件（不进布局），随滚动位置显隐；越界时提示未读新消息。
void SessionView::buildScrollToBottom() {
    m_scrollBottomBtn = new QToolButton(m_scroll->viewport());
    m_scrollBottomBtn->setText(QStringLiteral("↓ 回到底部"));
    m_scrollBottomBtn->setCursor(Qt::PointingHandCursor);
    m_scrollBottomBtn->setToolTip(QStringLiteral("跳到最新消息"));
    m_scrollBottomBtn->setStyleSheet(QStringLiteral(
        "QToolButton{background-color:%1;color:%2;border:1px solid %3;border-radius:14px;"
        "padding:4px 12px;font-size:8.5pt;}"
        "QToolButton:hover{border-color:%4;color:%4;}")
                                         .arg(theme::colors::panel().name(), theme::colors::textDim().name(),
                                              theme::colors::line().name(), theme::colors::accent().name()));
    m_scrollBottomBtn->hide();
    connect(m_scrollBottomBtn, &QToolButton::clicked, this, [this] {
        scrollToEnd();
        m_scrollBottomBtn->hide();
    });
    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { updateScrollButton(); });
    connect(m_scroll->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this] { updateScrollButton(); });
}

void SessionView::updateScrollButton() {
    if (!m_scrollBottomBtn)
        return;
    auto* bar = m_scroll->verticalScrollBar();
    // 距底部 24px 内视为"在底部"（避免因取整误差闪烁）
    const bool atBottom = bar->value() >= bar->maximum() - 24;
    if (atBottom) {
        m_scrollBottomBtn->hide();
        return;
    }
    // 贴到视口右下角上方一点，避免压住最后一条消息的操作按钮
    const int x = m_scroll->viewport()->width() - m_scrollBottomBtn->sizeHint().width() - 18;
    const int y = m_scroll->viewport()->height() - m_scrollBottomBtn->sizeHint().height() - 12;
    m_scrollBottomBtn->move(qMax(0, x), qMax(0, y));
    m_scrollBottomBtn->show();
    m_scrollBottomBtn->raise();
}

void SessionView::fillInput(const QString& text) {
    m_input->setPlainText(text);
    m_input->moveCursor(QTextCursor::End);
    focusInput();
}

bool SessionView::requestSwitchSession(qint64 id) {
    return switchToSession(id);
}

void SessionView::ensureSession(const QString& firstGoal) {
    if (!m_db || m_currentSessionId > 0)
        return;
    QString title = firstGoal.simplified();
    if (title.length() > 24)
        title = title.left(24) + QStringLiteral("…");
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 id = 0;
    if (m_db->executeInsert(QStringLiteral(
            "INSERT INTO sessions(title, created_at, updated_at) VALUES(?,?,?)"), {title, now, now}, &id))
        m_currentSessionId = id;
    emit sessionsChanged();
    emit currentSessionChanged(m_currentSessionId);
}

void SessionView::persistMessage(const char* role, const QString& content) {
    if (!m_db || m_currentSessionId < 0 || content.isEmpty())
        return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    m_db->executeInsert(QStringLiteral(
        "INSERT INTO messages(session_id, role, content, ts) VALUES(?,?,?,?)"),
        {m_currentSessionId, QString::fromLatin1(role), content, now}, nullptr);
    m_db->execute(QStringLiteral("UPDATE sessions SET updated_at=? WHERE id=?"), {now, m_currentSessionId});
    emit sessionsChanged(); // 左栏按 updated_at 排序，需重载
}

bool SessionView::switchToSession(qint64 id) {
    if (!m_db || id == m_currentSessionId)
        return true;
    if (m_loop->isRunning()) {
        QMessageBox::information(this, QStringLiteral("Miderforge"),
                                 QStringLiteral("任务执行中，暂不能切换会话；可先停止或等待完成。"));
        return false;
    }
    m_currentSessionId = id;
    clearFeed();
    m_goalLabel->setText(QStringLiteral("当前任务：—"));
    m_roundLabel->setText(QStringLiteral("第 0/25 轮"));
    m_tokensLabel->setText(QStringLiteral("tokens: 0"));
    m_elapsedLabel->setText(QStringLiteral("已用时 00:00"));
    setStateLabel(QStringLiteral("历史会话"), theme::colors::textDim());
    const auto rows = m_db->query(QStringLiteral(
        "SELECT role, content FROM messages WHERE session_id=? ORDER BY id ASC LIMIT 500"), {id});
    for (const auto& r : rows) {
        const QString role = r.value("role").toString();
        const QString content = r.value("content").toString();
        if (role == QStringLiteral("user")) {
            appendToFeed(new UserBubble(content, m_feedHost));
        } else {
            auto* block = new AssistantBlock(m_feedHost);
            block->setFinalContent(content); // 内部 finishStream + 隐藏复制行（历史不必挂操作按钮）
            m_feedLay->addWidget(block);
        }
    }
    scrollToEnd();
    emit sessionsChanged();
    emit currentSessionChanged(m_currentSessionId);
    return true;
}

void SessionView::clearFeed() {
    // 索引 0 是 stretch、空态卡也在布局里：只清真正的消息/工具卡（用指针判断而非计数，
    // 避免把空态卡一起删掉导致后面再也显示不出来）
    for (int i = m_feedLay->count() - 1; i >= 0; --i) {
        QLayoutItem* item = m_feedLay->itemAt(i);
        if (!item || !item->widget())
            continue;
        QWidget* w = item->widget();
        if (w == m_emptyState)
            continue; // 空态卡保留，只切换可见性
        delete m_feedLay->takeAt(i);
        w->deleteLater();
    }
    m_curAssistant = nullptr;
    m_roundCards.clear();
    m_idCards.clear();
    if (m_emptyState)
        m_emptyState->setVisible(true);
    // 变更审查条随消息流清空（新任务/切换会话都是新的变更集合）
    m_changes.clear();
    m_callPaths.clear();
    m_changesBar->clearChanges();
}

void SessionView::onNewSessionClicked() {
    newSession();
    focusInput();
}

// Composer 底行：附件 / 权限档 / 供应商 / 模型档（对齐 Codex「+  Ask for approval  模型  推理档」一行式）。
// 进度指标（轮次/tokens/已用时/状态）不在这里——已上移到中栏顶部条，避免「任务状态」与
// 「全局设置」混在输入框旁，也避免与底部状态栏重复。
QWidget* SessionView::buildComposerMetaRow(QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    // 附件：Codex 的 "+"。用 ASCII '+' 并显式给字号/前景色——
    // 全角 '＋' 在深色主题下几乎不可见（曾被误判为主题问题）
    auto* attachBtn = new QPushButton(QStringLiteral("+"), row);
    attachBtn->setToolTip(QStringLiteral("附加上下文文件（路径随目标发给 Agent，可用 read_file 查看）"));
    attachBtn->setFixedSize(28, 28);
    attachBtn->setCursor(Qt::PointingHandCursor);
    attachBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background-color:%1;color:%2;border:1px solid %3;border-radius:6px;"
        "font-size:14pt;font-weight:bold;padding:0;}"
        "QPushButton:hover{border-color:%4;color:%4;}")
                                 .arg(theme::colors::window().name(), theme::colors::text().name(),
                                      theme::colors::line().name(), theme::colors::accent().name()));
    connect(attachBtn, &QPushButton::clicked, this, &SessionView::onPickAttachment);
    lay->addWidget(attachBtn);

    m_permCombo = new QComboBox(row);
    m_permCombo->addItems({QStringLiteral("Suggest"), QStringLiteral("Auto Edit"), QStringLiteral("Full Access")});
    m_permCombo->setToolTip(QStringLiteral("权限三档（Codex 式）：Suggest 默认 / Auto Edit 工作区内自动写 / Full Access 全自动"));
    m_permCombo->setFixedHeight(28);
    connect(m_permCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        AppContext::instance().permissionMode = static_cast<PermissionMode>(idx);
        emit permissionModeChanged(idx);
    });
    lay->addWidget(m_permCombo);

    m_providerCombo = new QComboBox(row);
    m_providerCombo->setToolTip(QStringLiteral("当前供应商（切换后写盘，下次派发即生效）"));
    m_providerCombo->setFixedHeight(28);
    connect(m_providerCombo, &QComboBox::currentIndexChanged, this, &SessionView::onProviderChanged);
    lay->addWidget(m_providerCombo);

    m_modelLabel = new QLabel(row);
    m_modelLabel->setToolTip(QStringLiteral("本轮实际使用的模型（由三档路由按任务语义自动选择）"));
    m_modelLabel->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;").arg(theme::colors::textDim().name()));
    lay->addWidget(m_modelLabel);

    lay->addStretch(1);
    return row;
}

QWidget* SessionView::buildInputArea() {
    auto* frame = new QFrame(this);
    // 输入框是每轮交互的起点：给它更高的圆角与内边距，视觉上成为中栏的「重心」
    frame->setStyleSheet(QStringLiteral(
        "QFrame{background-color:%1;border:1px solid %2;border-radius:12px;}")
                            .arg(theme::colors::panel().name(), theme::colors::line().name()));
    auto* frameLay = new QVBoxLayout(frame);
    frameLay->setContentsMargins(12, 12, 12, 12);
    frameLay->setSpacing(8);

    // 附件上下文 chips 行（无附件时隐藏）
    m_chipsHost = new QWidget(frame);
    m_chipsLay = new QHBoxLayout(m_chipsHost);
    m_chipsLay->setContentsMargins(0, 0, 0, 0);
    m_chipsLay->setSpacing(6);
    m_chipsLay->addStretch(1);
    m_chipsHost->setVisible(false);
    frameLay->addWidget(m_chipsHost);

    // 输入框占满整宽（Codex 式），控件全部下沉到底行
    m_input = new QPlainTextEdit(frame);
    m_input->setPlaceholderText(QStringLiteral("输入任务目标，Enter 发送、Shift+Enter 换行（任务执行中发送将自动入队）"));
    m_input->setFixedHeight(72);
    // 聚焦时用品牌强调色描边：把「正在锻造」的反馈做成品牌色，而不是默认蓝框
    m_input->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background-color:%1;color:%2;border:1px solid %3;border-radius:8px;"
        "padding:8px;font-size:10pt;}"
        "QPlainTextEdit:focus{border:1px solid %4;}")
                               .arg(theme::colors::window().name(), theme::colors::text().name(),
                                    theme::colors::line().name(), theme::colors::accent().name()));
    m_input->installEventFilter(this);
    frameLay->addWidget(m_input);

    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(6);
    footer->addWidget(buildComposerMetaRow(frame), 1);

    m_sendBtn = new QPushButton(QStringLiteral("发送"), frame);
    m_sendBtn->setObjectName(QStringLiteral("primaryBtn")); // 全局 QSS 强调色主按钮
    m_sendBtn->setFixedHeight(28);
    connect(m_sendBtn, &QPushButton::clicked, this, &SessionView::onSend);
    m_stopBtn = new QPushButton(QStringLiteral("停止"), frame);
    m_stopBtn->setFixedHeight(28);
    m_stopBtn->setEnabled(false);
    connect(m_stopBtn, &QPushButton::clicked, this, &SessionView::onStop);
    m_pauseBtn = new QPushButton(QStringLiteral("暂停"), frame);
    m_pauseBtn->setFixedHeight(28);
    m_pauseBtn->setEnabled(false);
    connect(m_pauseBtn, &QPushButton::clicked, this, &SessionView::onPauseToggled);
    // M5 P2：挂起/继续状态驱动按钮文案
    connect(m_loop, &AgentLoop::taskPaused, this, [this] {
        m_pauseBtn->setText(QStringLiteral("继续"));
        setStateLabel(QStringLiteral("已暂停"), theme::colors::warn());
    });
    connect(m_loop, &AgentLoop::taskResumed, this, [this] {
        m_pauseBtn->setText(QStringLiteral("暂停"));
    });
    footer->addWidget(m_sendBtn);
    footer->addWidget(m_stopBtn);
    footer->addWidget(m_pauseBtn);
    frameLay->addLayout(footer);

    refreshProviderCombo(); // 初始填充（ProviderManager 已就绪）
    return frame;
}

// 输入区供应商下拉：切换即写盘并同步标签（原 MainWindow::onProviderComboChanged 迁入）
void SessionView::onProviderChanged(int index) {
    if (index < 0 || !m_pm)
        return;
    const QString name = m_providerCombo->itemData(index).toString();
    if (name.isEmpty())
        return;
    m_pm->setActive(name);
    refreshProviderCombo();
}

void SessionView::refreshProviderCombo() {
    if (!m_providerCombo || !m_pm)
        return;
    QSignalBlocker blocker(m_providerCombo);
    m_providerCombo->clear();
    const ProviderConfig* active = m_pm->activeProvider();
    for (const auto& cfg : m_pm->all()) {
        // QComboBox 不渲染富文本：用纯文本圆点（●已配置 / ○未配置）
        const QChar dot = cfg.configured ? QChar(0x25CF) : QChar(0x25CB);
        m_providerCombo->addItem(QStringLiteral("%1 %2").arg(dot, cfg.name), cfg.name);
    }
    if (active) {
        const int idx = m_providerCombo->findData(active->name);
        if (idx >= 0)
            m_providerCombo->setCurrentIndex(idx);
    }
    // 模型档标签：缺 Key 时明确提示，不让用户对着空标签猜
    if (m_modelLabel) {
        if (active && active->configured)
            m_modelLabel->setText(m_pm->modelForTier(*active, QStringLiteral("main")));
        else
            m_modelLabel->setText(QStringLiteral("未配置 Key"));
    }
}

bool SessionView::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == m_input && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            if (ke->modifiers() & Qt::ShiftModifier)
                return QWidget::eventFilter(obj, ev); // Shift+Enter 换行，交回默认处理
            onSend(); // Enter / Ctrl+Enter 发送（空文本由 onSend 自行忽略）
            return true;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

ToolCallCard* SessionView::makeCard(const QString& callId, const QString& toolName) {
    auto* card = new ToolCallCard(callId, toolName, m_feedHost);
    // 权限三按钮 → AgentLoop 恢复执行（0=允许一次 1=总是允许 2=拒绝）
    connect(card, &ToolCallCard::permissionDecided, m_loop, &AgentLoop::resumePermission);
    // 手动重跑：经 AgentLoop 重新过权限门后执行，结果回填同一张卡并给浮层回执
    connect(card, &ToolCallCard::rerunRequested, this,
            [this, card](const QString& name, const QString& args) {
                bool failed = false;
                const QString out = m_loop->rerunTool(name, args, &failed);
                card->setResult(out, 0);
                if (failed) {
                    card->setFailed();
                    // failed 既可能是权限门拒绝、也可能是执行本身失败，文案不替用户下结论
                    Toast::post(window(), QStringLiteral("%1 重跑未成功").arg(name),
                                Toast::Level::Warn, 2200);
                } else {
                    card->setSucceeded();
                    Toast::post(window(), QStringLiteral("已重跑 %1").arg(name),
                                Toast::Level::Success, 1800);
                }
            });
    return card;
}

void SessionView::appendToFeed(QWidget* w) {
    // 有真实内容了：空态引导卡立即让位。
    // ⚠️ 不能用 isVisible() 做守卫：父窗口尚未 show() 时它的有效可见性恒为 false，
    // 于是这里不执行隐藏，空态卡留在布局里——它的 addStretch(3)/(4) 会把所有消息
    // 挤到下方、同时空态主视觉与消息重叠（启动恢复会话时实测到）。
    // setVisible(false) 在父隐藏时同样安全，直接调用即可。
    if (m_emptyState)
        m_emptyState->setVisible(false);
    // stretch 在末尾，插到它之前 → 消息按时间顺序向下堆叠
    m_feedLay->insertWidget(m_feedLay->count() - 1, w);
    scrollToEnd();
}

void SessionView::scrollToEnd() {
    auto* bar = m_scroll->verticalScrollBar();
    bar->setValue(bar->maximum());
    if (m_scrollBottomBtn)
        m_scrollBottomBtn->hide(); // 已到底，无需回底按钮
}

// 唯一的状态视觉入口：文案 + 颜色 + 顶部条形态三处同步，
// 避免"状态文字变了但整体外观没变"这种反馈缺失（运行中还要给顶部条上品牌色描边）
void SessionView::setStateLabel(const QString& text, const QColor& color) {
    m_stateLabel->setText(text);
    m_stateLabel->setStyleSheet(
        QStringLiteral("border:none;font-weight:bold;color:%1;").arg(color.name()));
    const bool running = text != QStringLiteral("空闲") && text != QStringLiteral("已完成")
                         && text != QStringLiteral("历史会话") && text != QStringLiteral("失败")
                         && text != QStringLiteral("已熔断");
    if (m_topStrip) {
        m_topStrip->setStyleSheet(
            running ? QStringLiteral("QFrame{background-color:%1;border:1px solid %2;border-radius:8px;}"
                                     "padding:4px;")
                          .arg(theme::colors::panel().name(), theme::colors::brand().name())
                    : QStringLiteral("QFrame{background:transparent;border:none;}"));
    }
}

void SessionView::setPermissionMode(int mode) {
    if (mode < 0 || mode > 2 || mode == m_permCombo->currentIndex())
        return;
    QSignalBlocker blocker(m_permCombo); // 防回环
    m_permCombo->setCurrentIndex(mode);
}

void SessionView::focusInput() {
    m_input->setFocus();
}

void SessionView::onSend() {
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty())
        return;
    // 附件上下文：路径清单随目标发给 Agent（内容由 Agent 自行 read_file）
    QString goal = text;
    if (!m_attachedFiles.isEmpty()) {
        goal = QStringLiteral("【上下文文件】\n");
        for (const QString& f : m_attachedFiles)
            goal += QStringLiteral("- %1\n").arg(f);
        goal += QStringLiteral("\n【任务目标】\n%1").arg(text);
        m_attachedFiles.clear();
        // 清空 chips：deleteLater 只投递 DeferredDelete 事件、不会同步从子对象表移除，
        // 直接 while(findChild()) { deleteLater(); } 会在同一个指针上永久空转把 GUI 卡死；
        // 必须先一次性取全再逐个删除（hide 已在下方统一处理）
        const QList<QPushButton*> chips = m_chipsHost->findChildren<QPushButton*>();
        for (auto* chip : chips)
            chip->deleteLater();
        m_chipsHost->setVisible(false);
    }
    m_input->clear();
    if (m_loop->isRunning()) {
        // 任务执行中：入队不打断（规格 4.3；M2 迁入数据库任务队列）
        m_pendingQueue << goal;
        emit queueCountChanged(m_pendingQueue.size());
        // 回执改成浮层提示：原先是插一条静态 label 进消息流，会污染对话记录且不会消失
        Toast::post(window(), QStringLiteral("任务执行中，新目标已入队（队列 %1 条）")
                                 .arg(m_pendingQueue.size()),
                    Toast::Level::Info);
        return;
    }
    // 注意：必须传 goal 而非 text——goal 才是带【上下文文件】清单的完整目标，
    // 传 text 会让附件路径静默丢失（空闲发送是附件的常态用法）
    startGoal(goal);
}

void SessionView::startGoal(const QString& goal) {
    ensureSession(goal); // 首条目标创建会话记录，后续消息归属同一会话
    persistMessage("user", goal);
    appendToFeed(new UserBubble(goal, m_feedHost));
    m_curAssistant = nullptr;
    m_roundCards.clear();
    m_idCards.clear();
    m_taskClock.start();
    m_elapsedTimer.start(1000);
    m_elapsedLabel->setText(QStringLiteral("已用时 00:00"));
    m_sendBtn->setEnabled(false);
    m_stopBtn->setEnabled(true);
    m_pauseBtn->setEnabled(true);
    m_pauseBtn->setText(QStringLiteral("暂停"));
    setStateLabel(QStringLiteral("规划中"), theme::colors::accent());
    m_loop->start(goal);
}

void SessionView::onStop() {
    m_loop->cancel();
}

void SessionView::onPauseToggled() {
    if (!m_loop)
        return;
    if (m_loop->isPaused())
        m_loop->resume();
    else
        m_loop->pause();
}

void SessionView::onTaskStarted(const QString& goal) {
    QString shortGoal = goal;
    if (shortGoal.length() > 40)
        shortGoal = shortGoal.left(40) + QStringLiteral("…");
    m_goalLabel->setText(QStringLiteral("当前任务：%1").arg(shortGoal));
}

void SessionView::onStateChanged(const QString& stateText) {
    // 状态标签颜色映射（规格 4.3：规划中/执行中/等待确认/已完成/已熔断）
    if (stateText == QStringLiteral("等待确认"))
        setStateLabel(stateText, theme::colors::warn());
    else if (stateText == QStringLiteral("已完成"))
        setStateLabel(stateText, theme::colors::success());
    else if (stateText == QStringLiteral("已熔断") || stateText == QStringLiteral("失败"))
        setStateLabel(stateText, theme::colors::error());
    else if (stateText == QStringLiteral("空闲"))
        setStateLabel(stateText, theme::colors::textDim());
    else
        setStateLabel(stateText, theme::colors::accent());
}

void SessionView::onToolAwaitingConfirm(const QString& callId, const QString& toolName,
                                        const QString& riskNote, const QString& target) {
    ToolCallCard* card = m_idCards.value(callId, nullptr);
    if (!card) {
        card = makeCard(callId, toolName);
        m_idCards.insert(callId, card);
        appendToFeed(card);
    }
    card->setAwaitingConfirm(); // 展开卡片露出三按钮（允许一次/总是允许/拒绝）
    // 决策: v1 在消息流中追加一条风险说明行（规格 4.10 的弹窗模式在设置页后续补充开关）
    auto* note = new QLabel(m_feedHost);
    note->setWordWrap(true);
    note->setTextFormat(Qt::RichText);
    note->setText(QStringLiteral("<span style='color:%1'>⏸ <b>待确认</b>：%2<br/>目标：<code>%3</code><br/>"
                                 "在上方工具卡片中选择〔允许一次〕〔本会话总是允许〕或〔拒绝〕。</span>")
                      .arg(theme::colors::warn().name(), riskNote, target.toHtmlEscaped()));
    appendToFeed(note);
    setStateLabel(QStringLiteral("等待确认"), theme::colors::warn());
    scrollToEnd();
}

void SessionView::onRoundChanged(int round, int maxRounds) {
    m_roundLabel->setText(QStringLiteral("第 %1/%2 轮").arg(round).arg(maxRounds));
    m_roundCards.clear(); // 新一轮的 index 重新从 0 编号
    setStateLabel(QStringLiteral("执行中"), theme::colors::accent());
    if (m_curAssistant) {
        m_curAssistant->finishStream();
        m_curAssistant = nullptr; // 每轮一个新的助手块
    }
}

void SessionView::onToolCallDelta(int index, const QString& id, const QString& name, const QString& args) {
    ToolCallCard* card = m_roundCards.value(index, nullptr);
    if (!card) {
        card = makeCard(id.isEmpty() ? QStringLiteral("idx_%1").arg(index) : id, name);
        m_roundCards.insert(index, card);
        appendToFeed(card);
    }
    card->updateArgsPreview(args); // 边流边显示参数碎片拼装
    if (!id.isEmpty())
        m_idCards.insert(id, card);
}

void SessionView::onToolCallStarted(const QString& callId, const QString& name, const QString& args) {
    // 变更树：记录 write_file 目标路径（完成时用 diff 回填右栏）
    if (name == QLatin1String("write_file")) {
        const QJsonObject obj = QJsonDocument::fromJson(args.toUtf8()).object();
        const QString path = obj.value("path").toString();
        if (!path.isEmpty())
            m_callPaths.insert(callId, path);
    }
    ToolCallCard* card = m_idCards.value(callId, nullptr);
    if (!card) {
        // 个别响应无 delta 碎片：直接按 started 建卡
        card = makeCard(callId, name);
        m_idCards.insert(callId, card);
        appendToFeed(card);
    }
    card->setArgs(args);
    card->setArgsRaw(args); // 供「重跑」原样重发
    card->setRunning();
    setStateLabel(QStringLiteral("执行中"), theme::colors::accent());
    scrollToEnd();
}

void SessionView::onToolCallFinished(const QString& callId, bool ok, const QString& resultText, qint64 ms) {
    // 变更归档：write_file 结果 envelope 里的 diff 统计/正文回填顶部变更审查条
    if (m_callPaths.contains(callId)) {
        const QString path = m_callPaths.take(callId);
        const QJsonObject obj = QJsonDocument::fromJson(resultText.toUtf8()).object();
        const QJsonObject diff = obj.value("diff").toObject();
        if (!diff.isEmpty()) {
            ChangeInfo info;
            info.added = diff.value("added").toInt();
            info.removed = diff.value("removed").toInt();
            info.diff = diff.value("diff").toString();
            m_changes.insert(path, info);
            refreshChangesBar();
        }
    }
    if (ToolCallCard* card = m_idCards.value(callId, nullptr)) {
        card->setResult(resultText, ms);
        if (ok)
            card->setSucceeded();
        else
            card->setFailed();
    }
    scrollToEnd();
}

void SessionView::onStreamRetrying(const QString& reason) {
    if (m_curAssistant)
        m_curAssistant->resetStream();
    auto* note = new QLabel(QStringLiteral("⚠ 流中断（%1），指数退避重试中…").arg(reason), m_feedHost);
    note->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;").arg(theme::colors::warn().name()));
    appendToFeed(note);
}

void SessionView::onLoopFinished(bool ok, const QString& summary) {
    if (m_curAssistant) {
        if (!summary.isEmpty() && m_curAssistant)
            m_curAssistant->setFinalContent(summary);
        m_curAssistant->finishStream();
        m_curAssistant = nullptr;
    }
    persistMessage("assistant",
                   summary.isEmpty()
                       ? (ok ? QStringLiteral("（已完成）") : QStringLiteral("（已熔断）"))
                       : summary);
    m_elapsedTimer.stop();
    m_sendBtn->setEnabled(true);
    m_stopBtn->setEnabled(false);
    m_pauseBtn->setEnabled(false);
    m_pauseBtn->setText(QStringLiteral("暂停"));
    setStateLabel(ok ? QStringLiteral("已完成") : QStringLiteral("已熔断"),
                  ok ? theme::colors::success() : theme::colors::error());
    popQueueIfIdle();
}

void SessionView::onLoopFailed(const QString& error) {
    m_elapsedTimer.stop();
    m_sendBtn->setEnabled(true);
    m_stopBtn->setEnabled(false);
    m_pauseBtn->setEnabled(false);
    m_pauseBtn->setText(QStringLiteral("暂停"));
    setStateLabel(QStringLiteral("失败"), theme::colors::error());
    auto* note = new QLabel(QStringLiteral("✖ %1").arg(error), m_feedHost);
    note->setStyleSheet(QStringLiteral("color:%1;").arg(theme::colors::error().name()));
    note->setWordWrap(true);
    appendToFeed(note);
    persistMessage("assistant", QStringLiteral("✖ 失败：%1").arg(error));
    m_curAssistant = nullptr;
    popQueueIfIdle();
}

void SessionView::popQueueIfIdle() {
    if (m_pendingQueue.isEmpty() || m_loop->isRunning())
        return;
    const QString next = m_pendingQueue.takeFirst();
    emit queueCountChanged(m_pendingQueue.size());
    QTimer::singleShot(500, this, [this, next] { startGoal(next); });
}

void SessionView::newSession() {
    // ⚠️ 顺序关键：必须先把会话身份作废，再 cancel()。
    // cancel() 在"等待授权 / 已暂停"两条路径上是**同步**发 loopFinished 的，
    // 其处理器会 persistMessage("assistant", "已手动停止")——如果此时 m_currentSessionId
    // 还是旧值，就往一个"刚刚已被删除的会话 id"里插了一条消息（孤儿行，
    // session_search 永远搜得到、UI 永远够不着、也不会再被删）。
    m_currentSessionId = -1;
    if (m_loop->isRunning())
        m_loop->cancel();
    m_pendingQueue.clear();
    emit queueCountChanged(0);
    clearFeed(); // 内部会把空态卡恢复可见
    m_goalLabel->setText(QStringLiteral("当前任务：—"));
    m_roundLabel->setText(QStringLiteral("第 0/25 轮"));
    m_tokensLabel->setText(QStringLiteral("tokens: 0"));
    m_elapsedLabel->setText(QStringLiteral("已用时 00:00"));
    m_elapsedTimer.stop();
    setStateLabel(QStringLiteral("空闲"), theme::colors::textDim());
    m_input->clear();
    emit sessionsChanged();
    emit currentSessionChanged(-1); // 左栏取消高亮
    focusInput();
}

} // namespace miderforge
