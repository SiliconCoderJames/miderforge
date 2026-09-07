// 记忆管理（规格 6 分层记忆）：L1 核心记忆文件、L2 会话摘要（滚动 50 条）、L3 档案库；
// 检索 = FTS5 trigram（短词 LIKE 兜底）→ 综合评分 → Top5 → access_count 回写
#pragma once
#include <QVector>
#include <QString>

namespace miderforge {

class Database;

class MemoryManager {
public:
    struct MemoryRecord {
        qint64 id = 0;
        QString type; // user_profile|coding_pref|task_lesson|project_facts|session_summary
        QString content;
        double importance = 0.5;
        QString status = QStringLiteral("active");
        qint64 createdAt = 0;
        qint64 updatedAt = 0;
        qint64 accessCount = 0;
        qint64 lastAccessedAt = 0;
    };

    MemoryManager(Database* db, const QString& l1Path);

    // ---- L1 核心记忆（文件，用户可手改；上限 4000 token 由调用方控制） ----
    QString loadL1() const;              // 文件不存在返回空串
    bool saveL1(const QString& content) const;
    long long l1Tokens() const;          // 当前 L1 token 占用（导航底部进度条数据源）

    // ---- L3 档案库 ----
    qint64 addMemory(const QString& type, const QString& content, double importance);
    bool archiveMemory(qint64 id);                 // 废弃=标记，不物理删（规格 5）
    bool updateContent(qint64 id, const QString& content);
    QVector<MemoryRecord> listAll(const QString& typeFilter = QString()) const;
    bool recordAccess(qint64 id);                  // access_count+1、last_accessed_at 更新

    // ---- 检索（任务开始时注入 system prompt「相关记忆」段） ----
    // 短词（<3 字符）自动降级 LIKE 全扫（必须有单测覆盖）
    QVector<MemoryRecord> retrieve(const QString& rawQuery, int topN = 5);

    // ---- L2 会话摘要（滚动上限 50 条，FIFO） ----
    qint64 addSessionSummary(const QString& content);
    QVector<MemoryRecord> recentSummaries(int n = 5) const;

    // 综合得分（暴露给单测）：0.5*BM25 + 0.2*importance + 0.2*时间衰减(半衰期30天) + 0.1*log(access+1)
    static double score(double bm25Rank, double importance, qint64 updatedAt,
                        qint64 nowSecs, qint64 accessCount);

private:
    QVector<MemoryRecord> selectBySql(const QString& sql, const QVariantList& binds) const;

    Database* m_db = nullptr;
    QString m_l1Path;
};

} // namespace miderforge
