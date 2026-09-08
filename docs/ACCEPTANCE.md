# Miderforge 里程碑验收清单

每完成一项在 `[ ]` 内打 `x` 并注明验证方式与日期。

## M0：通信管道

- [ ] 两家供应商流式输出，中文无乱码（待真实 API Key 配置后人工验证；管线已由本地 mock SSE 服务器端到端覆盖）
- [ ] GLM-5.3 的 reasoning_content 与 content 分栏正确显示（双流逻辑已实现并经 mock 验证；真实 GLM 待 Key）
- [x] 工具参数碎片正确累积拼装，read_file 往返成功（碎片累积经 mock 与单测验证；read_file 完整往返待真实 Key）(2026-09-07)
- [x] 流中途断网 → 重连后自动重试成功（mock 模拟连接中断，指数退避后重连成功）(2026-09-07)
- [x] doctest：SseParser 分帧用例 ≥6 个（含跨块边界用例）全过（12 个用例；全仓 24/24 全绿）(2026-09-07)

## M1：Agent 循环 + 工具 + 沙箱

- [ ] 会话中下达真实小任务，Agent 自主多轮工具调用至完成（状态机与工具批已实现；端到端待真实 API Key 人工验证）
- [ ] Suggest 模式：write_file 生成提案弹确认，拒绝后不执行（权限门 NeedsConfirm→卡片确认流已实现并挂接；端到端待真实 Key）
- [x] Auto Edit：工作区内写自动通过，区外直接拒绝并记 events（PermissionGate 单测覆盖区内/区外/反斜杠路径）(2026-09-07)
- [x] 拒绝操作在审计日志视图可见完整 JSON（EventBus 双写 JSONL+events 表，AuditLogView 实时展示）(2026-09-07)
- [ ] Miderforge 退出时 Job Object 连带杀死所有工具子进程（Job Object KILL_ON_JOB_CLOSE 已实现并随 run_command 生效；零孤儿进程待任务管理器人工验证）

## M2：数据库 + 记忆

- [x] 灌入 100 条中文测试记忆，搜「编译器」能命中「编译器报错修复记录」（trigram 单测）(2026-09-07)
- [x] 搜 2 字词「热键」走 LIKE 兜底仍能命中（单测）(2026-09-07)
- [ ] system prompt 中可见 L1 内容（日志验证）；记忆视图手改 L1 后下一任务生效（注入代码已实现；待真实任务日志验证）
- [ ] 任务完成后 tasks/memories/events 三表各新增正确记录（收尾流程已实现；端到端待真实 Key）
- [ ] WAL 模式下 UI 查询与后台写入无锁死（WAL+busy_timeout 5s+写互斥已配置；长时间运行待验证）

## M3：技能库

- [ ] 跑标准示例任务（≥5 次工具调用、成功）后自动生成 SKILL.md，frontmatter 合规（自沉淀闭环已实现；端到端待真实 Key）
- [ ] 再次下达同类任务时 system prompt「可用技能」段出现该技能，Agent 调 read_skill 加载（注入逻辑已实现；待真实 Key）
- [x] 技能视图统计随使用更新；手建技能可被检索命中（SkillManager 单测：merge version+1、检索命中、统计降权）(2026-09-07)
- [x] 种子技能 cpp-cmake-qt-build 已预置（启动时自动写入）(2026-09-07)

## M4：路由 + 供应商面板 + 通知 + 托盘

- [ ] 指定“架构规划”类任务日志显示走 flagship 档（Router 分类单测通过；端到端日志待真实 Key）
- [ ] 把 active 供应商 base_url 改为不可达地址 → 连续 2 次失败自动切 DeepSeek，状态灯变红，events 有 provider_switch（switchToFailover 已实现；端到端待真实 Key）
- [ ] 收件箱收到任务完成邮件，中文标题不乱码（RFC 2047 B 编码 + smtps 已实现；待真实 SMTP 授权码人工验证）
- [ ] 设定 1 分钟后的定时任务 → 到点自动执行 → 托盘气泡弹出（Scheduler 20s 轮询+托盘气泡已实现；待人工验证）

## 已知边界与设计取舍（2026-09-08 外部审查复盘）

32 项外部审查中 28 项已修复（安全批 / Agent 循环批 / 网络批 / 持久化批，含新增对抗单测）。
以下 5 项经评估属设计权衡或明确记录的已知边界，暂不改动，留作 Roadmap 输入：

| # | 事项 | 处置结论 |
|---|---|---|
| 12 | 任务队列为串行（一次一个任务） | 有意设计：README 如实描述"执行中目标自动排队"；并行执行涉及工具互斥与权限卡片并发，待 Roadmap |
| 14 | WAL 下读者可能读到不含最新写入的快照 | WAL 快照语义本身，非缺陷；已在 Database.h 注释说明 |
| 17 | FTS5 trigram 对 <3 字符中文词召回依赖 LIKE 兜底（无索引全扫） | 兜底已修复通配符转义并支持多词 AND；向量检索通道（sqlite-vec 已加载）留待后续启用 |
| 29 | http_fetch 同步执行于 GUI 线程（最长 15s 冻结） | 该工具默认全部档位需用户确认后才执行；迁移工作线程与工具层整体线程化（含 #30）同批处理 |
| 30 | CommandTools 用嵌套 QEventLoop 等待进程 | 已加 ExcludeUserInputEvents 缓解输入事件重入；根治需把工具执行移出 GUI 线程，与 #29 同一 Roadmap 项 |

同时记录两条锁层级/线程纪律约束（防止后续扩展引入回归）：
- **锁顺序**：`EventBus::s_mutex → Database::m_writeMutex`，任何代码不得反向嵌套（EventBus.cpp / Database.h 注释）。
- **AppContext**：`todayTokens` 已原子化；其余成员仍约定 GUI 线程读写，引入工作线程前需整体审查（AppContext.h 注释）。

## M4.5：记忆分层强化（存储体系管理）——设计文档见 docs/MEMORY-DESIGN.md

- [x] 一致性失效：收尾改写 L1 时归档被新知覆盖的 L3 旧条目；编号白名单校验防幻觉（filterSupersededIds/archiveMemories 单测）(2026-09-08)
- [x] L1 容量纪律：收尾提示词带 token 预算，超限写 l1_overflow 告警（代码审查验证）(2026-09-08)
- [x] token 记账改增量口径（本轮新增输入+输出），消除 O(N²) 超线性（代码审查验证）(2026-09-08)
- [x] 每轮检索查询随模型反思演化，不再 25 轮注入同一批记忆（代码审查验证）(2026-09-08)

## M5：中断分级（实施中）

四级中断模型（P0 系统级 / P1 熔断级 / P2 用户级 / P3 操作级）+ 两轴分类（可恢复性 × 协作性）：

- [x] 终态语义统一：tasks 表五态落库（succeeded/failed/halted/cancelled/paused），取消经终态覆写不再误记为 failed；UI 语义色补 halted/paused (2026-09-08)
- [x] P3 取消令牌贯穿工具层：ToolRegistry 原子令牌 → run_command 入口快检 + 150ms 轮询 kill / http_fetch 进度回调中止 → 工具批收尾消费判停（单测覆盖令牌语义与入口快检）(2026-09-08)
- [ ] P2 暂停/恢复：轮边界挂起状态机，m_history 保持可续
- [ ] P0 接入 OS 会话信号（WM_QUERYENDSESSION）：存检查点后有界退出
- [ ] checkpoint 续跑：崩溃恢复从"重新入队重跑"升级为"断点续跑"
