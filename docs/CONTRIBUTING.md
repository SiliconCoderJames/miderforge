# 贡献指南

感谢考虑为 Miderforge 贡献！这是一个以「记忆分层 × 类人学习」为核心卖点的 Windows 桌面 AI Agent，任何让记忆/技能/成长闭环更可靠、让安全边界更严实的 PR 都特别受欢迎。

## 开发环境

- Windows 10+ · Visual Studio 2022/2026 (MSVC) · CMake ≥ 3.24 · vcpkg · Qt 6.8 Widgets
- 依赖通过 vcpkg manifest 自动安装（`vcpkg.json`），Qt 路径在 `CMakePresets.json` 的 `CMAKE_PREFIX_PATH` 配置

```bat
set VCPKG_ROOT=E:\FILE\APP\vcpkg
cmake --preset win64
cmake --build build --config Release --target miderforge mider_tests
ctest --preset win64-debug
```

## 提交前检查清单

1. **测试全绿**：`mider_tests` 全部通过；为纯逻辑改动（权限门/路由/熔断/记忆/SSE 等）补充 doctest 单测。权限门属于安全边界，任何行为变更必须带对抗用例回归。
2. **代码风格**：
   - 中文注释；注释解释「为什么/决策依据」，不复述代码；
   - 成员变量 `m_` 前缀；命名空间 `miderforge`；
   - 全链路 UTF-8（MSVC 下 `/utf-8` 已全局开启）；
   - 分层纪律：`src/core|llm|memory|...` 不得反向依赖 `src/app`；单测只允许链接 `mider_core`。
3. **密钥卫生红线**（硬约束）：
   - 运行自检：`git ls-files | findstr /i "key secret token env pass"`，确认无敏感值入库；
   - API Key / SMTP 授权码只能经 Windows DPAPI 加密落盘，源码、示例、测试中禁止出现可用的凭据字面量；
   - 唯一允许入库的配置文件是 `config/providers.template.json`。
4. **锁层级纪律**：`EventBus::s_mutex → Database::m_writeMutex` 方向不得反转（详见两处头文件注释）。
5. **提交规范**：小步提交，一个 commit 一件事；消息用中文、动词开头说明改了什么与为什么。

## 哪些改动需要先开 Issue 讨论

- 权限模型 / 永不解禁清单语义变更
- 记忆分层结构（L0–L3）与收尾提炼链路的字段协议
- 新增网络/命令类工具
- tasks 表状态机语义（M5 中断分级会重构此处，动手前请先对齐）

## 安全对抗资产与安全关键清单（硬约束）

1. **对抗用例不许删、不许 skip**：`tests/adversarial/` 下的用例是安全回归资产——
   任何 PR 不得删除、注释掉、或用 `SUBCASE`/跳过标记使其失效；`ctest -N` 必须能看到
   全部对抗套件（名字一律含 `adversarial`）。安全行为的任何变更（权限门/白名单/边界/
   投毒筛查/固化门槛/降权联动）必须带新的对抗用例或对既有用例的加强。
2. **安全关键清单须人工 review**：仓库根目录 `SECURITY-CRITICAL.txt` 列出安全关键文件。
   触及清单内任何文件的 PR 必须由人工逐一 review，不允许自动合并。
3. **不许改清单移出文件**：`SECURITY-CRITICAL.txt` 只允许追加（新增安全关键文件），
   禁止从清单删除或移出条目以绕开人工 review；移出必须走安全事件复盘流程并留审计记录。

## 报告 Bug

请附：复现步骤、期望 vs 实际、相关审计日志（`logs/events.jsonl` 中脱敏后的条目）。安全漏洞请勿公开 Issue，走 [SECURITY.md](SECURITY.md) 的私下披露流程。
