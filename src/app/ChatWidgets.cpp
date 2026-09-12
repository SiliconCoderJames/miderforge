// 聊天控件集实现
#include "app/ChatWidgets.h"
#include "app/Theme.h"
#include <QAbstractAnimation>
#include <QClipboard>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMainWindow>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace miderforge {

// ---------- 简易 markdown ----------
namespace {
QString escapeHtml(const QString& s) {
    return QString(s).toHtmlEscaped();
}

// diff 文本着色（+绿/−红/@@ 强调）：仅当检测到 unified diff 行首记号才逐行包裹
QString diffColorized(const QString& text) {
    const QString esc = escapeHtml(text);
    if (!esc.contains(QLatin1Char('\n')) ||
        !(esc.contains(QStringLiteral("\n+")) || esc.startsWith(QStringLiteral("+"))
          || esc.contains(QStringLiteral("\n-")) || esc.startsWith(QStringLiteral("-"))
          || esc.contains(QStringLiteral("@@"))))
        return esc;
    QStringList out;
    const QStringList lines = esc.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        if (line.startsWith(QStringLiteral("+")))
            out << QStringLiteral("<span style=\"color:%1\">%2</span>")
                       .arg(theme::colors::success().name(), line);
        else if (line.startsWith(QStringLiteral("-")))
            out << QStringLiteral("<span style=\"color:%1\">%2</span>")
                       .arg(theme::colors::error().name(), line);
        else if (line.startsWith(QStringLiteral("@@")))
            out << QStringLiteral("<span style=\"color:%1;font-weight:bold\">%2</span>")
                       .arg(theme::colors::brand().name(), line);
        else
            out << line;
    }
    return out.join(QLatin1Char('\n'));
}

QString inlineMd(QString s) {
    s = escapeHtml(s);
    static const QRegularExpression codeRe(QStringLiteral("`([^`]+)`"));
    s.replace(codeRe, QStringLiteral("<code style=\"background-color:%1;font-family:'Consolas';\">\\1</code>")
                          .arg(theme::colors::codeBg().name()));
    static const QRegularExpression boldRe(QStringLiteral(R"(\*\*([^*]+)\*\*)"));
    s.replace(boldRe, QStringLiteral("<b>\\1</b>"));
    static const QRegularExpression italicRe(QStringLiteral(R"((?<!\*)\*([^*\n]+)\*(?!\*))"));
    s.replace(italicRe, QStringLiteral("<i>\\1</i>"));
    return s;
}
} // namespace

QString mdToHtml(const QString& md) {
    QStringList htmlParts;
    // 按 ``` 围栏切块：奇数块为代码块（等宽灰底原样输出），偶数块走行内规则
    const QStringList blocks = md.split(QStringLiteral("```"));
    for (int i = 0; i < blocks.size(); ++i) {
        const QString& block = blocks.at(i);
        if (i % 2 == 1) {
            QString code = block;
            const int nl = code.indexOf(QLatin1Char('\n'));
            if (nl >= 0 && nl < 24)
                code.remove(0, nl + 1); // 去掉语言标注行
            htmlParts << QStringLiteral(
                "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"6\" "
                "style=\"background-color:%1;margin:4px 0;\"><tr><td>"
                "<pre style=\"font-family:'Consolas';margin:0;white-space:pre-wrap;\">%2</pre>"
                "</td></tr></table>")
                          .arg(theme::colors::codeBg().name(), escapeHtml(code));
        } else {
            const QStringList lines = block.split(QLatin1Char('\n'));
            QStringList out;
            for (const QString& line : lines) {
                if (line.startsWith(QStringLiteral("#### ")))
                    out << QStringLiteral("<b>%1</b>").arg(inlineMd(line.mid(5)));
                else if (line.startsWith(QStringLiteral("### ")))
                    out << QStringLiteral("<b>%1</b>").arg(inlineMd(line.mid(4)));
                else if (line.startsWith(QStringLiteral("## ")))
                    out << QStringLiteral("<b><big>%1</big></b>").arg(inlineMd(line.mid(3)));
                else if (line.startsWith(QStringLiteral("# ")))
                    out << QStringLiteral("<b><big>%1</big></b>").arg(inlineMd(line.mid(2)));
                else if (line.startsWith(QStringLiteral("- ")) || line.startsWith(QStringLiteral("* ")))
                    out << QStringLiteral("• %1").arg(inlineMd(line.mid(2)));
                else
                    out << inlineMd(line);
            }
            htmlParts << out.join(QLatin1String("<br/>"));
        }
    }
    return htmlParts.join(QString());
}

// ---------- UserBubble ----------
UserBubble::UserBubble(const QString& text, QWidget* parent) : QWidget(parent) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 2, 0, 2);
    lay->addStretch(1);

    // 气泡底色由**当前色板**派生（原先硬编码 #324059，切到 Codex/Claude 皮肤后配色不跟随）
    auto* label = new QLabel(this);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setText(inlineMd(text));
    label->setStyleSheet(QStringLiteral(
        "background-color:%1;border:1px solid %2;border-radius:10px;padding:8px 12px;"
        "font-size:11pt;")
                             .arg(theme::colors::accent().darker(300).name(),
                                  theme::colors::accent().name()));
    // 限宽：长目标不再横贯整个窗口（与助手正文一致的阅读宽度）
    label->setMaximumWidth(620);
    // ⚠️ 同时必须给最小宽度：带 wordWrap 的 QLabel 在"左 stretch + 右控件"布局里
    // 会被压到最小宽度，导致长目标被折成 187px 宽的一条竖排文字（实测）。
    label->setMinimumWidth(260);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    lay->addWidget(label);
}

// ---------- AssistantBlock ----------
AssistantBlock::AssistantBlock(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 2, 0, 2);
    lay->setSpacing(4);

    // 思考区：流式期间默认展开（此时正是"它在想什么"最有价值的时刻），
    // 结束后自动收起，保持消息流清爽
    m_thinkingToggle = new QToolButton(this);
    m_thinkingToggle->setText(QStringLiteral("▾ 💭 思考过程"));
    m_thinkingToggle->setCheckable(true);
    m_thinkingToggle->setChecked(true);
    m_thinkingToggle->setCursor(Qt::PointingHandCursor);
    m_thinkingToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_thinkingToggle->setVisible(false); // 首个思考增量到达时才显示
    m_thinkingToggle->setStyleSheet(QStringLiteral(
        "QToolButton{color:%1;border:none;text-align:left;font-size:10pt;}"
        "QToolButton:hover{color:%2;}")
                                        .arg(theme::colors::textDim().name(),
                                             theme::colors::accent().name()));

    m_thinkingLabel = new QLabel(this);
    m_thinkingLabel->setTextFormat(Qt::RichText);
    m_thinkingLabel->setWordWrap(true);
    m_thinkingLabel->setVisible(false);
    m_thinkingLabel->setStyleSheet(QStringLiteral(
        "color:%1;font-style:italic;background-color:%2;border-left:2px solid %3;"
        "border-radius:6px;padding:6px 8px;font-size:10pt;")
                                        .arg(theme::colors::textDim().name(), theme::colors::codeBg().name(),
                                             theme::colors::line().name()));
    connect(m_thinkingToggle, &QToolButton::toggled, this, [this](bool on) {
        m_thinkingLabel->setVisible(on);
        m_thinkingToggle->setText(on ? QStringLiteral("▾ 💭 思考过程")
                                     : QStringLiteral("▸ 💭 思考过程"));
    });

    m_contentLabel = new QLabel(this);
    m_contentLabel->setTextFormat(Qt::RichText);
    m_contentLabel->setWordWrap(true);
    m_contentLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_contentLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11pt;")
                                      .arg(theme::colors::text().name()));

    lay->addWidget(m_thinkingToggle);
    lay->addWidget(m_thinkingLabel);
    lay->addWidget(m_contentLabel);

    // 流式"正在生成"指示：正文末尾的品牌色方块 + 状态点，让用户明确知道还在输出
    m_streamRow = new QWidget(this);
    auto* streamLay = new QHBoxLayout(m_streamRow);
    streamLay->setContentsMargins(2, 0, 0, 0);
    streamLay->setSpacing(6);
    m_streamDot = new QLabel(QStringLiteral("●"), m_streamRow);
    m_streamDot->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;")
                                   .arg(theme::colors::accent().name()));
    m_streamLabel = new QLabel(QStringLiteral("正在生成…"), m_streamRow);
    m_streamLabel->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;")
                                     .arg(theme::colors::textDim().name()));
    streamLay->addWidget(m_streamDot);
    streamLay->addWidget(m_streamLabel);
    streamLay->addStretch(1);
    m_streamRow->setVisible(false);
    lay->addWidget(m_streamRow);

    // 完成后才显示的操作行（复制正文）——默认隐藏，避免空消息挂一排按钮
    m_actionRow = new QWidget(this);
    auto* actLay = new QHBoxLayout(m_actionRow);
    actLay->setContentsMargins(0, 0, 0, 0);
    actLay->setSpacing(8);
    auto* copyBtn = new QPushButton(QStringLiteral("复制"), m_actionRow);
    copyBtn->setFlat(true);
    copyBtn->setCursor(Qt::PointingHandCursor);
    copyBtn->setToolTip(QStringLiteral("复制这条回复的正文"));
    copyBtn->setStyleSheet(QStringLiteral(
        "QPushButton{color:%1;border:none;font-size:9.5pt;padding:1px 4px;}"
        "QPushButton:hover{color:%2;}")
                               .arg(theme::colors::textDim().name(), theme::colors::accent().name()));
    connect(copyBtn, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(m_contentText);
        Toast::post(window(), QStringLiteral("已复制回复正文（%1 字符）").arg(m_contentText.size()),
                    Toast::Level::Success, 1600);
    });
    actLay->addWidget(copyBtn);
    actLay->addStretch(1);
    m_actionRow->setVisible(false);
    lay->addWidget(m_actionRow);

    // 200ms 批量刷新（流式渲染性能硬要求，禁止逐 token 重绘）
    m_flushTimer.setInterval(200);
    m_flushTimer.setSingleShot(true);
    connect(&m_flushTimer, &QTimer::timeout, this, &AssistantBlock::flushBuffers);
}

void AssistantBlock::setStreaming(bool on) {
    m_streaming = on;
    m_streamRow->setVisible(on);
    if (!on) {
        m_actionRow->setVisible(!m_contentText.isEmpty());
        // 生成结束后收起思考区（内容已定稿，思考过程不再需要占位）
        if (m_thinkingToggle->isChecked())
            m_thinkingToggle->setChecked(false);
    }
}

void AssistantBlock::appendThinking(const QString& delta) {
    m_thinkingText += delta;
    m_pendingThinking += delta;
    if (!m_streaming)
        setStreaming(true); // 首个增量到达即进入"生成中"形态
    if (!m_flushTimer.isActive())
        m_flushTimer.start();
}

void AssistantBlock::appendContent(const QString& delta) {
    m_contentText += delta;
    m_pendingContent += delta;
    if (!m_streaming)
        setStreaming(true);
    if (!m_flushTimer.isActive())
        m_flushTimer.start();
}

void AssistantBlock::flushBuffers() {
    m_pendingThinking.clear();
    m_pendingContent.clear();
    // 决策: 每 200ms 整体重渲染当前块（5 帧/秒；QLabel 渲染 O(n) 在对话长度下可接受）
    if (!m_thinkingText.isEmpty()) {
        m_thinkingLabel->setText(mdToHtml(m_thinkingText));
        m_thinkingToggle->setVisible(true);
    }
    if (!m_contentText.isEmpty())
        m_contentLabel->setText(mdToHtml(m_contentText));
}

void AssistantBlock::finishStream() {
    if (m_flushTimer.isActive())
        m_flushTimer.stop();
    flushBuffers();
    setStreaming(false); // 收起"正在生成…"、露出复制按钮、折叠思考区
}

void AssistantBlock::resetStream() {
    if (m_flushTimer.isActive())
        m_flushTimer.stop();
    m_thinkingText.clear();
    m_contentText.clear();
    m_pendingThinking.clear();
    m_pendingContent.clear();
    m_thinkingLabel->clear();
    m_thinkingToggle->setVisible(false);
    m_actionRow->setVisible(false);
    setStreaming(true); // 重连期间仍处于"生成中"形态（那行提示不会被下一段增量覆盖成空）
    m_contentLabel->setText(QStringLiteral("<i style=\"color:%1\">连接中断，正在自动重试…</i>")
                                .arg(theme::colors::textDim().name()));
}

void AssistantBlock::setFinalContent(const QString& text) {
    m_contentText = text;
    finishStream();
    // 恢复历史会话时不必给每条旧消息都挂一个"复制"按钮——那只在刚生成完时有用
    m_actionRow->setVisible(false);
}

// ---------- ToolCallCard ----------
ToolCallCard::ToolCallCard(const QString& callId, const QString& toolName, QWidget* parent)
    : QWidget(parent), m_callId(callId), m_toolName(toolName) {
    setObjectName(QStringLiteral("toolCard"));
    setBorderColor(theme::colors::accent()); // 初始=运行中色

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 6, 10, 6);
    outer->setSpacing(4);

    // 头部：展开按钮 + 工具名 + 状态（圆点 + 文字）
    auto* headerRow = new QHBoxLayout();
    m_header = new QToolButton(this);
    m_header->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_header->setCheckable(true);
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->setText(QStringLiteral("▶ 🧰 %1").arg(toolName));
    m_header->setStyleSheet(QStringLiteral(
        "QToolButton{color:%1;border:none;font-weight:bold;text-align:left;}")
                                .arg(theme::colors::text().name()));
    connect(m_header, &QToolButton::toggled, this, &ToolCallCard::toggleBody);
    m_statusHint = new QLabel(this);
    m_statusHint->setStyleSheet(QStringLiteral("font-size:9.5pt;"));
    m_statusDot = new QLabel(this);
    m_statusDot->setTextFormat(Qt::RichText);
    m_statusDot->setText(theme::coloredDot(theme::colors::accent())); // 运行中=强调色
    headerRow->addWidget(m_header);
    headerRow->addStretch(1);
    headerRow->addWidget(m_statusHint);
    headerRow->addWidget(m_statusDot);
    outer->addLayout(headerRow);

    // 主体（默认折叠）
    m_body = new QWidget(this);
    auto* bodyLay = new QVBoxLayout(m_body);
    bodyLay->setContentsMargins(4, 0, 4, 0);
    bodyLay->setSpacing(2);
    m_body->setVisible(false);

    auto makeTitle = [this](const QString& t) {
        auto* l = new QLabel(t, m_body);
        l->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;").arg(theme::colors::textDim().name()));
        return l;
    };

    bodyLay->addWidget(makeTitle(QStringLiteral("参数")));
    m_argsLabel = new QLabel(m_body);
    m_argsLabel->setFont(theme::monoFont());
    m_argsLabel->setTextFormat(Qt::RichText);
    m_argsLabel->setWordWrap(true);
    m_argsLabel->setStyleSheet(QStringLiteral("color:%1;background-color:%2;border-radius:4px;padding:4px;")
                                                    .arg(theme::colors::text().name(), theme::colors::codeBg().name()));
    bodyLay->addWidget(m_argsLabel);

    bodyLay->addWidget(makeTitle(QStringLiteral("执行结果")));
    m_resultLabel = new QLabel(m_body);
    m_resultLabel->setFont(theme::monoFont());
    m_resultLabel->setTextFormat(Qt::RichText);
    m_resultLabel->setWordWrap(true);
    m_resultLabel->setStyleSheet(QStringLiteral("color:%1;background-color:%2;border-radius:4px;padding:4px;")
                                                      .arg(theme::colors::text().name(), theme::colors::codeBg().name()));
    bodyLay->addWidget(m_resultLabel);

    m_expandBtn = new QPushButton(QStringLiteral("展开"), m_body);
    m_expandBtn->setFlat(true);
    m_expandBtn->setVisible(false);
    connect(m_expandBtn, &QPushButton::clicked, this, [this] {
        m_resultExpanded = !m_resultExpanded;
        m_resultLabel->setText(m_resultExpanded ? m_fullResult : m_resultPreview);
        m_expandBtn->setText(m_resultExpanded ? QStringLiteral("收起") : QStringLiteral("展开"));
    });

    // 结果操作行：复制 / 重跑（结果不能只有"看"一种用法）
    auto* actionRow = new QHBoxLayout();
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->setSpacing(6);
    actionRow->addWidget(m_expandBtn);
    actionRow->addStretch(1);
    m_copyBtn = new QPushButton(QStringLiteral("复制结果"), m_body);
    m_copyBtn->setFlat(true);
    m_copyBtn->setCursor(Qt::PointingHandCursor);
    m_copyBtn->setToolTip(QStringLiteral("把该工具的完整输出复制到剪贴板"));
    m_copyBtn->setVisible(false);
    connect(m_copyBtn, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(m_resultRaw);
        Toast::post(window(), QStringLiteral("已复制 %1 的输出（%2 字符）")
                                 .arg(m_toolName).arg(m_resultRaw.size()),
                    Toast::Level::Success, 1800);
    });
    actionRow->addWidget(m_copyBtn);
    m_rerunBtn = new QPushButton(QStringLiteral("重跑"), m_body);
    m_rerunBtn->setFlat(true);
    m_rerunBtn->setCursor(Qt::PointingHandCursor);
    m_rerunBtn->setToolTip(QStringLiteral("用完全相同的参数再执行一次（会重新过权限门）"));
    m_rerunBtn->setVisible(false);
    connect(m_rerunBtn, &QPushButton::clicked, this, [this] {
        emit rerunRequested(m_toolName, m_argsRaw);
    });
    actionRow->addWidget(m_rerunBtn);
    bodyLay->addLayout(actionRow);

    m_costLabel = new QLabel(m_body);
    m_costLabel->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;").arg(theme::colors::textDim().name()));
    bodyLay->addWidget(m_costLabel);

    // 待确认三按钮（M1 权限门接入后可见）
    m_confirmRow = new QWidget(m_body);
    auto* confirmLay = new QHBoxLayout(m_confirmRow);
    confirmLay->setContentsMargins(0, 2, 0, 0);
    auto* allowOnce = new QPushButton(QStringLiteral("允许一次"), m_confirmRow);
    auto* allowAlways = new QPushButton(QStringLiteral("本会话总是允许"), m_confirmRow);
    auto* deny = new QPushButton(QStringLiteral("拒绝"), m_confirmRow);
    allowOnce->setProperty("decision", 0);
    allowAlways->setProperty("decision", 1);
    deny->setProperty("decision", 2);
    deny->setStyleSheet(QStringLiteral("QPushButton{color:%1;}").arg(theme::colors::error().name()));
    for (auto* btn : {allowOnce, allowAlways, deny}) {
        connect(btn, &QPushButton::clicked, this, [this, btn] {
            m_confirmRow->setVisible(false);
            emit permissionDecided(m_callId, btn->property("decision").toInt());
        });
        confirmLay->addWidget(btn);
    }
    confirmLay->addStretch(1);
    m_confirmRow->setVisible(false);
    bodyLay->addWidget(m_confirmRow);

    outer->addWidget(m_body);
}

void ToolCallCard::toggleBody(bool open) {
    m_body->setVisible(open);
    m_header->setText(m_header->text().replace(0, 1, open ? QStringLiteral("▼") : QStringLiteral("▶")));
}

void ToolCallCard::updateArgsPreview(const QString& argsSoFar) {
    m_argsLabel->setText(QStringLiteral("<pre style=\"white-space:pre-wrap;margin:0;\">%1</pre>")
                             .arg(escapeHtml(argsSoFar)));
}

void ToolCallCard::setArgs(const QString& args) {
    updateArgsPreview(args);
}

// 工具卡左边框是它的视觉锚点：让颜色随状态变化，
// 比只改一个 8pt 小圆点更容易在滚动中扫到（原先边框恒为强调色，卡片永远"看起来一样"）
void ToolCallCard::setBorderColor(const QColor& c) {
    setStyleSheet(QStringLiteral(
        "QWidget#toolCard{background-color:%1;border:1px solid %2;border-left:3px solid %3;"
        "border-radius:8px;}")
                      .arg(theme::colors::panel().name(), theme::colors::line().name(), c.name()));
}

void ToolCallCard::setRunning() {
    m_statusDot->setText(theme::coloredDot(theme::colors::accent()));
    m_statusHint->setText(QStringLiteral("执行中"));
    m_statusHint->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;")
                                    .arg(theme::colors::accent().name()));
    setBorderColor(theme::colors::accent());
}

void ToolCallCard::setSucceeded() {
    m_statusDot->setText(theme::coloredDot(theme::colors::success()));
    m_statusHint->setText(QStringLiteral("成功"));
    m_statusHint->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;")
                                    .arg(theme::colors::success().name()));
    setBorderColor(theme::colors::success());
}

void ToolCallCard::setFailed() {
    m_statusDot->setText(theme::coloredDot(theme::colors::error()));
    m_statusHint->setText(QStringLiteral("失败"));
    m_statusHint->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;")
                                    .arg(theme::colors::error().name()));
    setBorderColor(theme::colors::error());
}

void ToolCallCard::setAwaitingConfirm() {
    m_statusDot->setText(theme::coloredDot(theme::colors::warn()));
    m_statusHint->setText(QStringLiteral("等待确认"));
    m_statusHint->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;font-weight:bold;")
                                    .arg(theme::colors::warn().name()));
    setBorderColor(theme::colors::warn());
    if (!m_header->isChecked())
        m_header->toggle(); // 展开卡片露出三按钮
    m_confirmRow->setVisible(true);
}

void ToolCallCard::setResult(const QString& resultText, qint64 ms) {
    m_costLabel->setText(QStringLiteral("耗时 %1 ms").arg(ms));
    m_resultRaw = resultText; // 供「复制结果」原样复制（不带 HTML 着色）
    m_fullResult = QStringLiteral("<pre style=\"white-space:pre-wrap;margin:0;\">%1</pre>")
                       .arg(diffColorized(resultText));
    // 默认 3 行预览 + 展开按钮
    const QStringList lines = resultText.split(QLatin1Char('\n'));
    QString preview = lines.mid(0, 3).join(QLatin1Char('\n'));
    if (lines.size() > 3)
        preview += QStringLiteral("\n…（共 %1 行）").arg(lines.size());
    m_resultPreview = QStringLiteral("<pre style=\"white-space:pre-wrap;margin:0;\">%1</pre>")
                          .arg(diffColorized(preview));
    m_resultLabel->setText(m_resultPreview);
    m_resultExpanded = false;
    m_expandBtn->setText(QStringLiteral("展开"));
    m_expandBtn->setVisible(lines.size() > 3);
    // 结果就绪后操作按钮才有意义
    m_copyBtn->setVisible(!resultText.isEmpty());
    m_rerunBtn->setVisible(!m_argsRaw.isEmpty());
}

// ---------- 线程内变更审查条（Codex 式，消息流顶部常驻） ----------
QString ChangeSummaryBar::formatSummary(int files, int added, int removed) {
    if (files <= 0)
        return QStringLiteral("无文件变更");
    QString s = QStringLiteral("变更 %1 个文件").arg(files);
    // 只在真的有增删行时展示数字，避免「+0/−0」这种噪声
    if (added > 0 || removed > 0)
        s += QStringLiteral("  +%1/−%2").arg(added).arg(removed);
    return s;
}

ChangeSummaryBar::ChangeSummaryBar(QWidget* parent) : QFrame(parent) {
    setStyleSheet(QStringLiteral(
        "QFrame{background-color:%1;border:1px solid %2;border-radius:6px;}")
                      .arg(theme::colors::panel().name(), theme::colors::line().name()));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 4, 8, 4);
    lay->setSpacing(4);

    m_header = new QToolButton(this);
    m_header->setCheckable(true);
    m_header->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->setStyleSheet(QStringLiteral(
        "QToolButton{border:none;background:transparent;color:%1;font-size:10pt;text-align:left;}")
                                .arg(theme::colors::textDim().name()));
    connect(m_header, &QToolButton::clicked, this, &ChangeSummaryBar::toggleExpanded);
    lay->addWidget(m_header);

    m_body = new QWidget(this);
    auto* bodyLay = new QVBoxLayout(m_body);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    m_list = new QListWidget(m_body);
    m_list->setStyleSheet(QStringLiteral(
        "QListWidget{background:transparent;border:none;font-size:10pt;outline:none;}"
        "QListWidget::item{height:26px;border-radius:6px;padding-left:4px;color:%2;margin:1px 0;}"
        "QListWidget::item:hover{background-color:%1;}")
                              .arg(theme::colors::window().name(), theme::colors::textDim().name()));
    m_list->setMaximumHeight(160);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item)
            emit fileActivated(item->data(Qt::UserRole).toString());
    });
    bodyLay->addWidget(m_list);
    m_body->setVisible(false);
    lay->addWidget(m_body);

    // 空态不占高度：无变更时整条隐藏
    setVisible(false);
}

void ChangeSummaryBar::toggleExpanded() {
    m_expanded = m_header->isChecked();
    m_body->setVisible(m_expanded);
    refreshHeader();
}

void ChangeSummaryBar::refreshHeader() {
    int added = 0;
    int removed = 0;
    for (int v : m_added)
        added += v;
    for (int v : m_removed)
        removed += v;
    const QChar arrow = m_expanded ? QChar(0x25BE) : QChar(0x25B8); // ▾ / ▸
    m_header->setText(QStringLiteral("%1 🔧 %2")
                          .arg(arrow)
                          .arg(formatSummary(int(m_files.size()), added, removed)));
}

// ---------- 轻量浮层提示（Toast） ----------
namespace {
// 单个 Toast 的停留时长与尺寸常量
constexpr int kToastMargin = 16;  // 距父容器右下角的间距
constexpr int kToastGap = 8;      // 叠放间距
constexpr int kToastWidth = 340;  // 固定宽度：让尺寸可预测（否则宽高随文字换行变化，
                                  // 定位与实际渲染高度对不上，实测会被窗口下沿裁掉）
} // namespace

QVector<Toast*>& Toast::live() {
    static QVector<Toast*> v;
    return v;
}

Toast::Toast(QWidget* parent, const QString& text, Level level, int msec)
    : QFrame(parent), m_msec(msec) {
    setAttribute(Qt::WA_TransparentForMouseEvents); // 不抢鼠标，纯反馈
    setStyleSheet(QStringLiteral(
        "QFrame{background-color:%1;border:1px solid %2;border-radius:8px;}")
                      .arg(theme::colors::panel().name(), theme::colors::line().name()));
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(12, 9, 12, 9);
    lay->setSpacing(8);

    // 左侧语义色竖条：一眼区分 信息/成功/警告/失败，不依赖读文字
    auto* bar = new QFrame(this);
    bar->setFixedWidth(3);
    QColor c = theme::colors::accent();
    switch (level) {
    case Level::Success: c = theme::colors::success(); break;
    case Level::Warn: c = theme::colors::warn(); break;
    case Level::Error: c = theme::colors::error(); break;
    case Level::Info: break;
    }
    bar->setStyleSheet(QStringLiteral("background-color:%1;border:none;border-radius:1px;").arg(c.name()));
    lay->addWidget(bar);

    auto* label = new QLabel(text, this);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color:%1;font-size:10pt;border:none;")
                             .arg(theme::colors::text().name()));
    lay->addWidget(label);

    // 固定宽度 + 按需高度：先按固定宽布局，再取实际 sizeHint 定高
    setFixedWidth(kToastWidth);
    layout()->activate();
    setFixedHeight(layout()->sizeHint().height());

    m_opacity = new QGraphicsOpacityEffect(this);
    m_opacity->setOpacity(1.0);
    setGraphicsEffect(m_opacity);

    live().push_back(this);
    reposition();
    show();
    raise();

    // 停留后淡出并自毁
    QTimer::singleShot(m_msec, this, [this] {
        auto* fade = new QPropertyAnimation(m_opacity, "opacity", this);
        fade->setDuration(220);
        fade->setStartValue(1.0);
        fade->setEndValue(0.0);
        connect(fade, &QPropertyAnimation::finished, this, [this] { close(); deleteLater(); });
        fade->start(QAbstractAnimation::DeleteWhenStopped);
    });
}

Toast::~Toast() {
    live().removeAll(this);
    // 自身消失后，让上面的提示落回原位
    for (Toast* t : live())
        if (t)
            t->reposition();
}

void Toast::reposition() {
    QWidget* p = parentWidget();
    if (!p)
        return;
    // 自底向上叠放：越早出现的越靠下
    int offset = kToastMargin;
    for (Toast* t : live()) {
        if (!t || t == this)
            continue;
        if (t->y() > y() || (t->y() == y() && t > this))
            offset += t->height() + kToastGap;
    }
    // 父窗口是 QMainWindow 时，窗口坐标含菜单栏与状态栏：只避让菜单栏而不避让状态栏，
    // 会让提示被状态栏裁掉一半并与 Composer 按钮重叠——这里显式加回状态栏高度
    int bottomInset = 0;
    if (auto* mw = qobject_cast<QMainWindow*>(p))
        if (mw->statusBar())
            bottomInset = mw->statusBar()->height();

    const int w = qMin(width(), qMax(200, p->width() - 2 * kToastMargin));
    const int y = p->height() - bottomInset - height() - offset;
    move(p->width() - w - kToastMargin, qMax(kToastMargin, y));
    raise(); // 保证压在同级控件（Composer 边框等）之上
}

void Toast::post(QWidget* parent, const QString& text, Level level, int msec) {
    if (!parent || text.isEmpty())
        return;
    // 延迟到当前事件处理结束再建：避免在信号回调里改动控件树
    QPointer<QWidget> guard(parent);
    QTimer::singleShot(0, parent, [guard, text, level, msec] {
        if (guard)
            new Toast(guard, text, level, msec);
    });
}

void ChangeSummaryBar::setChanges(const QStringList& paths, const QVector<int>& added,
                                  const QVector<int>& removed) {
    m_files = paths;
    m_added = added;
    m_removed = removed;
    m_list->clear();
    for (int i = 0; i < paths.size(); ++i) {
        const int a = i < added.size() ? added[i] : 0;
        const int r = i < removed.size() ? removed[i] : 0;
        auto* item = new QListWidgetItem(
            QStringLiteral("%1   +%2/−%3").arg(QFileInfo(paths[i]).fileName()).arg(a).arg(r));
        item->setToolTip(paths[i]); // 完整路径（同名文件靠悬浮区分）
        item->setData(Qt::UserRole, paths[i]);
        m_list->addItem(item);
    }
    refreshHeader();
    // 有新变更时展开一次（让用户看到这轮改了什么），之后由用户控制
    if (!m_files.isEmpty() && !m_expanded) {
        m_expanded = true;
        m_header->setChecked(true);
        m_body->setVisible(true);
        refreshHeader();
    }
    setVisible(!m_files.isEmpty());
}

void ChangeSummaryBar::clearChanges() {
    m_files.clear();
    m_added.clear();
    m_removed.clear();
    m_list->clear();
    m_expanded = false;
    m_header->setChecked(false);
    m_body->setVisible(false);
    refreshHeader();
    setVisible(false);
}

} // namespace miderforge
