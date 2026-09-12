// 内置查看器（HTML / Markdown / 纯文本）：左列工作区文件，右侧渲染预览，可切编辑并保存。
// 设计取舍：
//   - 不引入 WebEngine（体积巨大），用 Qt 富文本引擎渲染——Markdown 走与聊天区同源的
//     mdToHtml，HTML 经 PreviewDoc 净化后交给 QTextBrowser（无脚本执行面）；
//   - 读取受 P0-2 保护区约束：命中保护区/名字清单的文件在列表中直接不出现，也不可打开；
//   - 保存必须过权限门（PermissionGate）：与工具层同一套判定，UI 不开后门。
#pragma once
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTextBrowser>
#include <QToolButton>
#include <QWidget>

namespace miderforge {

class PreviewPane : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPane(QWidget* parent = nullptr);

    // 载入并渲染指定文件（返回是否成功；保护区命中会拒绝并给出提示）
    bool openFile(const QString& path);
    void setWorkspaceRoot(const QString& root); // 文件列表根目录
    void refreshFileList();
    QString currentPath() const { return m_path; }

signals:
    void statusMessage(const QString& text);

private:
    bool loadInto(const QString& path, QString* err);
    void render();
    void setEditing(bool on);
    void saveEdits();
    bool pathBlocked(const QString& path, QString* reason) const;

    QString m_root;
    QString m_path;
    QString m_rawText;
    bool m_editing = false;

    QLabel* m_title = nullptr;
    QLabel* m_hint = nullptr;
    QListWidget* m_files = nullptr;
    QTextBrowser* m_view = nullptr;
    QPlainTextEdit* m_editor = nullptr;
    QToolButton* m_editToggle = nullptr;
    QPushButton* m_saveBtn = nullptr;
};

} // namespace miderforge
