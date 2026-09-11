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

### 第二轮自审新增：已修 / 已知边界（2026-09-10）

本轮以"直接读运行时数据库 + 编译探针 + 复刻 FTS5 schema"方式复查，发现并**已修复**：

| 缺陷 | 根因 | 修复 |
|---|---|---|
| L1 核心记忆自动改写**从未真正运行** | 写入基准只存在成员变量 `m_lastAgentWrite` 里，进程重启即丢失；而 `core.md` 只在首次为空时被写过一次 → 之后每次启动都判成"用户手建"，`l1UserEditedRecently()` 恒为 true | 基准改为持久化侧车文件 `core.md.agent-state.json`（内容 SHA-256 + 写入时刻），每次判定重读；新增 4 个单测覆盖"重启后允许改写/手改后保护/旧版无基准时保守保护/无文件不保护" |
| `session_search` **永远返回空**却报成功 | `snippet(messages_fts, 2, …)` 列号越界；`messages_fts` 只有 1 列，SQLite 在 step 时返回 `SQLITE_RANGE`，被 `Database::query` 的 `while(step==SQLITE_ROW)` 静默吞掉 | 列号改 0；新增单测断言必须真正召回 2 条并带 `[...]` 高亮 |
| 记忆检索对 C++/CMake 词元**静默零召回** | 用户词只做双引号转义就裸绑进 FTS5 `MATCH`，`CMakeLists.txt`/`C++20`/`Qt6::Widgets`/`utf-8` 全被当查询语法而报错 | 新增 `ftsquery::quoteTerm/orTerms`（引号包裹 + 内部 `"` 翻倍），`MemoryManager` 与 `session_search` 统一改用；新增单测覆盖上述四类词元 |
| 旧记忆被归档而**新知从未写入**（净知识丢失） | 一致性失效归档**未**以"L1 改写成功"为前提 | 归档改为仅在 L1 改写成功（`saveL1` 返回 true）后执行；未改写时记 `memory_invalidate_skipped` 事件与日志 |
| 附件选择后发送 → **GUI 永久卡死** | `while (findChild()) deleteLater();` 只投递 DeferredDelete，不会同步移除子对象 → 同一指针上无限空转 | 先 `findChildren` 一次性取全再逐个 `deleteLater()` |
| 附件**在空闲发送时被丢弃** | 空闲路径调用 `startGoal(text)` 而非 `startGoal(goal)`，丢掉 `【上下文文件】` 段 | 改传 `goal` |
| 故障转移 + 冷却回切是**不可达死代码** | `m_transportFailures` 在 `beginTask` 归零，而第一次传输类失败必定把任务判失败，单任务内攒不到 2 次 | 改为每任务至多自动转移一次（`m_failedOver` 兜底防来回切），备胎也失败才判失败；注释与 README 口径同步（ChatClient 内含 2 次重试，实际 HTTP 尝试 3 次） |
| `writeSkill` 落库失败仍报成功 | 两次 `m_db->execute` 返回值被忽略，`return true` 恒定 | 检查返回值并复查行是否命中，失败置 `err` 返回 false（避免 SKILL.md 落盘却永不进提示词） |

**已知边界（本轮确认，暂未改）**：

| 事项 | 说明与建议 |
|---|---|
| 命令白名单是任意代码执行通道 | `cmake`/`ninja`/`msbuild`/`git`/`cl` 在名单内，而 `cmake -P script.cmake`、`git` 的 `!` 别名均可执行任意脚本 → 绕过永不解禁清单、工作区写边界与命令黑名单。建议：拒绝脚本执行类子命令（`-P`/`--script`/`-c`）、拒绝 git `!` 别名，或改为"白名单子命令 + 参数模式校验"。**未改以避免破坏现有构建工作流。** |
| 工作区边界只做字符串比较 | `pathInWorkspace` 用 `QDir::cleanPath` 归一，不解析 reparse point；工作区内的 junction/symlink 指向区外即可越界读写（实测已复现）。建议加 `canonicalFilePath` 解析后二次判定。 |
| `read_file` 不设工作区边界 | 三档均为 `Allowed`，`hardDenyReason` 对读只查文件名黑名单；文件名无害的敏感文件（浏览器凭据库、邮件库等）在任意档位可读。建议明确产品口径：要么加读边界，要么在 SECURITY.md 如实声明"读取不限于工作区"。 |
| 嵌入通道在 GUI 线程且每轮调用 | `EmbeddingClient` 自述"仅允许工作线程调用"，但全项目只有 ChatClient 的 curl 线程；启用嵌入后每轮检索含阻塞 DNS + HTTPS（各 20s 上限）。与 #29/#30 同批线程化。 |
| 向量索引无模型/维度来源标识 | `memories` 表无 `embedding_model` 列，换嵌入模型后新旧向量混用且静默劣化（仅维度不符时跳过）。建议加列并触发重嵌入。 |

### 第三轮自审：发现并修复的缺陷（2026-09-10）

本轮以「两个独立审计 + 回归测试」方式复查，发现并**已修复**（每条都补了回归测试或明确验证方式）：

| 缺陷 | 根因 | 修复 |
|---|---|---|
| **任务引擎永久僵死**（严重） | `onStreamFailed` 里清 `m_running` 的语句全在 `if (!m_failedOver)` 块内。"主供应商失败→转移成功→备胎也失败"这条路径因 `m_failedOver` 已为 true 而整块跳过 → 已 `emit loopFailed`、任务行已判失败，但 `isRunning()` 恒为 true：新目标只进待办队列、`beginTask` 拒绝、`Scheduler::tick` 永远早退，**只能重启进程恢复** | 把 `m_running = false` 提到块外无条件执行 |
| **会话右键菜单悬垂读取**（严重） | `showSessionMenu` 里 `setCurrentItem(item)` 会**同步**触发 `currentItemChanged → sessionActivated → switchToSession → sessionsChanged → loadSessions() → clear()`，而 `clear()` 是 `qDeleteAll(item)` —— 随后继续用 `item` 读 id/toolTip | ① `sessionActivated` 改 `Qt::QueuedConnection`（避免在 QListWidget 处理 currentChanged 的过程中改模型）；② 菜单期间置 `m_menuOpen` 禁止重建列表；③ **不再调用 `setCurrentItem`**，进菜单前先拷贝 id/标题，菜单返回后按 id 重新查找存活条目 |
| **删除当前会话留下孤儿消息** | `newSession()` 先 `cancel()` 再置 `m_currentSessionId = -1`；而 `cancel()` 在"等待授权/已暂停"两条路径上**同步**发 `loopFinished` → `persistMessage` 用**已被删除的** session_id 插入 assistant 消息（外加 FTS 行）→ `session_search` 永远搜得到、UI 永远够不着 | `newSession()` 里把身份作废提到 `cancel()` **之前** |
| **恢复的中断任务卡在「排队中」永不启动** | 崩溃恢复执行 `UPDATE ... SET status='queued'`，但会话路径建的 `tasks` 行 `scheduled_at` 为 **NULL**；SQL 里 `NULL = 0` 与 `NULL <= ?` 均求值为 NULL（非真）→ `tick()` 筛选永远匹配不到该行 | 恢复语句同时 `scheduled_at=COALESCE(scheduled_at, 0)`；`tick()` 查询补 `scheduled_at IS NULL`；补 SQL 语义单测 |
| **失败的命令被当成成功**（连锁影响） | `run_command` 把成败只写在结果信封里、不设 `*err`，而 `ToolRegistry::ExecResult::ok == err.isEmpty()` → 编译失败被判为"调用成功"：卡片显示 ✓，且 `recordToolSuccess()` 每次重置同因失败计数 → **同因失败熔断与升档对最容易失败的工具永不生效** | `CommandTools` 失败时设置 `*err`（超时/取消/崩溃/退出码），信封保留 |
| **修上一行时暴露的连带问题** | `ToolRegistry` 失败时直接 `text = "错误：" + err`，会**丢掉 handler 返回的信封** → 模型只看到"命令退出码 1"、拿不到 `output` 里的编译报错，把可自愈的失败变成不可自愈 | 失败时保留正文并前置错误说明；补单测断言信封仍在 |
| **手动重跑谎报成功** | `rerunTool` 丢弃 `ExecResult::ok`，UI 无条件 `setSucceeded()` + Toast「已重跑」 | 失败时回传 `failed`，卡片显示 ✗；Toast 改为不替用户下结论的「重跑未成功」 |
| **重跑继承上一任务的取消令牌** | 任务"流式中取消"时 `onStreamFinished` 的 aborted 分支直接判停且**不消费**取消令牌，令牌残留 true → 手动重跑被工具入口快检拦下，报「已被用户取消」 | `rerunTool` 入口先 `consumeToolCancel()` |
| 左栏会话高亮每次重载后丢失 | `loadSessions()` 用 `QSignalBlocker` + `clear()` 重建列表后无人恢复选中，而它在**每次写消息**时都会被调用 | 记住 `m_currentId`，重建后据此恢复高亮 |

**第三轮确认但未修（留作 Roadmap，均有明确触发场景）：**

| 事项 | 说明与建议 |
|---|---|
| 工具批遇权限确认会丢掉后续调用 | assistant 消息带 N 个 `tool_calls`，遇 `NeedsConfirm` 即在该轮 `return`，索引 i 之后的调用既不执行也不补 `role:"tool"` 结果 → 严格厂商因配对不符返回 400，宽松厂商也丢失那些调用的结果。建议：先执行整批已放行的，确认块统一后置，并为被跳过的调用补一条"因等待授权未执行"的结果 |
| 每日重复任务失败后不再排期 | `Scheduler` 只接 `loopFinished`，失败路径发的是 `loopFailed` → 每日任务失败一次即永久停摆；且失败路径不清 `m_runningTaskId`，之后任意无关任务完成会用**过期 id** 再插一条"明天"行，导致重复膨胀 |
| `memory_write` 被归类为 ReadFile | 三档权限**均自动放行、从不弹确认卡**（`write_file` 在 Suggest 档反而要确认），而其内容会被注入此后每次 system prompt，且 `memoryWriteApproval` 默认关闭 → 建议单独归类并默认需确认 |
| 0 行 UPDATE/DELETE 报成功 | 全仓无 `sqlite3_changes()`：`memory_write replace/remove` 对不存在的 id 回 `"已改写"/"已废弃"`；`archiveMemories` 返回 `ids.size()` 而非真实影响行数（还被写进 `memory_invalidate` 事件） |
| 技能成功率恒为 100% | `recordUsage(..., true, ...)` 把 success 写死，`<30% 自动标记待审查` 成为不可达分支，`SkillView` 成功率/平均轮次统计失去意义 |
| 排队目标可能落到别的会话 | `popQueueIfIdle` 用 500ms `singleShot` 延迟启动，已出队的目标无人拥有；这 500ms 内切换/新建会话，目标会被写进新会话 |
| `write_file` 不校验写入结果 | `f.write()` 返回值被忽略，短写/磁盘满仍报「写入成功 +N/−M」 |
| 多语句写无事务 | 删除会话的 `messages`+`sessions`、`persistMessage` 的插入+更新时间戳、`TaskQueueView` 的任务+审计事件（且事件 `task_id` 硬编码 0，该任务时间线永远查不到自己的入队事件） |
| `core.md` 被清空后每次启动被重置 | `main.cpp` 在 `loadL1().isEmpty()` 时写入出厂四节模板，绕过 L1 手改保护：用户故意清空 L1 后重启会被重置，并重写基准使 Agent 恢复覆盖 |

同时记录两条锁层级/线程纪律约束（防止后续扩展引入回归）：
- **锁顺序**：`EventBus::s_mutex → Database::m_writeMutex`，任何代码不得反向嵌套（EventBus.cpp / Database.h 注释）。
- **AppContext**：`todayTokens` 已原子化；其余成员仍约定 GUI 线程读写，引入工作线程前需整体审查（AppContext.h 注释）。

## M4.5：记忆分层强化（存储体系管理）——设计文档见 docs/MEMORY-DESIGN.md

- [x] 一致性失效：收尾改写 L1 时归档被新知覆盖的 L3 旧条目；编号白名单校验防幻觉（filterSupersededIds/archiveMemories 单测）(2026-09-08)
- [x] L1 容量纪律：收尾提示词带 token 预算，超限写 l1_overflow 告警（代码审查验证）(2026-09-08)
- [x] token 记账改增量口径（本轮新增输入+输出），消除 O(N²) 超线性（代码审查验证）(2026-09-08)
- [x] 每轮检索查询随模型反思演化，不再 25 轮注入同一批记忆（代码审查验证）(2026-09-08)
- [x] L2 淘汰评分化：50 条 hot 席位按 importance×时间衰减评分保留（与 L3 评分同源），高价值旧摘要不再被 FIFO 挤掉；打平退化为 FIFO 保最新（单测：51 抢 50 席时 0.9 旧摘要存活）(2026-09-09)

## M5：中断分级（已完成）

四级中断模型（P0 系统级 / P1 熔断级 / P2 用户级 / P3 操作级）+ 两轴分类（可恢复性 × 协作性）：

- [x] 终态语义统一：tasks 表五态落库（succeeded/failed/halted/cancelled/paused），取消经终态覆写不再误记为 failed；UI 语义色补 halted/paused (2026-09-08)
- [x] P3 取消令牌贯穿工具层：ToolRegistry 原子令牌 → run_command 入口快检 + 150ms 轮询 kill / http_fetch 进度回调中止 → 工具批收尾消费判停（单测覆盖令牌语义与入口快检）(2026-09-08)
- [x] P2 暂停/恢复：轮边界安全点挂起（不打断在跑工具/流式），m_history 保持可续；挂起期间任务行 status='paused'，取消可直接判停，启动恢复将遗留 paused 行重新入队（代码审查验证，GUI 交互待人工）(2026-09-08)
- [x] P0 接入 OS 会话信号：aboutToQuit + commitDataRequest（WM_QUERYENDSESSION）→ shutdownRequeue（在跑任务放回 queued、请求工具快速中止，与启动恢复闭环）(2026-09-08)
- [x] checkpoint 续跑：tasks.context_json 列（增量迁移）每轮末/挂起时持久化 history/轮次/路由档/token 预算；Scheduler 重新入队时经 startWithCheckpoint 从断点的下一轮继续，终态自动清除断点（单测覆盖迁移列读写与清除）(2026-09-08)

## M6-A：L3 语义检索（已完成，端到端待真实 Key 复核）

- [x] EmbeddingClient：批量嵌入 + 响应解析（index 对位/维度一致性/缺项报错，单测）+ https 公网端点校验（单测）(2026-09-09)
- [x] NetGuard 严格公网判定：修复 Qt6.8 isGlobal() 误判 RFC1918 的 SSRF 缺口（fetch 与嵌入共用，字面私有 IP 回归单测）(2026-09-09)
- [x] 混合检索：FTS5 + 向量余弦 RRF 融合；BLOB 绑定/读取分支（0x00 不截断）；updateContent 向量失效；backfillEmbeddings 渐进补齐（全部单测，94 用例全绿）(2026-09-09)
- [x] 无嵌入器时行为与 M5 完全一致（既有记忆用例零改动通过，回归保护）(2026-09-09)
- [ ] 真实 Key 端到端：zhipu embedding-3 召回质量人工复核（配置后检索「语义近词面远」的记忆观察是否命中）
- [x] 供应商页 embedding 配置 UI（设置→供应商页：启用开关/嵌入供应商/模型，落盘 providers.json embedding 节）(2026-09-10)

## 打磨期：品牌 + Codex 要素 + Hermes 记忆（2026-09-10）

- [x] 品牌「熔炉·铁灰炉火」默认主题 + codex/zcode/claude 三致敬皮肤（QSettings 持久化；活动栏 🎨 菜单与设置→通用页切换，重启完全生效）；活动栏品牌区 🔨 + 品牌色→强调色渐变签名线；状态栏品牌签名「Miderforge · 锻造云脑·常驻本机」；106 处取色点迁移 colors:: 实时函数（主题纯函数单测 + 实机 UIA 结构取证：7 图标按钮/变更面板/会话面板）(2026-09-10)
- [x] Codex 要素：DiffUtil 行级 diff（单测）→ write_file 覆盖时 +N/−M 统计与 unified 正文进结果 envelope；工具卡 diff 逐行着色（+绿/−红/@@品牌色）；会话页右侧「变更文件」面板（路径归档/计数徽标/点击弹窗看 diff，随新任务/切会话清空）；Composer 📎 附件上下文（chips 可删，发送并入目标文本）（实机 UIA 取证「变更文件（0）」面板呈现）(2026-09-10)
      ⚠️ 2026-09-10 自审修正：本项此前标 ✅ 但**实际不可用**——① chips 清空用 `while(findChild()) deleteLater();` 不终止，选附件后发送会让 GUI 100% CPU 永久卡死；② 空闲发送走 `startGoal(text)` 丢掉 `【上下文文件】` 段，附件路径从未传给 Agent。两处均已修复（见下方第二轮自审表），**GUI 端到端仍需人工复核**。
- [x] Hermes 记忆：memory_write 三动作（add 查重/replace/remove + 500 字上限 + save/skip 策展门拒收清单转储）；write_approval 审批门（暂存 pending → 记忆页「⏳ 待审」批准/拒绝）；isSafeMemoryContent 安全扫描（不可见 Unicode 逐字符判定 + 中英注入词面，单测）；session_search（messages FTS5 trigram + 触发器三件套，snippet 工具）；L1 注入升级（用量头部 + § 分条，renderL1ForPrompt 单测）(2026-09-10)
      ⚠️ 2026-09-10 自审修正：`session_search` 此前**永远返回空**（`snippet` 列号越界 2 → 应为 0，错误被静默吞掉），已修复并补单测；`isSafeMemoryContent` 覆盖范围小于其注释所述（**未含**变体选择符 U+FE00–FE0F / U+E0100–E01EF 与方向隔离符 U+2066–2069），注释口径已按实际收窄，补齐待 Roadmap。
- [ ] 发一条真实任务人工复核：会话建档/消息入库/变更树回填/diff 弹窗/memory_write 全链路（需真实 API Key）
- [ ] 主题皮肤切换后的视觉人工复核（本会话截屏不可靠；重启后观察炉火橙主色调）
