// 技能库视图（规格 4.5）：左列表（搜索 FTS5 实时过滤 + 名称+成功率+使用次数）；
// 右详情（描述/统计/SKILL.md 预览/手动新建/编辑/标记废弃）
#pragma once
#include "skills/SkillManager.h"
#include <QLineEdit>
#include <QListWidget>
#include <QLabel>
#include <QPlainTextEdit>
#include <QWidget>

class QPushButton;

namespace miderforge {

class SkillView : public QWidget {
    Q_OBJECT
public:
    SkillView(SkillManager* skills, QWidget* parent = nullptr);

public slots:
    void reload();

protected:
    // 切到本页即重载（任务运行中可能自沉淀技能，回到本页所见即最新）
    void showEvent(QShowEvent* ev) override;

private slots:
    void onSearchChanged(const QString& text);
    void onSelected();
    void onNewSkill();
    void onEditSkill();
    void onDeprecate();

private:
    void showEditor(const QString& name, const QString& description, const QString& body);

    SkillManager* m_skills = nullptr;
    QLineEdit* m_search = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_meta = nullptr;
    QPlainTextEdit* m_preview = nullptr;
    QPushButton* m_editBtn = nullptr;
    QPushButton* m_deprecateBtn = nullptr;
    QVector<SkillManager::SkillMeta> m_records;
    QString m_current;
};

} // namespace miderforge
