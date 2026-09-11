// 记忆管理（规格 6 分层记忆）：L1 核心记忆文件、L2 会话摘要（滚动 50 条）、L3 档案库；
// 检索 = FTS5 trigram（短词 LIKE 兜底）+ 语义向量（M6-A，RRF 混合）→ 综合评分 → Top5 → access_count 回写
#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QPair>
#include <QVector>
#include <QString>
#include <QStringList>

namespace miderforge {

class Database;
class EmbeddingClient;

// FTS5 查询词构造（纯函数，可单测）。MATCH 右侧是 FTS5 自己的查询语法，不是纯文本：
// 裸绑用户文本会被当语法解析，C++/CMake 项目里最常见的词元全部报错并静默零召回——
//   'CMakeLists.txt' -> syntax error near "."   'C++20' -> syntax error near "+"
//   'Qt6::Widgets'   -> no such column: Qt6     'utf-8' -> no such column: 8
// 双引号包裹成短语后按字面量匹配（内部 " 按 FTS5 规则翻倍转义），以上全部正常命中。
namespace ftsquery {

// 单个词 → 带引号短语；空/全空白返回空串
QString quoteTerm(const QString& raw);
// 多个词 → "a" OR "b" OR …；跳过空项，全空返回空串
QString orTerms(const QStringList& terms);

} // namespace ftsquery

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
    // 用户手改保护（规格 6）：Agent 自动改写前必须检查；Agent 自己的写入不算用户编辑。
    // 判定依据是持久化的"Agent 上次写入状态"（core.md.agent-state.json：内容 SHA-256 + 写入时刻），
    // 跨进程有效——此前只比对内存里的 mtime，重启后基准丢失导致任何已存在的 core.md 都被判为
    // "用户手建"而永久拒绝改写，L1 自动改写从未真正运行过。
    bool l1UserEditedRecently() const;
    // 记录"本次写入的基准"（内容 SHA-256 + 时刻）并落盘，跨进程有效。
    // saveL1 内部自动调用；用户在记忆视图手改后也需调用一次：手改本身是用户行为，
    // 但它给出了新的已确认基准，否则保护逻辑永远无法确认"之后没人再改过"。
    void recordWriteBaseline(const QString& content) const;

    // ---- L3 档案库 ----
    // status：'active'（默认）/ 'pending'（Hermes write_approval 审批门：待人工批准）
    qint64 addMemory(const QString& type, const QString& content, double importance,
                     const QString& status = QStringLiteral("active"));
    // Hermes 查重：完全相同的 type+content（active/pending）视为已存在
    bool hasMemory(const QString& type, const QString& content) const;
    // 审批门（write_approval）：pending → active（批准）/ archived（拒绝）
    bool approveMemory(qint64 id);
    bool rejectMemory(qint64 id);
    // Hermes 同款 L1 提示词渲染：用量头部 + 条目 § 分隔（纯函数，可单测）
    static QString renderL1ForPrompt(const QString& l1, long long tokens, long long cap);
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
    // 短词（<3 字符）自动降级 LIKE 全扫（必须有单测覆盖）；
    // 注入嵌入器后走 FTS5 + 向量余弦双通道 RRF 混合排序（M6-A），嵌入失败静默降级纯 FTS5
    QVector<MemoryRecord> retrieve(const QString& rawQuery, int topN = 5);

    // ---- 语义检索（M6-A） ----
    // 注入嵌入器（空指针 = 纯 FTS5；单测注入确定性桩）。不转移所有权
    void setEmbedder(EmbeddingClient* e) { m_embedder = e; }
    // 为缺向量的 active 记忆补嵌入（每次至多 maxBatch 条；阻塞网络，须在可阻塞线程调用）；返回补齐条数
    int backfillEmbeddings(int maxBatch = 8);
    // 向量序列化（float32 小端字节数组，存 memories.embedding BLOB）与余弦相似度（暴露给单测）：
    // 零向量 → 0；维度不一致（换嵌入模型后的存量向量）→ NaN
    static QByteArray packVector(const QVector<float>& v);
    static QVector<float> unpackVector(const QByteArray& blob);
    static double cosineSim(const QVector<float>& a, const QVector<float>& b);
    // RRF 融合（暴露给单测）：两路按序 id 列表（优→劣），得分 Σ 1/(k+rank)（rank 从 0 计），按分降序返回
    static QVector<QPair<qint64, double>> fuseRRF(const QVector<qint64>& a,
                                                  const QVector<qint64>& b, int k = 60);

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

    // ---- L1 Agent 写入状态（core.md.agent-state.json，跨进程持久化） ----
    // 记录基准的内容 SHA-256 与写入时刻。判定语义：
    //   当前内容哈希 == 记录哈希      → 是基准写入者自己写的，允许下次改写
    //   哈希不同且文件 mtime 晚于记录时刻 → 之后有人手改，拒绝改写（保护）
    //   mtime 更早或相等（时钟回拨/外部还原）→ 按手改保护处理
    bool loadAgentWriteState() const; // 填充 m_lastAgentWrite/m_lastAgentWriteHash，缺文件返回 false

    Database* m_db = nullptr;
    QString m_l1Path;
    EmbeddingClient* m_embedder = nullptr; // M6-A 语义检索；nullptr = 纯 FTS5
    int m_embedFailStreak = 0; // 嵌入连续失败计数（≥3 熔断本进程语义通道，防断网时每轮检索白等超时）
    mutable QDateTime m_lastAgentWrite;    // 基准写入时刻（每次判定从状态文件重读）
    mutable QString m_lastAgentWriteHash;  // 基准内容 SHA-256（区分"基准写入者写的"与"之后被改的"）
};

} // namespace miderforge
