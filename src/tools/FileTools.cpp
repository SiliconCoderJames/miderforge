// 文件类工具实现
#include "tools/FileTools.h"
#include "tools/ToolRegistry.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace miderforge::FileTools {

static constexpr qint64 kMaxReadBytes = 1024 * 1024; // 1MB 读取上限（规格硬约束）

void registerReadFile(ToolRegistry& reg) {
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
        const QString rawPath = args.value("path").toString();
        if (rawPath.isEmpty()) {
            if (err) *err = QStringLiteral("path 参数为空");
            return {};
        }
        const QString path = QDir::fromNativeSeparators(QDir::cleanPath(rawPath));
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
        // 全链路 UTF-8：字节读入后 fromUtf8 解码（踩坑清单 #7）
        const QByteArray bytes = f.readAll();
        const QString content = QString::fromUtf8(bytes);
        // envelope 形式回填，便于模型区分文件内容与错误信息
        return QJsonDocument(QJsonObject{
            {"ok", true},
            {"path", path},
            {"bytes", int(bytes.size())},
            {"content", content},
        }).toJson(QJsonDocument::Compact);
    };
    reg.add(std::move(def));
}

void registerAll(ToolRegistry& reg) {
    registerReadFile(reg);
    // M1 在此追加：write_file / list_dir / search_files / run_command / read_skill / http_fetch
}

} // namespace miderforge::FileTools
