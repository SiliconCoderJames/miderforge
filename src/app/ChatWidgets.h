// 聊天界面控件集：简易 markdown 渲染、用户气泡、助手双栏块（思考折叠+正文）、工具调用卡片（规格 4.3）
#pragma once
#include <QFrame>
#include <QLabel>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QVector>
#include <QWidget>

class QGraphicsOpacityEffect;
class QListWidget;
class QListWidgetItem;
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
    // 流式形态切换：生成中显示"正在生成…"并展开思考区；结束后收起并露出复制按钮
    void setStreaming(bool on);

private slots:
    void flushBuffers();

private:
    QToolButton* m_thinkingToggle = nullptr;
    QLabel* m_thinkingLabel = nullptr;
    QLabel* m_contentLabel = nullptr;
    QWidget* m_streamRow = nullptr;   // "● 正在生成…"
    QLabel* m_streamDot = nullptr;
    QLabel* m_streamLabel = nullptr;
    QWidget* m_actionRow = nullptr;   // 完成后：复制 + 元信息脚注
    QLabel* m_metaLabel = nullptr;    // 元信息脚注（模型 · 时间）
    QTimer m_flushTimer;
    QString m_pendingThinking;
    QString m_pendingContent;
    QString m_thinkingText;
    QString m_contentText;
    bool m_streaming = false;
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
    // 记住原始参数，供「重跑」原样再发一次
    void setArgsRaw(const QString& args) { m_argsRaw = args; }

signals:
    void permissionDecided(const QString& callId, int decision); // 0=允许一次 1=总是允许 2=拒绝
    // 用户点「重跑该工具」：携带工具名与原始参数，由上层重新过权限门后执行
    void rerunRequested(const QString& toolName, const QString& argsJson);

private:
    void toggleBody(bool open);
    void setBorderColor(const QColor& c); // 左边框随状态变色（卡片视觉锚点）
    QString m_callId;
    QString m_toolName;
    QString m_argsRaw;
    QToolButton* m_header = nullptr;
    QLabel* m_statusDot = nullptr;
    QLabel* m_statusHint = nullptr; // 执行中/成功/失败/等待确认
    QWidget* m_body = nullptr;
    QLabel* m_argsLabel = nullptr;
    QLabel* m_resultLabel = nullptr;
    QPushButton* m_expandBtn = nullptr;
    QPushButton* m_copyBtn = nullptr;
    QPushButton* m_rerunBtn = nullptr;
    QLabel* m_costLabel = nullptr;
    QWidget* m_confirmRow = nullptr;
    QString m_resultPreview;
    QString m_fullResult;
    QString m_resultRaw;
    bool m_resultExpanded = false;
};

// 线程内变更审查条（Codex 式）：常驻消息流顶部，折叠态一行摘要、展开态列出变更文件。
// 取代原先固定在右侧的 180px「变更文件」栏——右栏占宽度，且文件多时挤压消息流。
// 无变更时整体隐藏（不占高度），避免空态挂一个「变更文件（0）」。
class ChangeSummaryBar : public QFrame {
    Q_OBJECT
public:
    explicit ChangeSummaryBar(QWidget* parent = nullptr);

    // 纯函数（可单测）：摘要文案。files=文件数，added/removed=总增删行数
    static QString formatSummary(int files, int added, int removed);

    // 重建列表（files 为空则整体隐藏并复位为折叠态）
    void setChanges(const QStringList& paths, const QVector<int>& added,
                    const QVector<int>& removed);
    void clearChanges();
    bool isEmpty() const { return m_files.isEmpty(); }

signals:
    // 用户点击某个变更文件（沿用既有 diff 弹窗）
    void fileActivated(const QString& path);

private:
    void toggleExpanded();
    void refreshHeader();

    QStringList m_files;
    QVector<int> m_added;
    QVector<int> m_removed;
    QToolButton* m_header = nullptr;
    QListWidget* m_list = nullptr;
    QWidget* m_body = nullptr;
    bool m_expanded = false;
};

// 轻量浮层提示（Toast）：右下角淡入，若干秒后自动淡出；同类提示叠放而非相互覆盖。
// 用途：把「已复制」「已入队」「已切换」这类回执做成不打断操作的可视反馈，
// 替代此前一律弹 QMessageBox 的做法（模态框会打断输入，且无人值守时会卡住流程）。
class Toast : public QFrame {
    Q_OBJECT
public:
    enum class Level { Info, Success, Warn, Error };

    // parent 建议传窗口的主内容区：提示会贴在它的右下角。
    // 命名用 post 而非 show：show() 会遮蔽 QWidget::show，导致类内 `show()` 解析到静态重载
    static void post(QWidget* parent, const QString& text,
                     Level level = Level::Info, int msec = 2600);

    ~Toast() override;

private:
    explicit Toast(QWidget* parent, const QString& text, Level level, int msec);
    void reposition();                 // 与其他存活中的 Toast 叠放
    static QVector<Toast*>& live();    // 当前存活列表（用于堆叠与重排）

    int m_msec = 2600;
    QGraphicsOpacityEffect* m_opacity = nullptr;
};

} // namespace miderforge
