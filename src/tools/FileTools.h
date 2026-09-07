// 文件类工具：read_file / write_file / list_dir / search_files（权限判定在 AgentLoop 的权限门完成）
#pragma once
#include <QString>

namespace miderforge {

class ToolRegistry;

namespace FileTools {

// M1 一次性注册全部文件类工具（read_file 权限默认三档自动；write_file 见权限门）
void registerAll(ToolRegistry& reg);

// 相对路径 → 绝对路径（相对工作区解析）；空路径返回工作区本身
QString resolveWorkspacePath(const QString& rawPath, const QString& workspaceRoot);

} // namespace FileTools
} // namespace miderforge
