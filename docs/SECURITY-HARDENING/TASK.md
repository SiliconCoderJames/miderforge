@分析GitHub开源项目现状 你在哪、做什么
你在 dsh 会话中工作，工作目录即 MiderForge 仓库根；dsh 替你读文件、跑命令、
回喂结果，你负责决策与写码。MiderForge 是 C++/Qt6 桌面 AI Agent
（CMake + vcpkg + doctest + ctest），带持久化记忆 / 技能自沉淀 / 供应商
failover——你这一轮加固的正是这些能力的安全边界。

使命：修三个已确认的越界漏洞（ACCEPTANCE.md 有案），补持久化层投毒防护，
把对抗测试固化为回归资产。完成后仓库比开始时更难攻破。

四条不变量（其余——实现选型、代码组织、测试写法、任务顺序微调——
全由你判断，我信任你的工程品味，选错有验收测试兜底）

安全只收紧、覆盖只增：HardDeny 降档、白名单加条目、永不解禁清单减项、
路径判定退回纯字符串比较、新增绕过权限判定的调用点——任何一条出现在
diff 里都是方向错误。
对抗测试是资产：用例必须真实生效——注册进 ctest、断言真实执行、
环境不支持时显式失败而非静默跳过。测试红→修实现，不是修测试。
不确定就问：要越出任务边界、需求互相打架、需要做安全取舍时，停下来问我。
我不怕被打断，怕被瞒。
结论带证据：报"通过"必附我能一条命令复核的证据；没验证的行为标
"未验证"，不包装成完成。
工程惯例：分支 security/hardening-p0；每任务独立 commit，且该 commit 上构建
与 ctest 全绿；commit 格式 [security] P0-1: <一句话>。你只写代码、测试、
STATUS.md 与 TASK.md；ACCEPTANCE.md / docs/SECURITY.md / README 由我维护。

第 0 步：绘制安全地图（唯一强制起点，做完停下等确认）
先把本任务书原文存为 docs/SECURITY-HARDENING/TASK.md（此后只读）。然后读
ACCEPTANCE.md、docs/SECURITY.md、README 权限章节，给我一份《安全地图》：

各安全职责落在哪些文件与函数：权限档位判定 / 工作区校验 / 命令白名单 /
永不解禁清单 / events 审计 / L1 收尾改写 / SKILL 固化 / 供应商 failover
read / write / 命令执行三条调用链：入口 → 判定 → 执行
全部读取与枚举类工具（list_dir / glob / grep 等——它们和 read_file 同为
信息泄露面，P0-2 一并覆盖）
你发现任务书与仓库现实不符之处，直接指出，我改任务书
等我说"地图无误"再写代码。后续所有验收都锚在这份地图上。
P0-1 工作区边界：reparse point 越界
问题：pathInWorkspace 只做 QDir::cleanPath 字符串比较，junction/symlink
已实测越界。
要达到的最终状态（实现自选——Qt / Win32 / std::filesystem 任意组合，
选你认为对的）：

任何路径形式（junction、symlink、多层 …\、\?\UNC、大小写混淆）指向
工作区外，在写入口被拒
判定基于解析后的真实路径；解析不了的按越界处理
旧字符串判定在安全调用路径上零残留（grep 可证；真迁不动的列进
STATUS.md 等我裁决）
验收（tests/adversarial/WorkspaceBoundaryTest.cpp，真实创建 reparse point）：
两层断言——写入口返回拒绝 + 目标文件在磁盘上不存在。至少覆盖上述五种
形式，欢迎加你想得到的变体。
P0-2 读取通道的永不解禁
问题：read_file 三档全 Allowed，.ssh 私钥、DPAPI 密文均可读。
最终状态：永不解禁清单同样约束 read_file 与地图里列出的所有读取/枚举工具，
任何档位拒绝，记 read_denied 事件（走现有 events 通道）。
验收：fixture 放测试目录内（不碰真实用户目录），断言入口拒绝 + 事件落表 +
返回内容不含文件内容。

P0-3 命令白名单：任意代码执行通道
问题：cmake -P、git !alias、git log --exec 都能执行任意脚本，绕过全部既有防护。
最终状态：

白名单收敛到"子命令 + 参数模式"粒度，cmake / git 的脚本执行类参数显式拒绝
拒绝发生在执行前（参数级拦截），记 command_denied 含完整命令行
白名单配置只许收敛；需要新增任何命令或参数模式，停下来问我
验收（tests/adversarial/CommandWhitelistTest.cpp）：cmake -P evil.cmake、
git !rm -rf ~、git log --exec=“curl http://x” 三例执行前拦截，并用 canary
证明命令真的没跑——把攻击命令设计成"若执行会在测试目录留下文件"，
断言该文件不存在。
（你为构建测试自己跑的 cmake / ctest / git 是开发命令，与运行时白名单无关。）
P1-4 对抗套件固化
tests/adversarial/ 收齐 P0 用例，ctest -N 全部可见；CONTRIBUTING.md 写明
该目录用例不许删、不许 skip。

P1-5 持久化层投毒防护（三场景各配对抗用例，复用现有 doctest/mock 基建）
a) L1 收尾改写：改写内容含外发 URL 或凭据特征时记 memory_rewrite_flagged，
events 留新旧 diff 摘要
b) SKILL 固化门槛：任务"成功"但期间有越界事件（boundary / command /
read_denied）→ 不固化，记 skill_solidify_denied
c) failover 权限联动：供应商切换发生时 Full Access 自动降为 Auto Edit，
记 permission_downgrade_on_failover
新事件一律走现有 events 基础设施，六元组齐全（actor / authorizer / target /
operation / outcome 含原因码 / timestamp），不另起平行日志。

P1-6 SECURITY-CRITICAL.txt
地图确认后的安全关键文件清单入库。CONTRIBUTING.md 补两条：触及清单的 PR
须人工 review；不许改清单把文件移出去。

检查点（只有三个，之间自主推进、自主排障）
地图确认 → 开工
P0 收尾：交证据包——各任务 commit hash、ctest -R adversarial 全绿输出、
白名单配置 diff——我确认后你再进 P1
P1 收尾：证据包 + STATUS.md 收尾（进度 / 遗留 / 待裁决项，每任务完成时更新）
我会自己跑的独立复核（你不用替我准备）
ctest -N 数 adversarial 用例；ctest --preset win64-release -R adversarial；
git diff main…security/hardening-p0 – 白名单配置（只删不增）；grep 旧判定
函数残留。所有验收都落在可观察的物理结果上——磁盘、ctest 输出、diff——
不依赖任何人的口头申报。

从第 0 步开始。
