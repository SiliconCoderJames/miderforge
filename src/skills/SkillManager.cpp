// 技能库管理实现
#include "skills/SkillManager.h"
#include "db/Database.h"
#include "util/Log.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace miderforge {

SkillManager::SkillManager(Database* db, const QString& skillsDir)
    : m_db(db), m_dir(skillsDir) {
    QDir().mkpath(m_dir);
}

bool SkillManager::parseFrontmatter(const QString& text, QString* name, QString* description,
                                    int* version, QString* body) {
    // 格式：---\nname: x\ndescription: y\nversion: n\n---\n正文
    static const QRegularExpression fence(QStringLiteral("\\A---\\s*\\n(.*?)\\n---\\s*\\n?"),
                                          QRegularExpression::DotMatchesEverythingOption);
    const auto m = fence.match(text);
    if (!m.hasMatch())
        return false;
    bool foundName = false, foundDesc = false;
    for (const QString& line : m.captured(1).split(QLatin1Char('\n'))) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const QString key = line.left(colon).trimmed();
        const QString val = line.mid(colon + 1).trimmed();
        if (key == QLatin1String("name") && !val.isEmpty()) {
            if (name) *name = val;
            foundName = true;
        } else if (key == QLatin1String("description") && !val.isEmpty()) {
            if (description) *description = val;
            foundDesc = true;
        } else if (key == QLatin1String("version")) {
            if (version) *version = val.toInt();
        }
    }
    if (body)
        *body = text.mid(m.capturedLength());
    return foundName && foundDesc;
}

QString SkillManager::serialize(const QString& name, const QString& description, int version,
                                const QString& body) {
    return QStringLiteral("---\nname: %1\ndescription: %2\nversion: %3\n---\n%4")
        .arg(name, description, QString::number(version), body);
}

static QString sanitizeSkillName(const QString& name) {
    QString out;
    for (const QChar ch : name) {
        if (ch.isLetterOrNumber() || ch == u'_' || ch == u'-')
            out += ch;
    }
    return out;
}

bool SkillManager::writeSkill(const QString& rawName, const QString& description,
                              const QString& body, QString* err, int* newVersion) {
    const QString name = sanitizeSkillName(rawName);
    if (name.isEmpty()) {
        if (err) *err = QStringLiteral("技能名不合法");
        return false;
    }
    // 同名 → merge：version+1，保留累计统计（规格 7）
    const SkillMeta existing = find(name);
    const int version = (existing.id > 0 ? existing.version : 0) + 1;

    const QString dirPath = m_dir + QLatin1Char('/') + name;
    if (!QDir().mkpath(dirPath)) {
        if (err) *err = QStringLiteral("技能目录创建失败");
        return false;
    }
    const QString path = dirPath + QStringLiteral("/SKILL.md");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QStringLiteral("SKILL.md 写入失败：%1").arg(f.errorString());
        return false;
    }
    f.write(serialize(name, description, version, body).toUtf8());
    f.close();

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (existing.id > 0) {
        m_db->execute(QStringLiteral(
            "UPDATE skills SET description=?, path=?, status='active', version=?, updated_at=? WHERE id=?"),
            {description, path, version, now, existing.id});
    } else {
        m_db->execute(QStringLiteral(
            "INSERT INTO skills(name, description, path, status, version, created_at, updated_at) "
            "VALUES(?,?,?, 'active', ?, ?, ?)"),
            {name, description, path, version, now, now});
    }
    if (newVersion)
        *newVersion = version;
    return true;
}

bool SkillManager::markDeprecated(const QString& name) {
    return m_db->execute(QStringLiteral("UPDATE skills SET status='deprecated' WHERE name=?"), {name});
}

SkillManager::SkillMeta SkillManager::find(const QString& name) const {
    for (const auto& row : m_db->query(QStringLiteral(
             "SELECT id,name,description,path,status,usage_count,success_count,avg_rounds,version "
             "FROM skills WHERE name=?"), {name})) {
        SkillMeta m;
        m.id = row.value("id").toLongLong();
        m.name = row.value("name").toString();
        m.description = row.value("description").toString();
        m.path = row.value("path").toString();
        m.status = row.value("status").toString();
        m.usageCount = row.value("usage_count").toInt();
        m.successCount = row.value("success_count").toInt();
        m.avgRounds = row.value("avg_rounds").toDouble();
        m.version = row.value("version").toInt();
        return m;
    }
    return {};
}

QVector<SkillManager::SkillMeta> SkillManager::listAll() const {
    QVector<SkillMeta> out;
    for (const auto& row : m_db->query(QStringLiteral(
             "SELECT id,name,description,path,status,usage_count,success_count,avg_rounds,version "
             "FROM skills ORDER BY usage_count DESC, name ASC"), {})) {
        SkillMeta m;
        m.id = row.value("id").toLongLong();
        m.name = row.value("name").toString();
        m.description = row.value("description").toString();
        m.path = row.value("path").toString();
        m.status = row.value("status").toString();
        m.usageCount = row.value("usage_count").toInt();
        m.successCount = row.value("success_count").toInt();
        m.avgRounds = row.value("avg_rounds").toDouble();
        m.version = row.value("version").toInt();
        out.push_back(std::move(m));
    }
    return out;
}

QVector<SkillManager::SkillMeta> SkillManager::search(const QString& query, int limit) const {
    QVector<SkillMeta> out;
    if (query.trimmed().isEmpty())
        return listAll();
    QString escaped = query.trimmed();
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    for (const auto& row : m_db->query(QStringLiteral(
             "SELECT s.id,s.name,s.description,s.path,s.status,s.usage_count,s.success_count,s.avg_rounds,s.version "
             "FROM skills_fts f JOIN skills s ON s.id=f.rowid "
             "WHERE skills_fts MATCH ? ORDER BY rank LIMIT ?"),
             {QStringLiteral("\"%1\"").arg(escaped), limit})) {
        SkillMeta m;
        m.id = row.value("id").toLongLong();
        m.name = row.value("name").toString();
        m.description = row.value("description").toString();
        m.path = row.value("path").toString();
        m.status = row.value("status").toString();
        m.usageCount = row.value("usage_count").toInt();
        m.successCount = row.value("success_count").toInt();
        m.avgRounds = row.value("avg_rounds").toDouble();
        m.version = row.value("version").toInt();
        out.push_back(std::move(m));
    }
    return out;
}

QString SkillManager::loadSkillMd(const QString& name) const {
    QFile f(m_dir + QLatin1Char('/') + sanitizeSkillName(name) + QStringLiteral("/SKILL.md"));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

void SkillManager::recordUsage(const QString& name, bool success, int rounds) {
    m_db->execute(QStringLiteral(
        "UPDATE skills SET usage_count=usage_count+1, "
        "success_count=success_count+?, "
        "avg_rounds=CASE WHEN avg_rounds IS NULL THEN ? ELSE (avg_rounds*usage_count+?)/(usage_count+1) END "
        "WHERE name=?"),
        {success ? 1 : 0, rounds, rounds, name});
    // 成功率 <30% 且使用 ≥5 → 自动标记待审查（不删除，规格 7）
    const SkillMeta m = find(name);
    if (m.id > 0 && m.usageCount >= 5 && m.successCount * 10 < m.usageCount * 3) {
        m_db->execute(QStringLiteral("UPDATE skills SET status='review' WHERE name=?"), {name});
        if (auto lg = logutil::logger())
            lg->warn("技能 {} 成功率过低，已标记待审查", name.toStdString());
    }
}

void SkillManager::ensureSeedSkill() {
    if (find(QStringLiteral("cpp-cmake-qt-build")).id > 0)
        return;
    const QString body = QStringLiteral(
        "# cpp-cmake-qt-build\n\n"
        "## 适用场景\n"
        "配置/构建/测试基于 CMake + Qt6 Widgets 的桌面工程（Windows + MSVC + vcpkg manifest）。\n\n"
        "## 执行步骤\n"
        "1. `cmake --preset win64`（vcpkg manifest 自动装依赖；Qt 套件路径见 CMakePresets.json 的 CMAKE_PREFIX_PATH）\n"
        "2. `cmake --build build --config Debug`（Release 需 --config Release）\n"
        "3. `ctest` 或直接运行 build/Debug/mider_tests.exe\n\n"
        "## 边界与坑\n"
        "- MSVC 对无 BOM UTF-8 源文件可能报 C4819：CMakeLists 已加 /utf-8，勿删\n"
        "- Qt 套件版本与 MSVC ABI 不匹配会报 LNK2038：检查 _ITERATOR_DEBUG_LEVEL 一致性\n"
        "- 增删源文件后需重新 `cmake --preset win64` 或让 ZERO_CHECK 自动重配置\n"
        "- vcpkg 依赖改动必须同步 vcpkg.json；不要手工往 vcpkg_installed 里塞东西\n\n"
        "## 验收标准\n"
        "- 编译零 error；单测全绿；应用可启动并显示主窗口\n");
    writeSkill(QStringLiteral("cpp-cmake-qt-build"),
               QStringLiteral("用 CMake 配置/构建/测试 Qt 工程的流程与 MSVC 常见坑"), body);
    if (auto lg = logutil::logger())
        lg->info("已写入种子技能 cpp-cmake-qt-build");
}

} // namespace miderforge
