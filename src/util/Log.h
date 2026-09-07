// 日志初始化：spdlog 滚动文件日志（logs/ 目录）+ Qt 消息处理器重定向
#pragma once
#include <memory>
#include <QString>

namespace spdlog {
class logger;
}

namespace miderforge::logutil {

// 初始化滚动文件日志；重复调用无副作用
void init(const QString& logDir);
// 全局 logger（init 前调用返回 nullptr）
std::shared_ptr<spdlog::logger> logger();

} // namespace miderforge::logutil
