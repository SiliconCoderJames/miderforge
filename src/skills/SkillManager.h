// 技能库管理（规格 7）：SKILL.md 读写（YAML frontmatter）、FTS5 检索、使用统计与降权、同名 merge version+1
#pragma once
#include <QDateTime>
#include <QVector>
#include <QString>

namespace miderforge {

class Database;

class SkillManager {
public:
    struct SkillMeta {
        qint64 id = 0;
        QString name;
        QString description; // 一句话，常驻 system prompt 的只有这部分
        QString path;
        QString status = QStringLiteral("active"); // active|deprecated|review
        int usageCount = 0;
        int successCount = 0;
        double avgRounds = 0;
        int version = 1;
    };

    SkillManager(Database* db, const QString& skillsDir);

    // ---- SKILL.md 解析/序列化（frontmatter: name/description/version） ----
    static bool parseFrontmatter(const QString& text, QString* name, QString* description,
                                 int* version, QString* body);
    static QString serialize(const QString& name, const QString& description, int version,
                             const QString& body);

    // ---- CRUD ----
    // 写盘 + 数据库 upsert；同名已存在 → version+1 且保留累计统计（merge 语义）
    bool writeSkill(const QString& name, const QString& description, const QString& body,
                    QString* err = nullptr, int* newVersion = nullptr);
    bool markDeprecated(const QString& name);            // 待审查/废弃=标记不删除
    SkillMeta find(const QString& name) const;
    QVector<SkillMeta> listAll() const;
    // FTS5 检索（任务开始时注入「可用技能」段）；空查询返回全部 active
    QVector<SkillMeta> search(const QString& query, int limit = 10) const;
    QString loadSkillMd(const QString& name) const;

    // ---- 使用统计：usage_count+1，成败计入 success_count；成功率<30% 且使用≥5 → 标记待审查 ----
    void recordUsage(const QString& name, bool success, int rounds);

    // 种子技能：首次运行写入 cpp-cmake-qt-build（规格 16 交付物）
    void ensureSeedSkill();

private:
    Database* m_db = nullptr;
    QString m_dir;
};

} // namespace miderforge
