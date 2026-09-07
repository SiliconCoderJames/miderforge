// SQLite 数据库：打开/迁移（四表+FTS5+触发器）/WAL/写串行化；events 为 append-only（代码中禁 UPDATE/DELETE）
#pragma once
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <sqlite3.h>
#include <vector>

namespace miderforge {

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 打开并迁移（幂等）；失败返回 false 并给出错误文本
    bool open(const QString& path, QString* error = nullptr);
    void close();
    bool isOpen() const { return m_db != nullptr; }

    // ---- 查询（读路径） ----
    // 返回每行为 列名→值；错误时返回空并设置 lastError
    std::vector<QVariantMap> query(const QString& sql, const QVariantList& binds = {});

    // ---- 执行（写路径，互斥串行化；busy_timeout 5s + WAL） ----
    bool execute(const QString& sql, const QVariantList& binds = {});
    // 返回 last_insert_rowid
    bool executeInsert(const QString& sql, const QVariantList& binds, qint64* rowId = nullptr);

    QString lastError() const { return m_lastError; }

    // 便捷：加载 sqlite-vec 扩展（v1 仅要求加载成功，不使用向量检索）
    bool loadVecExtension(QString* error = nullptr);

private:
    bool migrate();

    sqlite3* m_db = nullptr;
    QMutex m_writeMutex; // 写串行化（规格：一个写线程或 mutex）
    QString m_lastError;
};

} // namespace miderforge
