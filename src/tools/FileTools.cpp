// 文件类工具实现
#include "tools/FileTools.h"
#include "core/AppContext.h"
#include "tools/ToolRegistry.h"
#include "util/DiffUtil.h"
#include "util/PathReal.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace miderforge::FileTools {

static constexpr qint64 kMaxReadBytes = 1024 * 1024;      // 1MB 读取上限（规格硬约束）
static constexpr qint64 kMaxWriteBytes = 4 * 1024 * 1024; // 决策: 单次写入 4MB 上限防失控
static constexpr int kMaxSearchResults = 200;             // 决策: 搜索结果封顶

QString resolveWorkspacePath(const QString& rawPath, const QString& workspaceRoot) {
    // P0-1：不做 QDir::cleanPath 词法折叠——"ws/junction/../x" 会被折叠成区内的 ws/x，
    // 让权限门与落盘复核都在对一条与 OS 语义不同的路径做判定。这里只归一分隔符：
    // ".." 交给 resolveReal 逐组件真实解析（判定），交给 QFile 原生语义（执行）
    const QString trimmed = rawPath.trimmed();
    if (QFileInfo(trimmed).isAbsolute() || trimmed.contains(QLatin1Char(':')))
        return QDir::fromNativeSeparators(trimmed);
    if (workspaceRoot.isEmpty())
        return QDir::fromNativeSeparators(trimmed);
    return QDir::fromNativeSeparators(workspaceRoot + QLatin1Char('/') + trimmed);
}

static QString envelope(const QJsonObject& obj) {
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

// ---------- read_file（三档均自动） ----------
static void registerReadFile(ToolRegistry& reg) {
    ToolDef def;
    def.name = QStringLiteral("read_file");
    def.description = QStringLiteral("读取指定路径的文本文件内容（UTF-8），上限 1MB");
    def.parameters = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{
                           {"path", QJsonObject{
                                        {"type", "string"},
                                        {"description", "文件的绝对路径或相对工作区的路径"},
                                    }},
                       }},
        {"required", QJsonArray{"path"}},
    };
    def.handler = [](const QJsonObject& args, QString* err) -> QString {
        const QString path = resolveWorkspacePath(args.value("path").toString(),
                                                  AppContext::instance().workspaceRoot);
        if (path.isEmpty()) {
            if (err) *err = QStringLiteral("path 参数为空");
            return {};
        }
        QFileInfo info(path);
        if (!info.exists()) {
            if (err) *err = QStringLiteral("文件不存在：%1").arg(path);
            return {};
        }
        if (info.isDir()) {
            if (err) *err = QStringLiteral("目标是目录而非文件：%1").arg(path);
            return {};
        }
        if (info.size() > kMaxReadBytes) {
            if (err) *err = QStringLiteral("文件超过 1MB 读取上限（%1 字节）").arg(info.size());
            return {};
        }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (err) *err = QStringLiteral("无法打开：%1").arg(f.errorString());
            return {};
        }
        const QByteArray bytes = f.readAll();
        return envelope(QJsonObject{
            {"ok", true},
            {"path", path},
            {"bytes", int(bytes.size())},
            {"content", QString::fromUtf8(bytes)}, // 全链路 UTF-8（踩坑清单 #7）
        });
    };
    reg.add(std::move(def));
}

// ---------- write_file（权限：Suggest 需确认 / Auto Edit 区内自动 / Full Access 自动） ----------
static void registerWriteFile(ToolRegistry& reg) {
    ToolDef def;
    def.name = QStringLiteral("write_file");
    def.description = QStringLiteral("写入文本文件（UTF-8），必须带完整新内容；相对路径以工作区为根");
    def.parameters = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{
                           {"path", QJsonObject{
                                        {"type", "string"},
                                        {"description", "目标文件路径（绝对或相对工作区）"},
                                    }},
                           {"content", QJsonObject{
                                           {"type", "string"},
                                           {"description", "文件的完整新内容"},
                                       }},
                       }},
        {"required", QJsonArray{"path", "content"}},
    };
    def.handler = [](const QJsonObject& args, QString* err) -> QString {
        const QString path = resolveWorkspacePath(args.value("path").toString(),
                                                  AppContext::instance().workspaceRoot);
        const QString content = args.value("content").toString();
        if (path.isEmpty()) {
            if (err) *err = QStringLiteral("path 参数为空");
            return {};
        }
        // P0-1 纵深防御：落盘前按解析后的真实路径复核工作区边界。权限门已判一次；
        // 这里独立再拦一次——未来任何绕过权限门的调用点，junction/symlink 越界写也到不了磁盘
        //（tests/adversarial/WorkspaceBoundaryTest 的"绕过权限门直调 handler"用例验证本层）
        const QString wsRoot = AppContext::instance().workspaceRoot;
        if (!wsRoot.isEmpty()
            && !pathreal::isInsideOrEqual(pathreal::resolveReal(path), pathreal::resolveReal(wsRoot))) {
            if (err) *err = QStringLiteral("写入被拒绝：目标解析后的真实路径不在工作区内（%1）").arg(path);
            return {};
        }
        const qint64 bytes = content.toUtf8().size();
        if (bytes > kMaxWriteBytes) {
            if (err) *err = QStringLiteral("内容超过 4MB 写入上限");
            return {};
        }
        QFileInfo info(path);
        if (info.exists() && info.isDir()) {
            if (err) *err = QStringLiteral("目标是已存在的目录：%1").arg(path);
            return {};
        }
        // 覆盖已有文件时先取旧内容做行级 diff（Codex 式线程内变更审查）
        QJsonObject diffObj;
        QString message = QStringLiteral("写入成功");
        if (info.exists()) {
            QFile oldF(path);
            if (oldF.open(QIODevice::ReadOnly) && oldF.size() <= kMaxWriteBytes) {
                const QString oldText = QString::fromUtf8(oldF.readAll());
                oldF.close();
                const auto d = DiffUtil::unified(oldText, content);
                if (d.added || d.removed) {
                    message = QStringLiteral("写入成功（+%1/−%2 行）").arg(d.added).arg(d.removed);
                    diffObj = QJsonObject{
                        {"added", d.added},
                        {"removed", d.removed},
                        {"diff", d.unified},
                        {"truncated", d.truncated},
                    };
                }
            }
        } else {
            message = QStringLiteral("写入成功（新文件，%1 行）").arg(
                content.isEmpty() ? 0 : content.count(QLatin1Char('\n')) + 1);
        }
        QDir().mkpath(info.absolutePath()); // 决策: 自动创建父目录
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (err) *err = QStringLiteral("无法写入：%1").arg(f.errorString());
            return {};
        }
        const QByteArray bytes8 = content.toUtf8();
        f.write(bytes8);
        f.close();
        QJsonObject result{
            {"ok", true},
            {"path", path},
            {"bytes", int(bytes8.size())},
            {"message", message},
        };
        if (!diffObj.isEmpty())
            result["diff"] = std::move(diffObj);
        return envelope(result);
    };
    reg.add(std::move(def));
}

// ---------- list_dir（三档均自动） ----------
static void registerListDir(ToolRegistry& reg) {
    ToolDef def;
    def.name = QStringLiteral("list_dir");
    def.description = QStringLiteral("列出目录内容（名称/类型/大小），目录优先");
    def.parameters = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{
                           {"path", QJsonObject{
                                        {"type", "string"},
                                        {"description", "目录路径（默认工作区根）"},
                                    }},
                       }},
    };
    def.handler = [](const QJsonObject& args, QString* err) -> QString {
        QString path = args.value("path").toString();
        if (path.trimmed().isEmpty())
            path = AppContext::instance().workspaceRoot;
        path = resolveWorkspacePath(path, AppContext::instance().workspaceRoot);
        QFileInfo info(path);
        if (!info.isDir()) {
            if (err) *err = QStringLiteral("不是有效目录：%1").arg(path);
            return {};
        }
        QJsonArray entries;
        const QFileInfoList list =
            QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden, QDir::DirsFirst | QDir::Name);
        for (const QFileInfo& fi : list) {
            if (entries.size() >= 500) // 决策: 单次列目录封顶 500 条
                break;
            entries.append(QJsonObject{
                {"name", fi.fileName()},
                {"dir", fi.isDir()},
                {"bytes", fi.isDir() ? 0 : int(fi.size())},
            });
        }
        return envelope(QJsonObject{{"ok", true}, {"path", path}, {"entries", entries}});
    };
    reg.add(std::move(def));
}

// ---------- search_files（三档均自动）：文件名 glob + 可选内容正则 ----------
static void registerSearchFiles(ToolRegistry& reg) {
    ToolDef def;
    def.name = QStringLiteral("search_files");
    def.description = QStringLiteral("在工作区内递归搜索：按文件名通配符过滤，可再按内容正则匹配");
    def.parameters = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{
                           {"pattern", QJsonObject{
                                           {"type", "string"},
                                           {"description", "文件名通配符，如 *.cpp（* 为全部文件）"},
                                       }},
                           {"content", QJsonObject{
                                           {"type", "string"},
                                           {"description", "可选：文件内容正则表达式（UTF-8 文本文件）"},
                                       }},
                           {"path", QJsonObject{
                                        {"type", "string"},
                                        {"description", "搜索根目录（默认工作区）"},
                                    }},
                       }},
        {"required", QJsonArray{"pattern"}},
    };
    def.handler = [](const QJsonObject& args, QString* err) -> QString {
        const QString pattern = args.value("pattern").toString();
        if (pattern.isEmpty()) {
            if (err) *err = QStringLiteral("pattern 参数为空");
            return {};
        }
        QString root = args.value("path").toString();
        if (root.trimmed().isEmpty())
            root = AppContext::instance().workspaceRoot;
        root = resolveWorkspacePath(root, AppContext::instance().workspaceRoot);

        const QString contentRe = args.value("content").toString();
        QRegularExpression re;
        if (!contentRe.isEmpty()) {
            re = QRegularExpression(contentRe, QRegularExpression::CaseInsensitiveOption);
            if (!re.isValid()) {
                if (err) *err = QStringLiteral("内容正则不合法：%1").arg(re.errorString());
                return {};
            }
        }

        QJsonArray hits;
        int scanned = 0;
        QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            ++scanned;
            const QString name = QFileInfo(path).fileName();
            if (!QDir::match(pattern, name))
                continue;
            if (!re.isValid()) {
                hits.append(path);
                if (hits.size() >= kMaxSearchResults)
                    break;
                continue;
            }
            QFile f(path);
            if (f.size() > kMaxReadBytes)
                continue; // 大文件跳过内容匹配
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QByteArray bytes = f.readAll();
            if (bytes.contains('\0'))
                continue; // 二进制文件跳过
            const QString text = QString::fromUtf8(bytes);
            if (re.match(text).hasMatch()) {
                hits.append(path);
                if (hits.size() >= kMaxSearchResults)
                    break;
            }
        }
        return envelope(QJsonObject{
            {"ok", true}, {"root", root}, {"scanned", scanned}, {"hits", hits}});
    };
    reg.add(std::move(def));
}

void registerAll(ToolRegistry& reg) {
    registerReadFile(reg);
    registerWriteFile(reg);
    registerListDir(reg);
    registerSearchFiles(reg);
    // run_command 在 CommandTools、read_skill 在 SkillTools、http_fetch 在 NetTools 中注册
}

} // namespace miderforge::FileTools
