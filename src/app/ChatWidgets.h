// 聊天界面控件集：简易 markdown 渲染、用户气泡、助手双栏块（思考折叠+正文）、工具调用卡片（规格 4.3）
#pragma once
#include <QFrame>
#include <QLabel>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

class QPushButton;

namespace miderforge {

// 简易 markdown → HTML：围栏代码块（等宽灰底）、行内代码、**加粗**、*斜体*、# 标题、- 列表
QString mdToHtml(const QString& md);

// 右对齐蓝色边框用户气泡
class UserBubble : public QWidget {
    Q_OBJECT
public:
    explicit UserBubble(const QString& text, QWidget* parent = nullptr);
};

// 助手消息：折叠区「💭 思考过程」（灰斜体，默认折叠）+ 正文（markdown-lite）
// 流式增量内部 200ms 批量刷新（禁止逐 token 重绘，踩坑清单 #8）
class AssistantBlock : public QWidget {
    Q_OBJECT
public:
    explicit AssistantBlock(QWidget* parent = nullptr);
    void appendThinking(const QString& delta);
    void appendContent(const QString& delta);
    // 流结束后立即冲刷缓冲并做最终渲染
    void finishStream();
    // 重试前清空已渲染内容
    void resetStream();
    // 设置最终正文（覆盖）
    void setFinalContent(const QString& text);

private slots:
    void flushBuffers();

private:
    QToolButton* m_thinkingToggle = nullptr;
    QLabel* m_thinkingLabel = nullptr;
    QLabel* m_contentLabel = nullptr;
    QTimer m_flushTimer;
    QString m_pendingThinking;
    QString m_pendingContent;
    QString m_thinkingText;
    QString m_contentText;
};

// 工具调用卡片：蓝色左边框；头部=图标+工具名+状态色点；点击展开参数/结果/耗时；
// 待确认时内嵌三按钮（允许一次/本会话总是允许/拒绝）—— M1 权限门接入后启用
class ToolCallCard : public QWidget {
    Q_OBJECT
public:
    explicit ToolCallCard(const QString& callId, const QString& toolName, QWidget* parent = nullptr);
    void updateArgsPreview(const QString& argsSoFar);
    void setArgs(const QString& args);
    void setRunning();
    void setSucceeded();
    void setFailed();
    void setAwaitingConfirm(); // M1：等待用户授权
    void setResult(const QString& resultText, qint64 ms);

signals:
    void permissionDecided(const QString& callId, int decision); // 0=允许一次 1=总是允许 2=拒绝

private:
    void toggleBody(bool open);
    QString m_callId;
    QToolButton* m_header = nullptr;
    QLabel* m_statusDot = nullptr;
    QWidget* m_body = nullptr;
    QLabel* m_argsLabel = nullptr;
    QLabel* m_resultLabel = nullptr;
    QPushButton* m_expandBtn = nullptr;
    QLabel* m_costLabel = nullptr;
    QWidget* m_confirmRow = nullptr;
    QString m_resultPreview;
    QString m_fullResult;
    bool m_resultExpanded = false;
};

} // namespace miderforge
