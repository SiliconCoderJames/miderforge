# 记忆分层设计（Memory Hierarchy）

> Miderforge 的核心差异化不是"能干活"——那是所有 Agent harness 的及格线——而是**记忆**：
> 一个像计算机存储体系一样分区管理的、会随使用而成长的知识系统。

## 一、设计类比：计算机存储体系

| 存储体系 | 特性 | Miderforge 记忆层 | 管理策略 |
|---|---|---|---|
| 寄存器 / L1 Cache | 每周期访问、容量极小 | **L0** 当前任务上下文（目标、工具清单、轮次，每轮重建） | 编译期确定，无需淘汰 |
| SRAM Cache | 快、小、回写、可写保护 | **L1** 核心记忆（`memory/core.md`，常驻注入，默认 4000 token 上限） | 收尾 LLM 提炼式改写（≈回写）；用户手改保护（≈写保护引脚，见 §五 基准机制） |
| RAM 工作集 | 断电即失、有硬上限 | **会话历史** `m_history`（本轮上下文窗口） | 轮数熔断 + token 预算 + 工具结果截断（≈换页） |
| FLASH / SSD | 持久、较快、冷热分层 | **L2** 会话摘要（memories 表 `session_summary` 类型，滚动 50 条） | FIFO 上限 + 最旧降权 `importance 0.6→0.2`（≈冷数据降频） |
| 磁盘档案库 | 大、慢、按需检索 | **L3** 档案库（memories 表其余类型，FTS5 trigram 全文检索） | 每轮动态检索 Top5 注入（≈按需页入），命中即 `access_count` 回写 |
| ROM 固件 | 只读、出厂固化 | **禁区**：用户画像中的禁令 + 权限门「永不解禁清单」 | 任何档位不可覆写 |

## 二、每层的读写路径

```
任务开始 ──► L1 全文常驻注入（SRAM → 寄存器）
         ──► L3 检索 Top5 注入（磁盘 → RAM；查询 = 目标 + 模型上轮反思，随轮次演化）
         ──► L2 最近 5 条摘要注入（FLASH 预读）
任务进行 ──► 工具结果截断后回填（RAM 容量护栏：头 12K + 尾 2K 字符）
任务结束 ──► 收尾 LLM 提炼：result_summary / session_summary / l1_new / lesson
         ──► L2 追加 + FIFO 降权（单条 UPDATE … IN 子查询，隐式事务）
         ──► L1 提炼式改写（检测到用户手改则跳过，见 §五）
         ──► 一致性失效：归档被新知覆盖的 L3 旧条目（见下）
```

## 三、一致性失效（write-through + invalidation）

存储体系最容易被忽视的一课：**改了上层，下层要跟着失效**。Agent 更新了认知
（"项目已从 Meson 迁移到 CMake"）却不在磁盘档案里作废旧条目，下次检索就会
新旧并存、靠评分碰运气——像人脑记得两个矛盾的结论。

Miderforge 的机制（双保险防 LLM 幻觉编号）：

1. 任务进行中，所有被注入过的 L3 记忆登记为候选清单（id + 内容摘要）；
2. 收尾提示词把这份清单带编号交给 LLM，请它输出 `superseded_memory_ids`
   （被本次新知覆盖/推翻的旧记忆编号）；
3. 落库前 `MemoryManager::filterSupersededIds` 做编号白名单校验：**只接受
   数字、去重、且必须 ⊆ 本次真正注入过的编号**——幻觉编号直接丢弃；
4. `archiveMemories` 单条 `UPDATE … IN` 批量归档（废弃=标记，不物理删），
   并写 `memory_invalidate` 审计事件。

## 四、容量纪律与淘汰

| 层 | 容量 | 超限行为 |
|---|---|---|
| L1 | `l1TokenLimit`（默认 4000 token） | 提示词要求控制在限内 + 落库后实测超限写 `l1_overflow` 告警（v1 软约束） |
| 会话历史 | 轮数 ≤25 / 增量 token 预算（默认 500K） | 三重熔断 → 走完整收尾链（熔断即学习） |
| L2 | 50 条 hot（评分制：importance×时间衰减，与 L3 评分同源；分数打平退化为 FIFO 保最新） | 落选者单语句降权至 0.2（非删除，冷数据仍可被 L3 检索到；降权单调不回升） |
| L3 | 不设上限 | 排序靠综合评分：`0.5·BM25 + 0.2·importance + 0.2·时间衰减(半衰期30天) + 0.1·log(access+1)` |

token 记账口径为**增量制**：每轮只计「本轮新增输入（本轮 prompt − 上轮 prompt）
+ 本轮输出」，避免整段重发 history 导致的 O(N²) 超线性消耗。

## 五、检索策略

- **trigram FTS5**（≥3 字符词）为主，BM25 粗排 → 应用层综合评分精排；
- **短词 LIKE 兜底**（<3 字符，中文单/双字词）：通配符已转义（`%`/`_` 按字面匹配），
  多词 AND 组合；
- **查询演化**：首轮用任务原文，之后混入模型最近的反思/计划文本——RAG 的
  「动态相关」才有意义，25 轮注入同一批记忆等于没有记忆。
- **语义向量通道（M6-A）**：注入嵌入器后（providers.json 顶层 `embedding` 节，复用
  供应商 Key），检索升级为双通道——FTS5 字面召回 + 向量余弦召回（查询与全部已嵌入
  active 记忆算 cosine，Top20），RRF（k=60）融合名次。缺向量的条目由
  `backfillEmbeddings` 每轮渐进补齐（限 8 条控网络开销）；向量存 `memories.embedding`
  BLOB（float32 小端），内容更新即置 NULL 失效重嵌（write-through invalidation）。
  嵌入失败/未配置静默降级纯 FTS5，主路永不因语义通道受阻。

## 六、Roadmap（后续）

- ~~淘汰策略升级：L2 的 FIFO 降权升级为评分制~~ ✅ 已完成（2026-09-09，含"高价值旧摘要不被挤掉"回归单测）
- ~~跨层预取：任务开始时按目标预取相关技能~~ ✅ 已完成（2026-09-09，`searchRelevant` 拆词 OR + CJK 滑窗，提示词 ⭐ 前排标注；只做注意力预取、不全文注入，保持渐进披露与统计诚实）
- ~~记忆一致性检查~~ ✅ v1 已完成（2026-09-09）：**确定性候选对检测 + 人工裁决**。`findContradictionCandidates` 用字符三元组 Dice 相似度（中文友好、免分词）找出事实类记忆中"高度相似但不相同"（0.35≤dice≤0.95）的对；记忆视图「🔍 矛盾扫描」按钮人工裁决归档哪条。
- **矛盾扫描 v2（LLM 语义裁决）** ✅ 已实现（2026-09-09）：`AdjudicationService` 持独立 ChatClient 实例（不与 AgentLoop 主链路抢占），判定走 fast 档；`parseVerdicts` 纯逻辑解析（jsonextract 提取 + 标签白名单 contradiction/duplicate/complement + id 双向对齐候选对 + 同对首条胜出，幻觉 id 与非法标签分别过滤/归 unknown）；UI 列表前缀标注 [矛盾]/[重复]/[互补]，AI 失败可完全降级回人工裁决。

## 七、L1「写保护引脚」实现：基准机制（2026-09-10 修正）

L1 的写保护必须回答一个问题：**当前 `core.md` 是 Agent 自己写的，还是用户改的？**
旧实现把"Agent 上次写入时刻"只记在 `MemoryManager::m_lastAgentWrite` 成员变量里，进程重启即丢失；
而 `core.md` 只在**首次为空时**被 bootstrap 写过一次（`main.cpp`）——于是从第二次启动起，
基准永远无效，`l1UserEditedRecently()` 恒为 true，**L1 提炼式改写实际上从未运行过**
（运行时佐证：`core.md` 长期停留在 63 字节的出厂默认值）。

修正后的判定（全部依据就近的持久化侧车文件 `core.md.agent-state.json`）：

| 状态 | 判定 |
|---|---|
| 基准文件缺失 | 无法确证来源 → **保守保护**，不改写；用户在记忆视图保存一次即建立基准 |
| 当前内容 SHA-256 == 基准哈希 | 自基准写入后无人改动 → **允许改写** |
| 哈希不同且文件 mtime ≥ 基准时刻 | 有人在其后手改 → **保护**，跳过本次改写 |
| 哈希不同但 mtime 更早 | 时钟回拨/外部还原，无法确证 → 按手改**保护** |

侧车文件含 `agent_write_at`（ISO 时刻）与 `content_sha256` 两个字段；每次判定重新读取
（其它实例/进程可能更新过基准）。`saveL1` 成功即自动登记基准，因此**用户在记忆视图的手改保存
也会建立新基准**——手改本身是用户行为，但它同时给出了"此后无人再改"的确认点。
一致性失效（归档被新知覆盖的 L3 旧条目）**仅在 L1 改写成功后执行**，避免出现
"旧记忆被归档、新知识没写进去"的净知识丢失。

## 八、M6-A 语义检索落地记录（2026-09-09）

- `EmbeddingClient`：同步批量嵌入（curl 直连 `/embeddings`），https + 公网端点校验（`util/NetGuard`
  严格公网判定，修复 Qt6.8 `isGlobal()` 误判 RFC1918 私有段的 SSRF 缺口，http_fetch 同步受益）；
- 嵌入配置：providers.json 顶层 `embedding` 节（enabled/provider/model），随 `writeJson` 全量回写
  防止被 saveKey/setActive 抹除；模板默认随 zhipu 启用；
- 决策：**不启用 vec0 虚表**（维度在建表时锁死、换嵌入模型需重建；个人级记忆量千条级，
  应用层余弦扫描毫秒级），`loadVecExtension` 保持加载成功即可；
- 已知边界：端到端召回质量需真实 Key 人工复核（ACCEPTANCE.md）；供应商页已提供 embedding
  配置 UI（启用开关 / 嵌入供应商 / 模型，落盘 providers.json 的 `embedding` 节）；注意
  `config/providers.template.json` 中该节默认 `enabled=false`，即**语义通道默认关闭**，
  需显式启用后才走双通道。
