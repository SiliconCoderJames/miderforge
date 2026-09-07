// 应用数据目录管理：%APPDATA%\Miderforge 及其子目录（memory/skills/config/logs/workspace）的定位与创建
#pragma once
#include <QString>

namespace miderforge::appdirs {

// 根目录：%APPDATA%/Miderforge（QStandardPaths::AppDataLocation 之下）
QString root();
// 根目录下相对路径拼接（rel 用 '/' 分隔）
QString file(const QString& rel);
// 首次运行时创建完整目录骨架；返回是否全部就绪
bool ensureLayout();
// 默认工作区（Agent 写文件限定根目录）
QString workspaceRoot();

} // namespace miderforge::appdirs
