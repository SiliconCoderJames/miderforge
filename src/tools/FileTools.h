// 文件类工具：M0 注册 read_file；write_file/list_dir/search_files 在 M1 随权限门一起接入
#pragma once
namespace miderforge {

class ToolRegistry;

namespace FileTools {

// read_file：读取文本文件（≤1MB），三档权限均自动放行
void registerReadFile(ToolRegistry& reg);

// M1 一次性注册全部工具
void registerAll(ToolRegistry& reg);

} // namespace FileTools
} // namespace miderforge
