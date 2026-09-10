// 会话视图实现
#include "app/SessionView.h"
#include "app/Theme.h"
#include "core/AppContext.h"
#include "db/Database.h"
#include <QBoxLayout>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>

namespace miderforge {

SessionView::SessionView(Database* db, AgentLoop* loop, QWidget* parent)
    : QWidget(parent), m_loop(loop), m_db(db) {
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(8, 6, 8, 8);
    outer->setSpacing(8);
    outer->addWidget(buildSessionPanel());

    auto* content = new QWidget(this);
    auto* contentLay = new QHBoxLayout(content);
    contentLay->setContentsMargins(0, 0, 0, 0);
    contentLay->setSpacing(8);
    auto* mainCol = new QWidget(content);
    auto* rootLay = new QVBoxLayout(mainCol);
    rootLay->setContentsMargins(0, 0, 0, 0);
    rootLay->setSpacing(6);
    contentLay->addWidget(mainCol, 1);
    contentLay->addWidget(buildChangesPanel());
    outer->addWidget(content, 1);

    rootLay->addWidget(buildStatusStrip());

    // 中部消息流：QScrollArea 包 QVBoxLayout，新消息自动滚动到底（自底向上滚动）
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setStyleSheet(QStringLiteral("QScrollArea{background:%1;}").arg(theme::colors::window().name()));
    m_feedHost = new QWidget(m_scroll);
    m_feedLay = new QVBoxLayout(m_feedHost);
    m_feedLay->setContentsMargins(4, 4, 4, 4);
    m_feedLay->setSpacing(8);
    m_feedLay->addStretch(1); // 内容少时贴底，呈聊天式布局
    m_scroll->setWidget(m_feedHost);
    rootLay->addWidget(m_scroll, 1);

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

    loadSessions();
}

QWidget* SessionView::buildChangesPanel() {
    auto* panel = new QWidget(this);
    panel->setFixedWidth(180);
    panel->setStyleSheet(QStringLiteral(
        "QWidget{background-color:%1;border:1px solid #35383d;border-radius:6px;}")
                            .arg(theme::colors::panel().name()));
    auto* lay = new QVBoxLayout(panel);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);
    m_changesTitle = new QLabel(QStringLiteral("🔧 变更文件（0）"), panel);
    m_changesTitle->setStyleSheet(QStringLiteral("border:none;font-weight:bold;color:%1;font-size:9pt;")
                                      .arg(theme::colors::textDim().name()));
    lay->addWidget(m_changesTitle);
    m_changesList = new QListWidget(panel);
    m_changesList->setStyleSheet(QStringLiteral(
        "QListWidget{background:transparent;border:none;font-size:9pt;outline:none;}"
        "QListWidget::item{height:30px;border-radius:6px;padding-left:4px;color:%2;margin:1px 0;}"
        "QListWidget::item:hover{background-color:%1;}")
                                     .arg(theme::colors::window().name(), theme::colors::textDim().name()));
    m_changesList->setToolTip(QStringLiteral("本次任务的文件变更（点击查看 diff）"));
    connect(m_changesList, &QListWidget::itemClicked, this, &SessionView::onChangedFileClicked);
    lay->addWidget(m_changesList, 1);
    return panel;
}

void SessionView::onChangedFileClicked(QListWidgetItem* item) {
    const QString path = item->data(Qt::UserRole).toString();
    const auto it = m_changes.constFind(path);
    if (it == m_changes.constEnd())
        return;
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("变更审查 · %1（+%2/−%3）").arg(path).arg(it->added).arg(it->removed));
    dlg.resize(860, 620);
    auto* lay = new QVBoxLayout(&dlg);
    auto* view = new QPlainTextEdit(&dlg);
    view->setReadOnly(true);
    view->setFont(theme::monoFont());
    view->setPlainText(it->diff);
    lay->addWidget(view);
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

QWidget* SessionView::buildSessionPanel() {
    auto* panel = new QWidget(this);
    panel->setFixedWidth(190);
    panel->setStyleSheet(QStringLiteral(
        "QWidget{background-color:%1;border:1px solid #35383d;border-radius:6px;}")
                            .arg(theme::colors::panel().name()));
    auto* lay = new QVBoxLayout(panel);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);

    auto* newBtn = new QPushButton(QStringLiteral("＋ 新会话"), panel);
    newBtn->setObjectName(QStringLiteral("primaryBtn"));
    newBtn->setFixedHeight(30);
    connect(newBtn, &QPushButton::clicked, this, &SessionView::onNewSessionClicked);
    lay->addWidget(newBtn);

    m_sessionList = new QListWidget(panel);
    m_sessionList->setStyleSheet(QStringLiteral(
        "QListWidget{background:transparent;border:none;font-size:9pt;outline:none;}"
        "QListWidget::item{height:34px;border-radius:6px;padding-left:6px;color:%2;margin:1px 0;}"
        "QListWidget::item:hover{background-color:%1;}"
        "QListWidget::item:selected{background-color:%3;color:white;}")
                                     .arg(theme::colors::window().name(), theme::colors::textDim().name(),
                                          theme::colors::accent().name()));
    m_sessionList->setToolTip(QStringLiteral("历史会话（点击恢复消息流）"));
    connect(m_sessionList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* cur, QListWidgetItem*) {
                if (cur)
                    onSessionSelected();
            });
    lay->addWidget(m_sessionList, 1);
    return panel;
}

void SessionView::loadSessions() {
    refreshSessionList();
}

void SessionView::refreshSessionList() {
    if (!m_db)
        return;
    QSignalBlocker blocker(m_sessionList);
    m_sessionList->clear();
    const auto rows = m_db->query(QStringLiteral(
        "SELECT id, title, updated_at FROM sessions ORDER BY updated_at DESC LIMIT 100"), {});
    for (const auto& r : rows) {
        const QString title = r.value("title").toString();
        const QString time = QDateTime::fromSecsSinceEpoch(r.value("updated_at").toLongLong())
                                 .toString(QStringLiteral("MM-dd hh:mm"));
        auto* item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(title, time), m_sessionList);
        item->setToolTip(title);
        item->setData(Qt::UserRole, r.value("id").toLongLong());
        if (r.value("id").toLongLong() == m_currentSessionId)
            m_sessionList->setCurrentItem(item);
    }
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
    refreshSessionList();
}

void SessionView::persistMessage(const char* role, const QString& content) {
    if (!m_db || m_currentSessionId < 0 || content.isEmpty())
        return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    m_db->executeInsert(QStringLiteral(
        "INSERT INTO messages(session_id, role, content, ts) VALUES(?,?,?,?)"),
        {m_currentSessionId, QString::fromLatin1(role), content, now}, nullptr);
    m_db->execute(QStringLiteral("UPDATE sessions SET updated_at=? WHERE id=?"), {now, m_currentSessionId});
    refreshSessionList();
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
            block->setFinalContent(content);
            block->finishStream();
            m_feedLay->addWidget(block);
        }
    }
    scrollToEnd();
    refreshSessionList();
    return true;
}

void SessionView::clearFeed() {
    while (m_feedLay->count() > 1) { // 索引 0 是 stretch
        QLayoutItem* item = m_feedLay->takeAt(m_feedLay->count() - 1);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    m_curAssistant = nullptr;
    // 变更文件右栏随消息流清空（新任务/切换会话都是新的变更集合）
    m_changes.clear();
    m_callPaths.clear();
    m_changesList->clear();
    m_changesTitle->setText(QStringLiteral("🔧 变更文件（0）"));
}

void SessionView::onSessionSelected() {
    QListWidgetItem* cur = m_sessionList->currentItem();
    if (!cur)
        return;
    switchToSession(cur->data(Qt::UserRole).toLongLong());
}

void SessionView::onNewSessionClicked() {
    newSession();
    m_sessionList->setCurrentItem(nullptr);
    focusInput();
}

QWidget* SessionView::buildStatusStrip() {
    auto* strip = new QFrame(this);
    strip->setFixedHeight(36);
    strip->setStyleSheet(QStringLiteral(
        "QFrame{background-color:%1;border:1px solid #35383d;border-radius:6px;}")
                            .arg(theme::colors::panel().name()));
    auto* lay = new QHBoxLayout(strip);
    lay->setContentsMargins(10, 0, 10, 0);
    lay->setSpacing(14);
    m_goalLabel = new QLabel(QStringLiteral("当前任务：—"), strip);
    m_roundLabel = new QLabel(QStringLiteral("第 0/25 轮"), strip);
    m_tokensLabel = new QLabel(QStringLiteral("tokens: 0"), strip);
    m_elapsedLabel = new QLabel(QStringLiteral("已用时 00:00"), strip);
    m_stateLabel = new QLabel(strip);
    for (auto* l : {m_goalLabel, m_roundLabel, m_tokensLabel, m_elapsedLabel}) {
        l->setStyleSheet(QStringLiteral("border:none;color:%1;").arg(theme::colors::textDim().name()));
        lay->addWidget(l);
    }
    m_goalLabel->setMinimumWidth(220);
    lay->addStretch(1);
    lay->addWidget(m_stateLabel);
    m_stateLabel->setStyleSheet(QStringLiteral("border:none;font-weight:bold;"));
    return strip;
}

QWidget* SessionView::buildInputArea() {
    auto* frame = new QFrame(this);
    frame->setStyleSheet(QStringLiteral(
        "QFrame{background-color:%1;border:1px solid #35383d;border-radius:6px;}")
                            .arg(theme::colors::panel().name()));
    auto* frameLay = new QVBoxLayout(frame);
    frameLay->setContentsMargins(8, 8, 8, 8);
    frameLay->setSpacing(6);

    // 附件上下文 chips 行（无附件时隐藏）
    m_chipsHost = new QWidget(frame);
    m_chipsLay = new QHBoxLayout(m_chipsHost);
    m_chipsLay->setContentsMargins(0, 0, 0, 0);
    m_chipsLay->setSpacing(6);
    m_chipsLay->addStretch(1);
    m_chipsHost->setVisible(false);
    frameLay->addWidget(m_chipsHost);

    auto* lay = new QHBoxLayout();
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);
    frameLay->addLayout(lay);

    m_input = new QPlainTextEdit(frame);
    m_input->setPlaceholderText(QStringLiteral("输入任务目标，Enter 发送、Shift+Enter 换行（任务执行中发送将自动入队）"));
    m_input->setFixedHeight(66); // 3 行高
    m_input->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background-color:%1;color:%2;border:1px solid #3d4045;border-radius:6px;padding:4px;}")
                               .arg(theme::colors::window().name(), theme::colors::text().name()));
    m_input->installEventFilter(this);

    m_permCombo = new QComboBox(frame);
    m_permCombo->addItems({QStringLiteral("Suggest"), QStringLiteral("Auto Edit"), QStringLiteral("Full Access")});
    m_permCombo->setToolTip(QStringLiteral("权限三档（Codex 式）：Suggest 默认 / Auto Edit 工作区内自动写 / Full Access 全自动"));
    connect(m_permCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        AppContext::instance().permissionMode = static_cast<PermissionMode>(idx);
        emit permissionModeChanged(idx);
    });

    m_sendBtn = new QPushButton(QStringLiteral("发送"), frame);
    m_sendBtn->setObjectName(QStringLiteral("primaryBtn")); // 全局 QSS 强调色主按钮
    m_sendBtn->setFixedHeight(28);
    m_stopBtn = new QPushButton(QStringLiteral("停止"), frame);
    m_stopBtn->setFixedHeight(28);
    m_stopBtn->setEnabled(false);
    connect(m_sendBtn, &QPushButton::clicked, this, &SessionView::onSend);
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

    lay->addWidget(m_input, 1);
    auto* rightLay = new QVBoxLayout();
    rightLay->setSpacing(6);
    m_permCombo->setFixedHeight(28);
    auto* attachComboRow = new QHBoxLayout();
    attachComboRow->setSpacing(6);
    auto* attachBtn = new QPushButton(QStringLiteral("📎"), frame);
    attachBtn->setToolTip(QStringLiteral("附加上下文文件（路径随目标发给 Agent，可用 read_file 查看）"));
    attachBtn->setFixedSize(28, 28);
    connect(attachBtn, &QPushButton::clicked, this, &SessionView::onPickAttachment);
    attachComboRow->addWidget(m_permCombo);
    attachComboRow->addWidget(attachBtn);
    rightLay->addLayout(attachComboRow);
    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(6);
    btnRow->addWidget(m_sendBtn, 1);
    btnRow->addWidget(m_stopBtn, 1);
    btnRow->addWidget(m_pauseBtn, 1);
    rightLay->addLayout(btnRow);
    lay->addLayout(rightLay);
    return frame;
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
    return card;
}

void SessionView::appendToFeed(QWidget* w) {
    // stretch 在索引 0，插入到 stretch 之后保持贴底布局
    m_feedLay->insertWidget(m_feedLay->count() - 1, w);
    scrollToEnd();
}

void SessionView::scrollToEnd() {
    auto* bar = m_scroll->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void SessionView::setStateLabel(const QString& text, const QColor& color) {
    m_stateLabel->setText(text);
    m_stateLabel->setStyleSheet(
        QStringLiteral("border:none;font-weight:bold;color:%1;").arg(color.name()));
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
        // 清空 chips
        while (auto* chip = m_chipsHost->findChild<QPushButton*>())
            chip->deleteLater();
        m_chipsHost->setVisible(false);
    }
    m_input->clear();
    if (m_loop->isRunning()) {
        // 任务执行中：入队不打断（规格 4.3；M2 迁入数据库任务队列）
        m_pendingQueue << goal;
        emit queueCountChanged(m_pendingQueue.size());
        auto* note = new QLabel(QStringLiteral("⏳ 任务执行中，新目标已入队"), m_feedHost);
        note->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;").arg(theme::colors::warn().name()));
        appendToFeed(note);
        return;
    }
    startGoal(text);
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
    card->setRunning();
    setStateLabel(QStringLiteral("执行中"), theme::colors::accent());
    scrollToEnd();
}

void SessionView::onToolCallFinished(const QString& callId, bool ok, const QString& resultText, qint64 ms) {
    // 变更树归档：write_file 结果 envelope 里的 diff 统计/正文回填右栏
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
            m_changesList->clear();
            for (auto ch = m_changes.constBegin(); ch != m_changes.constEnd(); ++ch) {
                auto* item = new QListWidgetItem(QStringLiteral("%1  +%2/−%3")
                                                     .arg(QFileInfo(ch.key()).fileName())
                                                     .arg(ch->added)
                                                     .arg(ch->removed),
                                                 m_changesList);
                item->setToolTip(ch.key());
                item->setData(Qt::UserRole, ch.key());
            }
            m_changesTitle->setText(QStringLiteral("🔧 变更文件（%1）").arg(m_changes.size()));
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
    if (m_loop->isRunning())
        m_loop->cancel();
    m_pendingQueue.clear();
    emit queueCountChanged(0);
    clearFeed();
    m_currentSessionId = -1; // 新会话：下一条目标入库时创建新记录
    m_sessionList->setCurrentItem(nullptr);
    m_roundCards.clear();
    m_idCards.clear();
    m_goalLabel->setText(QStringLiteral("当前任务：—"));
    m_roundLabel->setText(QStringLiteral("第 0/25 轮"));
    m_tokensLabel->setText(QStringLiteral("tokens: 0"));
    m_elapsedLabel->setText(QStringLiteral("已用时 00:00"));
    m_elapsedTimer.stop();
    setStateLabel(QStringLiteral("空闲"), theme::colors::textDim());
    m_input->clear();
    focusInput();
}

} // namespace miderforge
