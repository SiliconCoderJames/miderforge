// 会话视图实现
#include "app/SessionView.h"
#include "app/Theme.h"
#include "core/AppContext.h"
#include <QBoxLayout>
#include <QComboBox>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>

namespace miderforge {

SessionView::SessionView(AgentLoop* loop, QWidget* parent)
    : QWidget(parent), m_loop(loop) {
    auto* rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(8, 6, 8, 8);
    rootLay->setSpacing(6);

    rootLay->addWidget(buildStatusStrip());

    // 中部消息流：QScrollArea 包 QVBoxLayout，新消息自动滚动到底（自底向上滚动）
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setStyleSheet(QStringLiteral("QScrollArea{background:%1;}").arg(theme::colors::window.name()));
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

    setStateLabel(QStringLiteral("空闲"), theme::colors::textDim);
}

QWidget* SessionView::buildStatusStrip() {
    auto* strip = new QFrame(this);
    strip->setFixedHeight(36);
    strip->setStyleSheet(QStringLiteral(
        "QFrame{background-color:%1;border:1px solid #35383d;border-radius:6px;}")
                            .arg(theme::colors::panel.name()));
    auto* lay = new QHBoxLayout(strip);
    lay->setContentsMargins(10, 0, 10, 0);
    lay->setSpacing(14);
    m_goalLabel = new QLabel(QStringLiteral("当前任务：—"), strip);
    m_roundLabel = new QLabel(QStringLiteral("第 0/25 轮"), strip);
    m_tokensLabel = new QLabel(QStringLiteral("tokens: 0"), strip);
    m_elapsedLabel = new QLabel(QStringLiteral("已用时 00:00"), strip);
    m_stateLabel = new QLabel(strip);
    for (auto* l : {m_goalLabel, m_roundLabel, m_tokensLabel, m_elapsedLabel}) {
        l->setStyleSheet(QStringLiteral("border:none;color:%1;").arg(theme::colors::textDim.name()));
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
                            .arg(theme::colors::panel.name()));
    auto* lay = new QHBoxLayout(frame);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(8);

    m_input = new QPlainTextEdit(frame);
    m_input->setPlaceholderText(QStringLiteral("输入任务目标，Ctrl+Enter 发送（任务执行中发送将自动入队）"));
    m_input->setFixedHeight(66); // 3 行高
    m_input->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background-color:%1;color:%2;border:1px solid #3d4045;border-radius:6px;padding:4px;}")
                               .arg(theme::colors::window.name(), theme::colors::text.name()));

    m_permCombo = new QComboBox(frame);
    m_permCombo->addItems({QStringLiteral("Suggest"), QStringLiteral("Auto Edit"), QStringLiteral("Full Access")});
    m_permCombo->setToolTip(QStringLiteral("权限三档（Codex 式）：Suggest 默认 / Auto Edit 工作区内自动写 / Full Access 全自动"));
    connect(m_permCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        AppContext::instance().permissionMode = static_cast<PermissionMode>(idx);
        emit permissionModeChanged(idx);
    });

    m_sendBtn = new QPushButton(QStringLiteral("发送"), frame);
    m_stopBtn = new QPushButton(QStringLiteral("停止"), frame);
    connect(m_sendBtn, &QPushButton::clicked, this, &SessionView::onSend);
    connect(m_stopBtn, &QPushButton::clicked, this, &SessionView::onStop);
    m_stopBtn->setEnabled(false);

    auto* sendShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), m_input);
    connect(sendShortcut, &QShortcut::activated, this, &SessionView::onSend);

    lay->addWidget(m_input, 1);
    auto* rightLay = new QVBoxLayout();
    rightLay->addWidget(m_permCombo);
    rightLay->addWidget(m_sendBtn);
    lay->addLayout(rightLay);
    lay->addWidget(m_stopBtn);
    return frame;
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
    m_input->clear();
    if (m_loop->isRunning()) {
        // 任务执行中：入队不打断（规格 4.3；M2 迁入数据库任务队列）
        m_pendingQueue << text;
        emit queueCountChanged(m_pendingQueue.size());
        auto* note = new QLabel(QStringLiteral("⏳ 任务执行中，新目标已入队"), m_feedHost);
        note->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;").arg(theme::colors::warn.name()));
        appendToFeed(note);
        return;
    }
    startGoal(text);
}

void SessionView::startGoal(const QString& goal) {
    appendToFeed(new UserBubble(goal, m_feedHost));
    m_curAssistant = nullptr;
    m_roundCards.clear();
    m_idCards.clear();
    m_taskClock.start();
    m_elapsedTimer.start(1000);
    m_elapsedLabel->setText(QStringLiteral("已用时 00:00"));
    m_sendBtn->setEnabled(false);
    m_stopBtn->setEnabled(true);
    setStateLabel(QStringLiteral("规划中"), theme::colors::accent);
    m_loop->start(goal);
}

void SessionView::onStop() {
    m_loop->cancel();
}

void SessionView::onTaskStarted(const QString& goal) {
    QString shortGoal = goal;
    if (shortGoal.length() > 40)
        shortGoal = shortGoal.left(40) + QStringLiteral("…");
    m_goalLabel->setText(QStringLiteral("当前任务：%1").arg(shortGoal));
}

void SessionView::onRoundChanged(int round, int maxRounds) {
    m_roundLabel->setText(QStringLiteral("第 %1/%2 轮").arg(round).arg(maxRounds));
    m_roundCards.clear(); // 新一轮的 index 重新从 0 编号
    setStateLabel(QStringLiteral("执行中"), theme::colors::accent);
    if (m_curAssistant) {
        m_curAssistant->finishStream();
        m_curAssistant = nullptr; // 每轮一个新的助手块
    }
}

void SessionView::onToolCallDelta(int index, const QString& id, const QString& name, const QString& args) {
    ToolCallCard* card = m_roundCards.value(index, nullptr);
    if (!card) {
        card = new ToolCallCard(id.isEmpty() ? QStringLiteral("idx_%1").arg(index) : id, name, m_feedHost);
        m_roundCards.insert(index, card);
        appendToFeed(card);
    }
    card->updateArgsPreview(args); // 边流边显示参数碎片拼装
    if (!id.isEmpty())
        m_idCards.insert(id, card);
}

void SessionView::onToolCallStarted(const QString& callId, const QString& name, const QString& args) {
    ToolCallCard* card = m_idCards.value(callId, nullptr);
    if (!card) {
        // 个别响应无 delta 碎片：直接按 started 建卡
        card = new ToolCallCard(callId, name, m_feedHost);
        m_idCards.insert(callId, card);
        appendToFeed(card);
    }
    card->setArgs(args);
    card->setRunning();
    setStateLabel(QStringLiteral("执行中"), theme::colors::accent);
    scrollToEnd();
}

void SessionView::onToolCallFinished(const QString& callId, bool ok, const QString& resultText, qint64 ms) {
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
    note->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;").arg(theme::colors::warn.name()));
    appendToFeed(note);
}

void SessionView::onLoopFinished(bool ok, const QString& summary) {
    if (m_curAssistant) {
        if (!summary.isEmpty() && m_curAssistant)
            m_curAssistant->setFinalContent(summary);
        m_curAssistant->finishStream();
        m_curAssistant = nullptr;
    }
    m_elapsedTimer.stop();
    m_sendBtn->setEnabled(true);
    m_stopBtn->setEnabled(false);
    setStateLabel(ok ? QStringLiteral("已完成") : QStringLiteral("已熔断"),
                  ok ? theme::colors::success : theme::colors::error);
    popQueueIfIdle();
}

void SessionView::onLoopFailed(const QString& error) {
    m_elapsedTimer.stop();
    m_sendBtn->setEnabled(true);
    m_stopBtn->setEnabled(false);
    setStateLabel(QStringLiteral("失败"), theme::colors::error);
    auto* note = new QLabel(QStringLiteral("✖ %1").arg(error), m_feedHost);
    note->setStyleSheet(QStringLiteral("color:%1;").arg(theme::colors::error.name()));
    note->setWordWrap(true);
    appendToFeed(note);
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
    // 清空消息流
    while (m_feedLay->count() > 1) { // 索引 0 是 stretch
        QLayoutItem* item = m_feedLay->takeAt(m_feedLay->count() - 1);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    m_curAssistant = nullptr;
    m_roundCards.clear();
    m_idCards.clear();
    m_goalLabel->setText(QStringLiteral("当前任务：—"));
    m_roundLabel->setText(QStringLiteral("第 0/25 轮"));
    m_tokensLabel->setText(QStringLiteral("tokens: 0"));
    m_elapsedLabel->setText(QStringLiteral("已用时 00:00"));
    m_elapsedTimer.stop();
    setStateLabel(QStringLiteral("空闲"), theme::colors::textDim);
    m_input->clear();
    focusInput();
}

} // namespace miderforge
