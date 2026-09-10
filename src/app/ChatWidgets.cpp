// 聊天控件集实现
#include "app/ChatWidgets.h"
#include "app/Theme.h"
#include <QHBoxLayout>
#include <QPushButton>
#include <QRegularExpression>
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
    auto* label = new QLabel(this);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setText(inlineMd(text));
    label->setStyleSheet(QStringLiteral(
        "background-color:#324059;border:1px solid %1;border-radius:8px;padding:6px 10px;")
                             .arg(theme::colors::accent().name()));
    label->setMaximumWidth(560);

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 2, 0, 2);
    lay->addStretch(1);
    lay->addWidget(label);
}

// ---------- AssistantBlock ----------
AssistantBlock::AssistantBlock(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 2, 0, 2);
    lay->setSpacing(2);

    m_thinkingToggle = new QToolButton(this);
    m_thinkingToggle->setText(QStringLiteral("💭 思考过程"));
    m_thinkingToggle->setCheckable(true);
    m_thinkingToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_thinkingToggle->setVisible(false); // 首个思考增量到达时才显示
    m_thinkingToggle->setStyleSheet(QStringLiteral(
        "QToolButton{color:%1;border:none;text-align:left;}")
                                        .arg(theme::colors::textDim().name()));

    m_thinkingLabel = new QLabel(this);
    m_thinkingLabel->setTextFormat(Qt::RichText);
    m_thinkingLabel->setWordWrap(true);
    m_thinkingLabel->setVisible(false);
    m_thinkingLabel->setStyleSheet(QStringLiteral(
        "color:%1;font-style:italic;background-color:%2;border-radius:6px;padding:6px;")
                                        .arg(theme::colors::textDim().name(), theme::colors::panel().name()));
    connect(m_thinkingToggle, &QToolButton::toggled, m_thinkingLabel, &QWidget::setVisible);

    m_contentLabel = new QLabel(this);
    m_contentLabel->setTextFormat(Qt::RichText);
    m_contentLabel->setWordWrap(true);
    m_contentLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_contentLabel->setStyleSheet(QStringLiteral("color:%1;").arg(theme::colors::text().name()));

    lay->addWidget(m_thinkingToggle);
    lay->addWidget(m_thinkingLabel);
    lay->addWidget(m_contentLabel);

    // 200ms 批量刷新（流式渲染性能硬要求，禁止逐 token 重绘）
    m_flushTimer.setInterval(200);
    m_flushTimer.setSingleShot(true);
    connect(&m_flushTimer, &QTimer::timeout, this, &AssistantBlock::flushBuffers);
}

void AssistantBlock::appendThinking(const QString& delta) {
    m_thinkingText += delta;
    m_pendingThinking += delta;
    if (!m_flushTimer.isActive())
        m_flushTimer.start();
}

void AssistantBlock::appendContent(const QString& delta) {
    m_contentText += delta;
    m_pendingContent += delta;
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
}

void AssistantBlock::resetStream() {
    if (m_flushTimer.isActive())
        m_flushTimer.stop();
    m_thinkingText.clear();
    m_contentText.clear();
    m_pendingThinking.clear();
    m_pendingContent.clear();
    m_thinkingLabel->clear();
    m_contentLabel->setText(QStringLiteral("<i style=\"color:%1\">连接中断，正在自动重试…</i>")
                                .arg(theme::colors::textDim().name()));
}

void AssistantBlock::setFinalContent(const QString& text) {
    m_contentText = text;
    finishStream();
}

// ---------- ToolCallCard ----------
ToolCallCard::ToolCallCard(const QString& callId, const QString& toolName, QWidget* parent)
    : QWidget(parent), m_callId(callId) {
    setObjectName(QStringLiteral("toolCard"));
    setStyleSheet(QStringLiteral(
        "QWidget#toolCard{background-color:%1;border:1px solid #35383d;border-left:3px solid %2;border-radius:6px;}")
                      .arg(theme::colors::panel().name(), theme::colors::accent().name()));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 4, 8, 4);
    outer->setSpacing(4);

    // 头部：展开按钮 + 工具名 + 状态点
    auto* headerRow = new QHBoxLayout();
    m_header = new QToolButton(this);
    m_header->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_header->setCheckable(true);
    m_header->setText(QStringLiteral("▶ 🧰 %1").arg(toolName));
    m_header->setStyleSheet(QStringLiteral(
        "QToolButton{color:%1;border:none;font-weight:bold;text-align:left;}")
                                .arg(theme::colors::text().name()));
    connect(m_header, &QToolButton::toggled, this, &ToolCallCard::toggleBody);
    m_statusDot = new QLabel(this);
    m_statusDot->setTextFormat(Qt::RichText);
    m_statusDot->setText(theme::coloredDot(theme::colors::accent())); // 运行中=强调色
    headerRow->addWidget(m_header);
    headerRow->addStretch(1);
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
        l->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;").arg(theme::colors::textDim().name()));
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
    bodyLay->addWidget(m_expandBtn);

    m_costLabel = new QLabel(m_body);
    m_costLabel->setStyleSheet(QStringLiteral("color:%1;font-size:8pt;").arg(theme::colors::textDim().name()));
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

void ToolCallCard::setRunning() {
    m_statusDot->setText(theme::coloredDot(theme::colors::accent()));
}

void ToolCallCard::setSucceeded() {
    m_statusDot->setText(theme::coloredDot(theme::colors::success()));
}

void ToolCallCard::setFailed() {
    m_statusDot->setText(theme::coloredDot(theme::colors::error()));
}

void ToolCallCard::setAwaitingConfirm() {
    m_statusDot->setText(theme::coloredDot(theme::colors::warn()));
    if (!m_header->isChecked())
        m_header->toggle(); // 展开卡片露出三按钮
    m_confirmRow->setVisible(true);
}

void ToolCallCard::setResult(const QString& resultText, qint64 ms) {
    m_costLabel->setText(QStringLiteral("耗时 %1 ms").arg(ms));
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
}

} // namespace miderforge
