// SQLite 数据库实现
#include "db/Database.h"
#include "util/Log.h"
#include <QDir>
#include <QFileInfo>
#include <spdlog/spdlog.h>

// sqlite-vec 静态编译（SQLITE_CORE + SQLITE_VEC_STATIC），直接注册 vec0 虚表模块；
// v1 仅要求加载成功（规格 2），不使用向量检索
extern "C" int sqlite3_vec_init(sqlite3* db, char** pzErrMsg, const sqlite3_api_routines* pApi);

namespace miderforge {

Database::~Database() {
    close();
}

void Database::close() {
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

bool Database::open(const QString& path, QString* error) {
    close();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const int rc = sqlite3_open_v2(path.toUtf8().constData(), &m_db,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
        if (error)
            *error = QStringLiteral("sqlite 打开失败: %1").arg(m_db ? sqlite3_errmsg(m_db) : "unknown");
        close();
        return false;
    }
    // 规格硬约束：WAL + busy_timeout 5s
    char* errMsg = nullptr;
    sqlite3_exec(m_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errMsg);
    sqlite3_exec(m_db, "PRAGMA busy_timeout=5000;", nullptr, nullptr, &errMsg);
    sqlite3_exec(m_db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, &errMsg);
    if (errMsg)
        sqlite3_free(errMsg);

    if (!migrate()) {
        if (error)
            *error = m_lastError;
        close();
        return false;
    }
    if (auto lg = logutil::logger())
        lg->info("数据库已打开：{}", path.toStdString());
    return true;
}

bool Database::migrate() {
    // ---- memories（L3 档案记忆；embedding v1 恒 NULL 预留） ----
    const char* kDdl[] = {
        R"(CREATE TABLE IF NOT EXISTS memories (
            id INTEGER PRIMARY KEY,
            type TEXT NOT NULL,
            content TEXT NOT NULL,
            importance REAL NOT NULL DEFAULT 0.5,
            status TEXT NOT NULL DEFAULT 'active',
            embedding BLOB,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL,
            access_count INTEGER NOT NULL DEFAULT 0,
            last_accessed_at INTEGER
        ))",
        // FTS5 external content 模式 + trigram 分词（中文检索核心）
        R"(CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5(
            content, content='memories', content_rowid='id', tokenize='trigram'
        ))",
        // 同步触发器三件套（standard external content 模式）
        R"(CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN
            INSERT INTO memories_fts(rowid, content) VALUES (new.id, new.content);
        END)",
        R"(CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN
            INSERT INTO memories_fts(memories_fts, rowid, content) VALUES ('delete', old.id, old.content);
        END)",
        R"(CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE ON memories BEGIN
            INSERT INTO memories_fts(memories_fts, rowid, content) VALUES ('delete', old.id, old.content);
            INSERT INTO memories_fts(rowid, content) VALUES (new.id, new.content);
        END)",
        // ---- skills（决策: 规格基础上补 version 列，支撑同名 merge version+1） ----
        R"(CREATE TABLE IF NOT EXISTS skills (
            id INTEGER PRIMARY KEY,
            name TEXT NOT NULL UNIQUE,
            description TEXT NOT NULL,
            path TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'active',
            usage_count INTEGER NOT NULL DEFAULT 0,
            success_count INTEGER NOT NULL DEFAULT 0,
            avg_rounds REAL,
            version INTEGER NOT NULL DEFAULT 1,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        ))",
        R"(CREATE VIRTUAL TABLE IF NOT EXISTS skills_fts USING fts5(
            name, description, content='skills', content_rowid='id', tokenize='trigram'
        ))",
        R"(CREATE TRIGGER IF NOT EXISTS skills_ai AFTER INSERT ON skills BEGIN
            INSERT INTO skills_fts(rowid, name, description) VALUES (new.id, new.name, new.description);
        END)",
        R"(CREATE TRIGGER IF NOT EXISTS skills_ad AFTER DELETE ON skills BEGIN
            INSERT INTO skills_fts(skills_fts, rowid, name, description) VALUES ('delete', old.id, old.name, old.description);
        END)",
        R"(CREATE TRIGGER IF NOT EXISTS skills_au AFTER UPDATE ON skills BEGIN
            INSERT INTO skills_fts(skills_fts, rowid, name, description) VALUES ('delete', old.id, old.name, old.description);
            INSERT INTO skills_fts(rowid, name, description) VALUES (new.id, new.name, new.description);
        END)",
        // ---- tasks ----
        R"(CREATE TABLE IF NOT EXISTS tasks (
            id INTEGER PRIMARY KEY,
            session_id INTEGER,
            goal TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'queued',
            scheduled_at INTEGER,
            repeat_daily INTEGER DEFAULT 0,
            rounds_used INTEGER NOT NULL DEFAULT 0,
            tokens_in INTEGER NOT NULL DEFAULT 0,
            tokens_out INTEGER NOT NULL DEFAULT 0,
            result_summary TEXT,
            failure_reason TEXT,
            context_json TEXT,
            created_at INTEGER NOT NULL,
            finished_at INTEGER
        ))",
        // ---- events（append-only 审计流：代码中禁止任何 UPDATE/DELETE 路径） ----
        R"(CREATE TABLE IF NOT EXISTS events (
            id INTEGER PRIMARY KEY,
            ts INTEGER NOT NULL,
            type TEXT NOT NULL,
            task_id INTEGER,
            payload_json TEXT NOT NULL
        ))",
        "CREATE INDEX IF NOT EXISTS idx_events_task ON events(task_id, ts)",
        "CREATE INDEX IF NOT EXISTS idx_events_type_ts ON events(type, ts)",
    };
    for (const char* ddl : kDdl) {
        char* err = nullptr;
        if (sqlite3_exec(m_db, ddl, nullptr, nullptr, &err) != SQLITE_OK) {
            m_lastError = QString::fromUtf8(err ? err : "migrate failed");
            if (err)
                sqlite3_free(err);
            return false;
        }
    }
    // ---- 老库升级：CREATE TABLE IF NOT EXISTS 不会给已存在的表补列，这里按列名增量 ALTER ----
    // skills.version 是后补列（决策: 支撑同名 merge version+1）；缺失时 writeSkill 的 INSERT 会静默失败
    {
        sqlite3_stmt* stmt = nullptr;
        bool hasVersion = false;
        if (sqlite3_prepare_v2(m_db, "PRAGMA table_info(skills)", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* col = sqlite3_column_text(stmt, 1);
                if (col && QLatin1String(reinterpret_cast<const char*>(col)) == QLatin1String("version"))
                    hasVersion = true;
            }
            sqlite3_finalize(stmt);
        }
        if (!hasVersion) {
            char* err = nullptr;
            if (sqlite3_exec(m_db,
                             "ALTER TABLE skills ADD COLUMN version INTEGER NOT NULL DEFAULT 1",
                             nullptr, nullptr, &err) != SQLITE_OK) {
                m_lastError = QString::fromUtf8(err ? err : "skills.version 迁移失败");
                if (err)
                    sqlite3_free(err);
                return false;
            }
        }
    }
    // tasks.context_json 是 M5 后补列（决策: checkpoint 断点续跑——每轮末持久化 history/轮次/路由档，
    // 崩溃/退出后 Scheduler 重新入队时可从断点恢复而非从头重跑）
    {
        sqlite3_stmt* stmt = nullptr;
        bool hasCtx = false;
        if (sqlite3_prepare_v2(m_db, "PRAGMA table_info(tasks)", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* col = sqlite3_column_text(stmt, 1);
                if (col && QLatin1String(reinterpret_cast<const char*>(col)) == QLatin1String("context_json"))
                    hasCtx = true;
            }
            sqlite3_finalize(stmt);
        }
        if (!hasCtx) {
            char* err = nullptr;
            if (sqlite3_exec(m_db, "ALTER TABLE tasks ADD COLUMN context_json TEXT",
                             nullptr, nullptr, &err) != SQLITE_OK) {
                m_lastError = QString::fromUtf8(err ? err : "tasks.context_json 迁移失败");
                if (err)
                    sqlite3_free(err);
                return false;
            }
        }
    }
    return true;
}

bool Database::loadVecExtension(QString* error) {
    if (!m_db)
        return false;
    // SQLITE_VEC_STATIC：直接调用 init 注册 vec0 虚表模块（v1 仅要求加载成功，不使用）
    char* err = nullptr;
    const int rc = sqlite3_vec_init(m_db, &err, nullptr);
    if (rc != SQLITE_OK) {
        if (error)
            *error = QString::fromUtf8(err ? err : "sqlite-vec 加载失败");
        if (err)
            sqlite3_free(err);
        return false;
    }
    return true;
}

std::vector<QVariantMap> Database::query(const QString& sql, const QVariantList& binds) {
    std::vector<QVariantMap> rows;
    if (!m_db)
        return rows;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.toUtf8().constData(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return rows;
    }
    for (int i = 0; i < binds.size(); ++i) {
        const QVariant& v = binds.at(i);
        switch (v.typeId()) {
        case QMetaType::Int: case QMetaType::LongLong:
            sqlite3_bind_int64(stmt, i + 1, v.toLongLong());
            break;
        case QMetaType::Double:
            sqlite3_bind_double(stmt, i + 1, v.toDouble());
            break;
        case QMetaType::Bool:
            sqlite3_bind_int(stmt, i + 1, v.toBool() ? 1 : 0);
            break;
        default:
            sqlite3_bind_text(stmt, i + 1, v.toString().toUtf8().constData(), -1, SQLITE_TRANSIENT);
            break;
        }
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QVariantMap row;
        for (int c = 0; c < sqlite3_column_count(stmt); ++c) {
            const QString name = QString::fromUtf8(sqlite3_column_name(stmt, c));
            switch (sqlite3_column_type(stmt, c)) {
            case SQLITE_INTEGER: row.insert(name, sqlite3_column_int64(stmt, c)); break;
            case SQLITE_FLOAT: row.insert(name, sqlite3_column_double(stmt, c)); break;
            case SQLITE_NULL: row.insert(name, QVariant()); break;
            default: row.insert(name, QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, c)))); break;
            }
        }
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

bool Database::execute(const QString& sql, const QVariantList& binds) {
    return executeInsert(sql, binds, nullptr);
}

bool Database::executeInsert(const QString& sql, const QVariantList& binds, qint64* rowId) {
    if (!m_db)
        return false;
    QMutexLocker locker(&m_writeMutex); // 写串行化
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.toUtf8().constData(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    for (int i = 0; i < binds.size(); ++i) {
        const QVariant& v = binds.at(i);
        switch (v.typeId()) {
        case QMetaType::Int: case QMetaType::LongLong:
            sqlite3_bind_int64(stmt, i + 1, v.toLongLong());
            break;
        case QMetaType::Double:
            sqlite3_bind_double(stmt, i + 1, v.toDouble());
            break;
        case QMetaType::Bool:
            sqlite3_bind_int(stmt, i + 1, v.toBool() ? 1 : 0);
            break;
        default:
            sqlite3_bind_text(stmt, i + 1, v.toString().toUtf8().constData(), -1, SQLITE_TRANSIENT);
            break;
        }
    }
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        sqlite3_finalize(stmt);
        return false;
    }
    if (rowId)
        *rowId = sqlite3_last_insert_rowid(m_db);
    sqlite3_finalize(stmt);
    return true;
}

} // namespace miderforge
