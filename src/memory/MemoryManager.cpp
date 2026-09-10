// 记忆管理实现
#include "memory/MemoryManager.h"
#include "db/Database.h"
#include "llm/EmbeddingClient.h"
#include "util/Log.h"
#include "util/Tokens.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace miderforge {

namespace {

// LIKE 通配符转义：% 与 _ 是用户内容里的常见字符（"100%"、"a_c"），不转义会被当通配符，
// 查询行为错乱（虽非注入，但召回全错）；配合 SQL 的 ESCAPE '\' 子句使用
QString likeEscape(QString word) {
    word.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    word.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    word.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return word;
}

// 嵌入连续失败熔断阈值：连续 3 次失败即认定网络/服务不可用，本进程内停用语义通道，
// 只走 FTS5。否则断网时每轮检索都要白等 10s 连接超时（backfill + 查询各一次）
constexpr int kMaxEmbedFailStreak = 3;

} // namespace

MemoryManager::MemoryManager(Database* db, const QString& l1Path)
    : m_db(db), m_l1Path(l1Path) {}

QString MemoryManager::loadL1() const {
    QFile f(m_l1Path);
    if (!f.open(QIODevice::ReadOnly))
        return QString(); // 文件不存在=空 L1（首次运行）
    return QString::fromUtf8(f.readAll());
}

bool MemoryManager::saveL1(const QString& content) const {
    QDir().mkpath(QFileInfo(m_l1Path).absolutePath());
    QFile f(m_l1Path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = f.write(content.toUtf8()) >= 0; // 全链路 UTF-8
    f.close();
    if (ok)
        m_lastAgentWrite = QFileInfo(m_l1Path).lastModified(); // 登记 Agent 写入
    return ok;
}

long long MemoryManager::l1Tokens() const {
    return tokens::estimate(loadL1());
}

bool MemoryManager::l1UserEditedRecently() const {
    // 决策: 记录 Agent 最近一次写入的文件 mtime；文件 mtime 与之不同且距 Agent 写入 <24h
    // 视为用户手改（Agent 写入由 saveL1 统一登记）
    QFileInfo info(m_l1Path);
    if (!info.exists())
        return false;
    const QDateTime mtime = info.lastModified();
    if (m_lastAgentWrite.isValid() && mtime == m_lastAgentWrite)
        return false; // 就是 Agent 自己写的
    if (!m_lastAgentWrite.isValid())
        return true; // Agent 从未写过而文件存在：用户手建，保护
    return m_lastAgentWrite.secsTo(mtime) != 0 && mtime > m_lastAgentWrite.addSecs(-1);
}

qint64 MemoryManager::addMemory(const QString& type, const QString& content, double importance,
                                const QString& status) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 id = 0;
    m_db->executeInsert(
        QStringLiteral("INSERT INTO memories(type, content, importance, status, created_at, updated_at) "
                       "VALUES(?,?,?,?,?,?)"),
        {type, content, importance, status.isEmpty() ? QStringLiteral("active") : status, now, now}, &id);
    return id;
}

bool MemoryManager::hasMemory(const QString& type, const QString& content) const {
    // Hermes 查重语义：完全相同的 type+content（active/pending 视为存在）拒绝重复添加
    const auto rows = m_db->query(QStringLiteral(
        "SELECT id FROM memories WHERE type=? AND content=? AND status IN ('active','pending') LIMIT 1"),
        {type, content});
    return !rows.empty();
}

bool MemoryManager::approveMemory(qint64 id) {
    return m_db->execute(QStringLiteral(
        "UPDATE memories SET status='active', updated_at=? WHERE id=? AND status='pending'"),
        {QDateTime::currentSecsSinceEpoch(), id});
}

bool MemoryManager::rejectMemory(qint64 id) {
    return m_db->execute(QStringLiteral(
        "UPDATE memories SET status='archived', updated_at=? WHERE id=? AND status='pending'"),
        {QDateTime::currentSecsSinceEpoch(), id});
}

QString MemoryManager::renderL1ForPrompt(const QString& l1, long long tokens, long long cap) {
    // Hermes 同款渲染：用量头部 + 条目以 § 分隔（每个非空行视作一条）
    if (l1.trimmed().isEmpty())
        return QStringLiteral("（L1 核心记忆为空）");
    const int pct = cap > 0 ? int(tokens * 100 / cap) : 0;
    QString body;
    const QStringList lines = l1.split(QLatin1Char('\n'));
    bool first = true;
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (!first)
            body += QStringLiteral("\n§ ");
        else
            first = false;
        body += line;
    }
    return QStringLiteral("L1 核心记忆（%1/%2 tokens，占用 %3%）\n§ %4")
        .arg(tokens)
        .arg(cap)
        .arg(pct)
        .arg(body);
}

bool MemoryManager::archiveMemory(qint64 id) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    return m_db->execute(QStringLiteral("UPDATE memories SET status='archived', updated_at=? WHERE id=?"),
                         {now, id});
}

bool MemoryManager::updateContent(qint64 id, const QString& content) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // 内容变更即失效旧向量（存储体系 write-through invalidation 语义）：
    // 旧嵌入描述的是旧文本，留着会持续污染语义召回；置 NULL 由 backfillEmbeddings 用新文本重嵌
    return m_db->execute(QStringLiteral("UPDATE memories SET content=?, embedding=NULL, updated_at=? WHERE id=?"),
                         {content, now, id});
}

bool MemoryManager::recordAccess(qint64 id) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    return m_db->execute(QStringLiteral(
        "UPDATE memories SET access_count=access_count+1, last_accessed_at=? WHERE id=?"),
        {now, id});
}

QVector<MemoryManager::MemoryRecord> MemoryManager::listAll(const QString& typeFilter) const {
    QString sql = QStringLiteral("SELECT id,type,content,importance,status,created_at,updated_at,"
                                 "access_count,last_accessed_at FROM memories");
    QVariantList binds;
    if (!typeFilter.isEmpty()) {
        sql += QStringLiteral(" WHERE type=?");
        binds << typeFilter;
    }
    sql += QStringLiteral(" ORDER BY updated_at DESC");
    return selectBySql(sql, binds);
}

QVector<MemoryManager::MemoryRecord> MemoryManager::selectBySql(const QString& sql,
                                                                const QVariantList& binds) const {
    QVector<MemoryRecord> out;
    for (const auto& row : m_db->query(sql, binds)) {
        MemoryRecord r;
        r.id = row.value("id").toLongLong();
        r.type = row.value("type").toString();
        r.content = row.value("content").toString();
        r.importance = row.value("importance").toDouble();
        r.status = row.value("status").toString();
        r.createdAt = row.value("created_at").toLongLong();
        r.updatedAt = row.value("updated_at").toLongLong();
        r.accessCount = row.value("access_count").toLongLong();
        r.lastAccessedAt = row.value("last_accessed_at").toLongLong();
        out.push_back(std::move(r));
    }
    return out;
}

QVector<qint64> MemoryManager::filterSupersededIds(const QJsonObject& parsed,
                                                   const QVector<qint64>& injectedIds) {
    QVector<qint64> out;
    const auto arr = parsed.value(QStringLiteral("superseded_memory_ids")).toArray();
    for (const auto& v : arr) {
        bool ok = false;
        const qint64 id = v.toVariant().toLongLong(&ok); // 数字与数字字符串都接受
        if (!ok || id <= 0)
            continue;
        if (!injectedIds.contains(id))
            continue; // 只允许失效本次真正注入过的条目，杜绝幻觉编号误伤
        if (!out.contains(id))
            out.push_back(id);
    }
    return out;
}

int MemoryManager::archiveMemories(const QVector<qint64>& ids) {
    if (ids.isEmpty())
        return 0;
    QStringList ph;
    QVariantList binds;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    binds << now; // 注意：SQL 第一个 ? 是 updated_at，其后才是 IN 列表（漏绑会让 NULL 落进 IN）
    for (qint64 id : ids) {
        ph << QStringLiteral("?");
        binds << id;
    }
    const bool ok = m_db->execute(QStringLiteral(
        "UPDATE memories SET status='archived', updated_at=? WHERE id IN (%1)")
        .arg(ph.join(QLatin1Char(','))), binds);
    return ok ? ids.size() : 0;
}

double MemoryManager::score(double bm25Rank, double importance, qint64 updatedAt,
                            qint64 nowSecs, qint64 accessCount) {
    // BM25：fts5 的 bm25() 越小越相关（负值），归一化到 (0,1]
    const double bm25 = 1.0 / (1.0 + (bm25Rank < 0 ? -bm25Rank : bm25Rank));
    // 时间衰减：半衰期 30 天
    const double ageDays = double(qMax<long long>(0, nowSecs - updatedAt)) / 86400.0;
    const double decay = std::pow(0.5, ageDays / 30.0);
    const double access = std::log10(double(accessCount) + 1.0);
    return 0.5 * bm25 + 0.2 * importance + 0.2 * decay + 0.1 * access;
}

QByteArray MemoryManager::packVector(const QVector<float>& v) {
    QByteArray blob(v.size() * int(sizeof(float)), Qt::Uninitialized);
    if (!v.isEmpty())
        std::memcpy(blob.data(), v.constData(), size_t(v.size()) * sizeof(float));
    return blob;
}

QVector<float> MemoryManager::unpackVector(const QByteArray& blob) {
    QVector<float> out(blob.size() / int(sizeof(float)), 0.0f);
    if (!out.isEmpty())
        std::memcpy(out.data(), blob.constData(), size_t(out.size()) * sizeof(float));
    return out;
}

double MemoryManager::cosineSim(const QVector<float>& a, const QVector<float>& b) {
    if (a.size() != b.size())
        return std::numeric_limits<double>::quiet_NaN(); // 换嵌入模型后的存量向量维度不一致
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < a.size(); ++i) {
        dot += double(a[i]) * b[i];
        na += double(a[i]) * a[i];
        nb += double(b[i]) * b[i];
    }
    if (na <= 0.0 || nb <= 0.0)
        return 0.0; // 零向量无方向
    return dot / std::sqrt(na * nb);
}

QVector<QPair<qint64, double>> MemoryManager::fuseRRF(const QVector<qint64>& a,
                                                      const QVector<qint64>& b, int k) {
    QHash<qint64, double> scored;
    const auto add = [&scored, k](const QVector<qint64>& ids) {
        for (int r = 0; r < ids.size(); ++r)
            scored[ids[r]] += 1.0 / (double(k) + double(r)); // rank 从 0 计（Cormack RRF 惯例）
    };
    add(a);
    add(b);
    QVector<QPair<qint64, double>> out;
    out.reserve(scored.size());
    for (auto it = scored.constBegin(); it != scored.constEnd(); ++it)
        out.append({it.key(), it.value()});
    std::sort(out.begin(), out.end(),
              [](const QPair<qint64, double>& x, const QPair<qint64, double>& y) {
                  // 同分按 id 升序决胜：std::sort 不稳定 + QHash 迭代序随机化，
                  // 不加决胜会导致同分名次跨进程摇摆（纯函数必须输出确定）
                  if (x.second != y.second)
                      return x.second > y.second;
                  return x.first < y.first;
              });
    return out;
}

int MemoryManager::backfillEmbeddings(int maxBatch) {
    if (!m_embedder || !m_embedder->valid() || maxBatch <= 0)
        return 0;
    if (m_embedFailStreak >= kMaxEmbedFailStreak)
        return 0; // 已熔断：本进程不再尝试嵌入
    const auto rows = m_db->query(QStringLiteral(
        "SELECT id, content FROM memories WHERE status='active' AND embedding IS NULL "
        "ORDER BY updated_at DESC LIMIT ?"), {maxBatch});
    if (rows.empty())
        return 0;
    QStringList texts;
    QVector<qint64> ids;
    texts.reserve(rows.size());
    for (const auto& row : rows) {
        ids << row.value("id").toLongLong();
        texts << row.value("content").toString();
    }
    QVector<QVector<float>> vecs;
    QString err;
    if (!m_embedder->embed(texts, &vecs, &err)) {
        ++m_embedFailStreak;
        if (auto lg = logutil::logger()) {
            lg->warn("记忆补嵌入失败（连续第 {} 次）：{}", m_embedFailStreak, err.toStdString());
            if (m_embedFailStreak >= kMaxEmbedFailStreak)
                lg->warn("嵌入连续失败 {} 次，本进程熔断语义通道，检索仅走 FTS5", m_embedFailStreak);
        }
        return 0;
    }
    m_embedFailStreak = 0; // 成功即清零，偶发一次网络抖动不会触发熔断
    if (vecs.size() != ids.size()) {
        if (auto lg = logutil::logger())
            lg->warn("记忆补嵌入条数对不上：请求 {} 返回 {}", ids.size(), vecs.size());
        return 0;
    }
    int n = 0;
    for (int i = 0; i < ids.size(); ++i) {
        if (vecs[i].isEmpty())
            continue; // 响应缺项：留 NULL 下轮重嵌
        if (m_db->execute(QStringLiteral("UPDATE memories SET embedding=? WHERE id=?"),
                          {packVector(vecs[i]), ids[i]}))
            ++n;
    }
    return n;
}

QVector<MemoryManager::MemoryRecord> MemoryManager::retrieve(const QString& rawQuery, int topN) {
    QVector<MemoryRecord> out;
    const QString q = rawQuery.trimmed();
    if (q.isEmpty())
        return out;

    // 短词兜底：trigram 分词要求 ≥3 字符，2 字及以下的词召回差 → LIKE 全扫（规格 6，附单测）
    QString shortestWord;
    const QStringList words = q.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString& w : words) {
        if (shortestWord.isEmpty() || w.length() < shortestWord.length())
            shortestWord = w;
    }
    if (shortestWord.length() < 3) {
        // LIKE 兜底（content LIKE '%词%'，active 过滤），按 importance+时间排序。
        // 所有词 AND 逐个匹配（多词查询只取最短词会漏召回），通配符已转义
        QStringList conds;
        QVariantList binds;
        for (const QString& w : words) {
            conds << QStringLiteral("content LIKE ? ESCAPE '\\'");
            binds << QStringLiteral("%1%2%1").arg(QStringLiteral("%"), likeEscape(w));
        }
        binds << topN;
        QVector<MemoryRecord> hits = selectBySql(QStringLiteral(
            "SELECT id,type,content,importance,status,created_at,updated_at,access_count,last_accessed_at "
            "FROM memories WHERE status='active' AND (%1) "
            "ORDER BY importance DESC, updated_at DESC LIMIT ?").arg(conds.join(QStringLiteral(" AND "))),
            binds);
        for (const auto& h : hits)
            recordAccess(h.id);
        return hits;
    }

    // ---- 通道 A：FTS5 逐词 MATCH（bm25 排序）+ 综合评分（原有语义保留） ----
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    struct Candidate { MemoryRecord rec; double s; };
    std::vector<Candidate> cands;
    for (const QString& w : words) {
        if (w.length() < 3)
            continue;
        QString escaped = w;
        escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        const QString sql = QStringLiteral(
            "SELECT m.id,m.type,m.content,m.importance,m.status,m.created_at,m.updated_at,"
            "m.access_count,m.last_accessed_at, rank "
            "FROM memories_fts f JOIN memories m ON m.id=f.rowid "
            "WHERE memories_fts MATCH ? AND m.status='active' "
            "ORDER BY rank LIMIT 20");
        for (const auto& row : m_db->query(sql, {escaped})) {
            MemoryRecord r;
            r.id = row.value("id").toLongLong();
            r.type = row.value("type").toString();
            r.content = row.value("content").toString();
            r.importance = row.value("importance").toDouble();
            r.status = row.value("status").toString();
            r.createdAt = row.value("created_at").toLongLong();
            r.updatedAt = row.value("updated_at").toLongLong();
            r.accessCount = row.value("access_count").toLongLong();
            r.lastAccessedAt = row.value("last_accessed_at").toLongLong();
            const double s = score(row.value("rank").toDouble(), r.importance, r.updatedAt, now, r.accessCount);
            bool dup = false;
            for (const auto& c : cands)
                if (c.rec.id == r.id) { dup = true; break; }
            if (!dup)
                cands.push_back({std::move(r), s});
        }
    }
    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) { return a.s > b.s; });

    // ---- 通道 B：语义向量（M6-A）。查询向量余弦召回，词面不同语义近的记忆由此补齐 ----
    QVector<qint64> ftsIds;
    QHash<qint64, MemoryRecord> recs;
    for (const auto& c : cands) {
        ftsIds << c.rec.id;
        recs.insert(c.rec.id, c.rec);
    }
    QVector<qint64> vecIds;
    if (m_embedder && m_embedder->valid() && m_embedFailStreak < kMaxEmbedFailStreak) {
        backfillEmbeddings(8); // 渐进补齐缺向量条目；失败静默（FTS 主路不受影响）
        QVector<QVector<float>> qv;
        QString eerr;
        if (m_embedder->embed({q}, &qv, &eerr) && qv.size() == 1 && !qv.front().isEmpty()) {
            m_embedFailStreak = 0;
            struct VecHit { qint64 id; double sim; };
            std::vector<VecHit> hits;
            for (const auto& row : m_db->query(QStringLiteral(
                    "SELECT id,type,content,importance,status,created_at,updated_at,access_count,"
                    "last_accessed_at,embedding FROM memories WHERE status='active' AND embedding IS NOT NULL"))) {
                MemoryRecord r;
                r.id = row.value("id").toLongLong();
                r.type = row.value("type").toString();
                r.content = row.value("content").toString();
                r.importance = row.value("importance").toDouble();
                r.status = row.value("status").toString();
                r.createdAt = row.value("created_at").toLongLong();
                r.updatedAt = row.value("updated_at").toLongLong();
                r.accessCount = row.value("access_count").toLongLong();
                r.lastAccessedAt = row.value("last_accessed_at").toLongLong();
                const double sim = cosineSim(qv.front(), unpackVector(row.value("embedding").toByteArray()));
                if (std::isnan(sim) || sim <= 0.0)
                    continue; // 维度不一致（换模型存量）/零向量/负相关一律不召回
                hits.push_back({r.id, sim});
                recs.insert(r.id, std::move(r)); // 语义独有命中也要能取出完整记录
            }
            std::sort(hits.begin(), hits.end(),
                      [](const VecHit& x, const VecHit& y) { return x.sim > y.sim; });
            for (int i = 0; i < int(hits.size()) && i < 20; ++i)
                vecIds << hits[i].id;
        } else {
            ++m_embedFailStreak; // 调用失败或返回不可用向量，同样计入熔断
            if (auto lg = logutil::logger())
                lg->warn("查询嵌入失败（连续第 {} 次），本次检索降级纯 FTS5：{}",
                         m_embedFailStreak, eerr.toStdString());
        }
    }

    if (vecIds.isEmpty()) {
        // 无语义通道：保持原有纯 FTS5 行为（回归红线，附带单测覆盖）
        for (int i = 0; i < int(cands.size()) && i < topN; ++i) {
            recordAccess(cands[i].rec.id); // 命中即计数（规格 6）
            out.push_back(cands[i].rec);
        }
        return out;
    }

    // ---- RRF 融合双通道：两路排名互不可比（bm25 分数 vs 余弦值），只融合名次最稳 ----
    const auto fused = fuseRRF(ftsIds, vecIds);
    for (int i = 0; i < fused.size() && i < topN; ++i) {
        const auto it = recs.constFind(fused[i].first);
        if (it == recs.constEnd())
            continue;
        recordAccess(it->id);
        out.push_back(*it);
    }
    return out;
}

double MemoryManager::trigramDice(const QString& a, const QString& b) {
    // 字符三元组 Dice：中文友好（无需分词），"高度相似但不相同"在 0.35~0.95 区间信号最强
    const auto grams = [](const QString& s) {
        QSet<QString> out;
        const QString t = s.simplified();
        for (int i = 0; i + 3 <= t.size(); ++i)
            out.insert(t.mid(i, 3));
        return out;
    };
    const QSet<QString> ga = grams(a);
    const QSet<QString> gb = grams(b);
    if (ga.isEmpty() || gb.isEmpty())
        return 0.0;
    int shared = 0;
    for (const QString& g : ga)
        if (gb.contains(g))
            ++shared;
    return 2.0 * shared / double(ga.size() + gb.size());
}

QVector<MemoryManager::ContradictionPair> MemoryManager::findContradictionCandidates(int maxPairs) const {
    constexpr double kMinSim = 0.35; // 低于此视为无关
    constexpr double kMaxSim = 0.95; // 高于此视为冗余副本而非矛盾
    constexpr int kScanLimit = 200;  // 每类型扫描上限（按钮触发的一次性 O(n²) 可接受）

    const auto facts = selectBySql(QStringLiteral(
        "SELECT id,type,content FROM memories WHERE status='active' "
        "AND type IN ('project_facts','task_lesson','coding_pref','user_profile') "
        "ORDER BY updated_at DESC LIMIT ?"), {kScanLimit});

    QVector<ContradictionPair> out;
    for (int i = 0; i < facts.size(); ++i) {
        for (int j = i + 1; j < facts.size(); ++j) {
            if (facts[i].content == facts[j].content)
                continue;
            const double sim = trigramDice(facts[i].content, facts[j].content);
            if (sim < kMinSim || sim > kMaxSim)
                continue;
            out.push_back({facts[i].id, facts[j].id, facts[i].content, facts[j].content,
                           facts[i].type, sim});
            if (out.size() >= maxPairs)
                return out;
        }
    }
    std::sort(out.begin(), out.end(), [](const ContradictionPair& a, const ContradictionPair& b) {
        return a.similarity > b.similarity;
    });
    return out;
}

qint64 MemoryManager::addSessionSummary(const QString& content) {
    const qint64 id = addMemory(QStringLiteral("session_summary"), content, 0.6);
    // 淘汰评分化（M4.5 后续项）：50 条 hot 名额按 importance×时间衰减 评分保留，
    // 落选者降权至 0.2（降权单调不回升：0.2 者不参与 hot 竞争）。
    // 评分公式与 L3 综合评分同源（半衰期 30 天）；分数打平时按 updated_at/id 降序决胜，
    // 退化为 FIFO（保最新），与旧语义兼容。单条 UPDATE … IN (子查询)：隐式事务、只写溢出条目
    m_db->execute(QStringLiteral(
        "UPDATE memories SET importance=0.2 WHERE type='session_summary' AND status='active' "
        "AND importance > 0.2 AND id NOT IN ("
        "SELECT id FROM memories WHERE type='session_summary' AND status='active' AND importance > 0.2 "
        "ORDER BY importance * pow(0.5, (strftime('%s','now') - updated_at) / 2592000.0) DESC, "
        "updated_at DESC, id DESC LIMIT 50)"), {});
    return id;
}

QVector<MemoryManager::MemoryRecord> MemoryManager::recentSummaries(int n) const {
    return selectBySql(QStringLiteral(
        "SELECT id,type,content,importance,status,created_at,updated_at,access_count,last_accessed_at "
        "FROM memories WHERE type='session_summary' AND status='active' "
        "ORDER BY updated_at DESC, id DESC LIMIT ?"), {n});
}

} // namespace miderforge
