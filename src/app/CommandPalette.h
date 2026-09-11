// 命令面板（Ctrl+K）：键盘优先的统一入口，对齐 Codex/ZCode 的 Ctrl+K 搜索。
// 一个输入框同时检索三类目标：功能页 / 会话 / 动作（新建会话、切主题、打开设置…），
// ↑↓ 选择、Enter 执行、Esc 关闭——不必先用鼠标找菜单。
#pragma once
#include <QDialog>
#include <QString>
#include <QVector>

class QLabel;
class QLineEdit;
class QListWidget;

namespace miderforge {

class CommandPalette : public QDialog {
    Q_OBJECT
public:
    struct Item {
        QString kind;   // "功能页" / "会话" / "动作"
        QString title;  // 显示主文本
        QString hint;   // 右侧快捷键/时间提示
        int actionId = -1;  // 动作编号（由调用方定义，原样回传）
        qint64 sessionId = -1;
        int pageIndex = -1;
    };

    explicit CommandPalette(QWidget* parent = nullptr);

    // 候选集在打开前设置
    void setItems(const QVector<Item>& items);

    // 纯函数（可单测）：按查询过滤并排序候选。
    // 规则：空查询返回全部（保持原序）；否则大小写不敏感子串匹配，
    // 前缀命中优先、其次命中位置更靠前、再次主文本更短（更"精确"）。
    static QVector<int> rank(const QStringList& titles, const QString& query);

signals:
    // 用户选定：pageIndex / sessionId / actionId 三者按 kind 取用
    void pageChosen(int pageIndex);
    void sessionChosen(qint64 sessionId);
    void actionChosen(int actionId);

protected:
    void keyPressEvent(QKeyEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void refilter();
    void acceptCurrent();

    QLineEdit* m_input = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_hint = nullptr;
    QVector<Item> m_items;
};

} // namespace miderforge
