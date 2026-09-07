# Miderforge

**中文**：Miderforge = 云端大脑（多个大模型 API）+ 本地身体（C++ 常驻进程）——带 Qt 图形界面的 Windows 桌面 AI 编码 Agent，具备持久记忆、自沉淀技能库、任务队列、多供应商路由、沙箱化工具执行与邮件通知。
**English**: A Qt-based Windows desktop AI coding agent — cloud brain (multi-provider LLM APIs) + local body (a resident C++ process) with persistent memory, self-distilling skill library, task queue, provider routing, sandboxed tool execution and email notifications.

![License](https://img.shields.io/badge/License-MIT-blue) ![C++](https://img.shields.io/badge/C%2B%2B-20-00599C) ![Qt](https://img.shields.io/badge/Qt-6.8-41CD52) ![CI](https://img.shields.io/badge/CI-pending-lightgrey)

## Features

- 🧠 **分层记忆**（L0 工作记忆 / L1 核心记忆 / L2 会话摘要 / L3 档案库）：Agent 越用越了解你
- 🧰 **技能自沉淀**：成功任务方案自动固化为可复用 SKILL.md（agentskills.io 风格），带使用统计与降权
- 🔐 **三档权限**（Suggest / Auto Edit / Full Access）+ 永不解禁清单 + Windows 沙箱（Job Object / 环境剥离）
- 🔀 **多供应商路由**：智谱 / DeepSeek 三档（fast/main/flagship）路由与自动故障转移，DPAPI 加密存 Key
- 📋 **任务队列**：定时任务、重复执行、预算熔断（轮数 / token / 死循环三重保险），人不在电脑前也能干活
- 📧 **通知**：任务完成/失败/熔断 → 邮件（libcurl SMTP）+ 托盘气泡

## ⚠️ 安全须知

Miderforge 会在你的电脑上读写文件并执行命令。请从 Suggest 权限档开始使用；理解三档权限差异后再提升；所有操作均记录于本地审计日志（events 表）。项目按 MIT 协议“原样”提供，使用者自行承担运行风险。

## Screenshots

> 占位：UI 完成后补充主界面截图（深色主题会话视图）。

## Quick Start (English)

Prerequisites: Windows 10+, Visual Studio 2022/2026 (MSVC), CMake ≥ 3.24, vcpkg, Qt 6.8 Widgets.

```bat
git clone https://github.com/microsoft/vcpkg E:\FILE\APP\vcpkg
E:\FILE\APP\vcpkg\bootstrap-vcpkg.bat -disableMetrics
set VCPKG_ROOT=E:\FILE\APP\vcpkg
cmake --preset win64
cmake --build build --config Release --target miderforge mider_tests
ctest --preset win64-debug
```

## 构建（中文）

### 环境要求

- Windows 10 及以上，x64
- Visual Studio 2022/2026（含 MSVC v143+，本仓库在 VS18 / MSVC 14.51 验证）
- CMake ≥ 3.24（本仓库用 4.4.2 验证）
- vcpkg（manifest 模式，克隆后执行 bootstrap）
- Qt 6.8 Widgets 预编译套件（本仓库用 `C:/Qt/6.8.3/msvc2022_64`；`// 决策:` Qt 不经 vcpkg 源码重编，直接复用本机预编译套件，避免数小时编译）

### 步骤

```bat
:: 1) 获取 vcpkg（若已有可跳过；建议放在仓库外，如 E:\FILE\APP\vcpkg）
git clone https://github.com/microsoft/vcpkg E:\FILE\APP\vcpkg
E:\FILE\APP\vcpkg\bootstrap-vcpkg.bat -disableMetrics

:: 2) 配置（vcpkg manifest 模式自动安装 curl/nlohmann-json/spdlog/doctest）
set VCPKG_ROOT=E:\FILE\APP\vcpkg
cmake --preset win64

:: 3) 编译
cmake --build build --config Release --target miderforge mider_tests

:: 4) 单元测试（必须全绿）
ctest --preset win64-debug
```

> 若 Qt 安装路径不同，改 `CMakePresets.json` 里的 `CMAKE_PREFIX_PATH`。

### 首次运行向导

1. 启动 `build\Release\miderforge.exe`（或 Debug）。
2. 若 `providers.json` 不存在或未配置 Key，自动弹出首次配置向导：
   - 智谱：API Key 从 [open.bigmodel.cn](https://open.bigmodel.cn) 获取
   - DeepSeek（故障转移备胎）：API Key 从 [platform.deepseek.com](https://platform.deepseek.com) 获取
3. Key 通过 Windows DPAPI 加密后写入 `%APPDATA%\Miderforge\config\providers.json`，绝不落明文。
4. 以后可经菜单「工具 → 运行首次配置向导」重新配置。

### 运行时数据目录

```
%APPDATA%\Miderforge\
├── miderforge.db        # SQLite（M2 起）
├── memory\core.md       # L1 核心记忆（M2 起）
├── skills\              # 技能库（M3 起）
├── config\providers.json
├── logs\miderforge.log  # spdlog 滚动日志
└── workspace\           # 默认工作区
```

## Roadmap

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | 通信管道（SSE 流式/供应商直连/工具往返/断线重试） | 🔄 |
| M1 | Agent 循环 + 7 工具 + 三档权限 + Windows 沙箱 + 审计 | ⬜ |
| M2 | SQLite 四表 + FTS5 记忆系统 + 任务队列 | ⬜ |
| M3 | 技能库 + 自沉淀闭环 | ⬜ |
| M4 | 三档路由 + 故障转移 + 托盘 + 邮件 | ⬜ |

验收明细见 [docs/ACCEPTANCE.md](docs/ACCEPTANCE.md)；配置说明见 [docs/CONFIG.md](docs/CONFIG.md)。

## 开发纪律

- 按里程碑顺序实施（M0 通信管道 → M1 Agent 循环+工具+沙箱 → M2 数据库+记忆 → M3 技能库 → M4 路由+通知）
- 每里程碑结束：doctest 全绿 + 可编译可运行 + 一次 commit
