// 记忆管理（规格 6 分层记忆）：L1 核心记忆文件、L2 会话摘要（滚动 50 条）、L3 档案库；
// 检索 = FTS5 trigram（短词 LIKE 兜底）→ 综合评分 → Top5 → access_count 回写
#pragma once
#include <QDateTime>
#include <QJsonObject>
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
    // 用户 24h 内手改保护（规格 6）：Agent 自动改写前必须检查；Agent 自己的写入不算用户编辑
    bool l1UserEditedRecently() const;

    // ---- L3 档案库 ----
    qint64 addMemory(const QString& type, const QString& content, double importance);
    bool archiveMemory(qint64 id);                 // 废弃=标记，不物理删（规格 5）
    bool updateContent(qint64 id, const QString& content);
    QVector<MemoryRecord> listAll(const QString& typeFilter = QString()) const;
    bool recordAccess(qint64 id);                  // access_count+1、last_accessed_at 更新

    // ---- 一致性失效（存储体系 write-through + invalidation） ----
    // 从收尾 LLM 输出中提取"被本次新知覆盖"的记忆编号：只接受数字/数字字符串、去重、
    // 且必须 ⊆ 本次任务实际注入过的编号集合（防 LLM 幻觉编号误伤无关记忆）
    static QVector<qint64> filterSupersededIds(const QJsonObject& parsed,
                                               const QVector<qint64>& injectedIds);
    // 批量归档（单条 UPDATE … IN）：L1 改写时同步失效被取代的 L3 旧条目；返回成功归档数
    int archiveMemories(const QVector<qint64>& ids);

    // ---- 检索（任务开始时注入 system prompt「相关记忆」段） ----
    // 短词（<3 字符）自动降级 LIKE 全扫（必须有单测覆盖）
    QVector<MemoryRecord> retrieve(const QString& rawQuery, int topN = 5);

    // ---- L2 会话摘要（滚动上限 50 条，FIFO） ----
    qint64 addSessionSummary(const QString& content);
    QVector<MemoryRecord> recentSummaries(int n = 5) const;

    // 综合得分（暴露给单测）：0.5*BM25 + 0.2*importance + 0.2*时间衰减(半衰期30天) + 0.1*log(access+1)
    static double score(double bm25Rank, double importance, qint64 updatedAt,
                        qint64 nowSecs, qint64 accessCount);

    // ---- 一致性扫描（存储体系：跨条目矛盾候选检测） ----
    struct ContradictionPair {
        qint64 idA = 0;
        qint64 idB = 0;
        QString contentA;
        QString contentB;
        QString type;
        double similarity = 0; // 字符三元组 Dice 系数 (0,1]
    };
    // 检测"高度相似但不相同"的 active 记忆对（矛盾/冗余的确定性信号，交用户裁决）：
    // 事实类类型（project_facts/task_lesson/coding_pref/user_profile）、相似度 ∈ [0.35, 0.95]、
    // 排除完全相同（冗余副本另计）。会话摘要按时间天然演化，不参与。按相似度降序，至多 maxPairs
    QVector<ContradictionPair> findContradictionCandidates(int maxPairs = 20) const;
    // 字符三元组 Dice 相似度（暴露给单测；中文友好，无需分词）
    static double trigramDice(const QString& a, const QString& b);

private:
    QVector<MemoryRecord> selectBySql(const QString& sql, const QVariantList& binds) const;

    Database* m_db = nullptr;
    QString m_l1Path;
    mutable QDateTime m_lastAgentWrite; // Agent 最近一次写 L1 的文件 mtime（用户保护判定）
};

} // namespace miderforge
