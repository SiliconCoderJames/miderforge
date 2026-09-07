// 命令类工具：run_command —— QProcess 加固（工作区钉死 + 环境白名单）+ Windows Job Object 沙箱
#pragma once
namespace miderforge {

class ToolRegistry;

namespace CommandTools {

void registerAll(ToolRegistry& reg);

} // namespace CommandTools
} // namespace miderforge
