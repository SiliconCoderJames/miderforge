// 记忆管理实现
#include "memory/MemoryManager.h"
#include "db/Database.h"
#include "util/Log.h"
#include "util/Tokens.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

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

qint64 MemoryManager::addMemory(const QString& type, const QString& content, double importance) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 id = 0;
    m_db->executeInsert(
        QStringLiteral("INSERT INTO memories(type, content, importance, status, created_at, updated_at) "
                       "VALUES(?,?,?, 'active', ?, ?)"),
        {type, content, importance, now, now}, &id);
    return id;
}

bool MemoryManager::archiveMemory(qint64 id) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    return m_db->execute(QStringLiteral("UPDATE memories SET status='archived', updated_at=? WHERE id=?"),
                         {now, id});
}

bool MemoryManager::updateContent(qint64 id, const QString& content) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    return m_db->execute(QStringLiteral("UPDATE memories SET content=?, updated_at=? WHERE id=?"),
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

    // FTS5 路径：逐词 MATCH 取候选（bm25 排序），再在应用层做综合评分
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
    for (int i = 0; i < int(cands.size()) && i < topN; ++i) {
        recordAccess(cands[i].rec.id); // 命中即计数（规格 6）
        out.push_back(cands[i].rec);
    }
    return out;
}

qint64 MemoryManager::addSessionSummary(const QString& content) {
    const qint64 id = addMemory(QStringLiteral("session_summary"), content, 0.6);
    // FIFO 上限 50：超出时最旧摘要降 importance（规格 6：合并进 L3 或降 importance；v1 取降级）。
    // 同秒内 created_at 会打平（时间统一 unix 秒），必须加 id 决胜保证稳定顺序。
    // 单条 UPDATE … IN (子查询)：隐式事务、只写溢出条目——逐条 UPDATE 是 O(n) 全表写且未事务化，
    // 中途崩溃会留下"部分降级"的不一致
    m_db->execute(QStringLiteral(
        "UPDATE memories SET importance=0.2 WHERE type='session_summary' AND id IN ("
        "SELECT id FROM memories WHERE type='session_summary' "
        "ORDER BY updated_at ASC, id ASC LIMIT -1 OFFSET 50)"), {});
    return id;
}

QVector<MemoryManager::MemoryRecord> MemoryManager::recentSummaries(int n) const {
    return selectBySql(QStringLiteral(
        "SELECT id,type,content,importance,status,created_at,updated_at,access_count,last_accessed_at "
        "FROM memories WHERE type='session_summary' AND status='active' "
        "ORDER BY updated_at DESC, id DESC LIMIT ?"), {n});
}

} // namespace miderforge
