# STATUS — security/hardening-p0

进度 / 遗留 / 待裁决项（每任务完成时更新）。任务书见 docs/SECURITY-HARDENING/TASK.md（只读）。

## 环境与基线备注
- 仓库曾被移动：旧 build/ 缓存指向 e:/FILE/APP/MiderForge（无 Miders 段），已废弃重建。
- 本机构建配方（离线）：VS2026 自带 vcpkg 工具链 + \`-DVCPKG_MANIFEST_INSTALL=OFF\`
  （本机连不上 github，vcpkg manifest 对账失败）+ \`-DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64\`，
  复用 build/vcpkg_installed 既有依赖树。使用者若在有网环境，仍可 \`cmake --preset win64\`。
- 基线（改动前）：cmake build mider_tests + ctest --preset win64-release → mider_unit_tests Passed (12.0s)。
- 工作树存在**非本任务**的未提交改动（不归我动，未纳入任何提交）：
  \`M src/app/SidebarView.h\`、\`M src/main.cpp\`、\`?? design/\`。

## P0-1 工作区边界：reparse point 越界（完成）
- 改动：
  - 新增 \`src/util/PathReal.{h,cpp}\`：逐组件真实路径解析——已存在组件经
    GetFinalPathNameByHandleW 展开 junction/symlink/挂载点；首个不存在组件之后词法拼接；
    悬空 reparse point、拒绝访问、根不可达一律 fail-closed 按越界。
  - \`PermissionGate\`：删除 \`pathInWorkspace\` 纯字符串判定（grep 可证零残留，见下），
    WriteFile 区内判定与递归删除的绝对路径判定改为 resolveReal + isInsideOrEqual。
  - \`FileTools::resolveWorkspacePath\`：不再 QDir::cleanPath 词法折叠——
    "ws/j/../x" 曾被折叠成区内 ws/x，权限门与落盘复核都在判定一条与 OS 语义不同的路径
    （对抗用例红出后修正；只归一分隔符，".." 交 resolveReal/QFile 原生语义）。
  - \`FileTools::write_file\` 落盘前独立复核真实路径边界（纵深第二层，防未来绕过权限门的调用点）。
  - 新建 \`tests/adversarial/\`：adversarial_main.cpp + WorkspaceBoundaryTest.cpp，
    7 用例 / 56 断言：junction、目录 symlink、悬空文件 symlink、junction+多层 ..、链式 junction、
    多层 .. 上穿、\\?\\ 与 \\?\\UNC、大小写混淆、相对路径；两层断言（写入口拒绝 + canary 不落盘）；
    另有反过度封锁（区内普通写/深层新目录/区内 ../根大小写混淆保持放行）与
    "绕过权限门直调 handler 仍被拦"用例。注册为 ctest 项 \`adversarial.workspace_boundary\`。
  - \`tests/test_permission.cpp\`：4 个用 fictitious 工作区的用例迁至 QTemporaryDir 真实目录
    （判定改真实路径后，虚构路径 fail-closed 拒绝，旧断言不再成立；用例语义意图未变、未删未 skip）。
- 证据（本机实测）：BUILD_EXIT=0；\`ctest --preset win64-release\` → 2/2 Passed
  （mider_unit_tests 11.9s + adversarial.workspace_boundary 0.9s，7 用例 56 断言全过）。
- 遗留 / 说明：
  - symlink 用例需开发者模式或管理员特权；无特权环境会**显式 FAIL**（任务书要求：不静默跳过）。
  - TOCTOU：判定与落盘之间的链接竞态窗口仍在（攻击者需本机并发改文件系统）；
    落盘前复核已收窄窗口，根治需写时打开改用 reparse 语义（留 Roadmap）。
  - workspaceRoot 自身解析失败 → 一切写按越界拒绝（fail-closed）；空工作区根同样拒绝一切写。

## P0-2 读取通道：永不解禁 + 读取侧保护区（完成）
- 改动：
  - \`AppContext::protectedReadRoots\`：读取侧保护区真实路径根列表（运行时注入，测试可注入临时目录）。
  - \`main.cpp\`：注入应用自身数据根（解析后真实路径）：config/（DPAPI 密文）、miderforge.db、
    memory/、logs/、miderforge.lock；skills/（技能加载通道）与 workspace/ 不在列。
  - \`PermissionGate::hardDenyReason\`：ReadFile 增保护区判定——resolveReal 后逐根 isInsideOrEqual，
    junction/symlink 指进保护区同样命中；列表为空零开销直通（单测场景）。任何档位（含 FullAccess）拒绝。
  - \`AgentLoop::classifyTarget\`：read_skill 判定对象改为实际读取路径 skills/<name>/SKILL.md
    （名字清单与保护区对全部读取通道一视同仁；正常技能名不受影响）。
  - deny 事件按通道分流：读通道 \`read_denied\`，其余仍 \`permission_deny\`；六元组入 payload
    （actor/authorizer/target/operation/outcome/reason；ts/task_id 走 events 列），
    自动执行与手动重跑（rerunTool）同口径。
  - \`FileTools\`：list_dir/search_files 枚举结果逐项过滤（命中清单/保护区只隐名并计数），
    envelope 增加 \`skipped_protected\`；search_files 的内容正则在过滤之后，读不到保护文件字节。
- 测试（tests/adversarial/ReadChannelTest.cpp，ctest 项 \`adversarial.read_channel\`，6 用例 52 断言）：
  - 门槛三档拒绝（保护区 + 名字清单回归 + 正常文件保持可读）。
  - list_dir/search_files 零泄露（命中项不出现、skipped_protected 计数、返回文本不含 canary）。
  - read_skill 判定对象路径组合与 AgentLoop 同构（credentials 命中拒，正常技能放行）。
  - 端到端：本地 mock SSE 驱动真实 AgentLoop+EventBus+Database——模型请求读保护文件 →
    工具以失败回填且不含文件内容 → events 表出现 read_denied 行且六元组齐全 → JSONL 双写留痕。
  - 隔离：fixture 全在 QTemporaryDir；QStandardPaths 测试模式把 appdirs 隔离到 qttest
    （adversarial 入口统一启用），provider 配置/假 Key（DPAPI）不触碰真实用户目录。
- 证据（本机实测）：BUILD_EXIT=0；\`ctest --preset win64-release\` → 3/3 Passed
  （mider_unit_tests 11.1s + workspace_boundary 0.2s + read_channel 0.2s）；对抗可执行连跑 8/8 全绿。
- 遗留 / 说明：
  - 保护区根在启动时解析一次；运行中更换数据目录需重启生效（v1 可接受）。
  - 【已修复，见 P0-fix】对抗套件曾出现偶发 fastfail/挂起（read_channel 端到端约 1/6），
    根因见下节，非本任务改动引入。

## P0-3 命令白名单：任意代码执行向量拦截（完成）
- 改动（\`PermissionGate.cpp\`，第一层；\`CommandTools.cpp\` 第二层同源复用）：
  - cmake：\`-P\`（含附着形式 \`-Pscript.cmake\`）执行脚本——脚本内 execute_process/file(WRITE)
    任意执行/写；\`-E env\` / \`-E chdir\` 借改环境/换目录执行外部命令。常规构建形态
    （-S/-B/--build/--target）不受影响。
  - git：任何 \`!\` 开头词元（别名注入，含任务书原样向量 \`git !rm -rf ~\`）、\`-c\` 内联配置、
    \`--exec\` / \`--exec=\`（rebase、am 的 exec 语义）、\`--exec-path\`（伪造子命令查找目录）、
    \`config\` 写入形态（alias.*/core.pager/core.fsmonitor/core.sshCommand 注入；只读
    --get/--get-all/--get-regexp/--list 放行）、\`filter-branch\`、\`submodule foreach\`、
    \`rebase|am -x\`。\`git --version\`/status/log/diff/add/commit 与 \`git config --get\` 保持放行。
  - \`CommandTools::run_command\` 第二层：复用 \`hardDenyReason(Kind::RunCommand,…)\`，
    绕过权限门直调 handler 同样拦截；拒绝发生在 spawn 之前（子进程从未启动）。
  - 事件：命令通道拒绝改记 \`command_denied\`（自动执行与手动重跑同口径），target=完整命令行，
    六元组沿用 P0-2 结构；读/写/网络通道不变。
  - 精度修正（本任务暴露的旧误报）：磁盘级操作判定由正则改为令牌级——旧 \\b(format)\\b 把
    **clang-format** 误杀（对抗用例红出）；真正会执行磁盘操作的只有独立 format/format.com/
    diskpart 令牌与 cipher /w 参数，令牌级全数保留拦截。
- 测试（tests/adversarial/CommandWhitelistTest.cpp，ctest 项 \`adversarial.command_whitelist\`，
  6 用例 / 66 断言）：
  - 任务书原样向量：\`cmake -P evil.cmake\`、\`git !rm -rf ~\`、\`git log --exec=\"curl http://x\"\`
    均三档拒绝；handler 层直调同拒。
  - canary 证据：evil.cmake 一旦被任何通道执行即写出 canary 文件——全部向量尝试后
    canary 不存在、工作区除 fixture 的 evil.cmake 外无任何执行产物。
    （开发过程曾真实抓到一次 canary：检查块误嵌进递归删除分支导致漏拦，用例红出后修正位置。）
  - 反过度封锁：cmake -S/-B/--build、git 常规与 config 只读、ninja/msbuild/clang-format/
    where/echo 全部保持放行。
- 证据（本机实测）：BUILD_EXIT=0；\`ctest --preset win64-release\` → 4/4 Passed
  （unit 11.8s + workspace_boundary 0.9s + read_channel 0.2s + command_whitelist 0.03s），
  连续三轮 100%。
- 遗留 / 说明：
  - 白名单只收紧不放宽：本轮对白名单的净变化是**新增两类拒绝向量**；无任何新增放行。
  - 已知残余：git commit/merge 会触发仓库钩子（.git/hooks 属工作区内可写文件，写边界管不住
    其内容）；钓出钩子执行需勾子语义审查，留 Roadmap。

## P0-fix 偶发挂起/崩溃根因修复（checkpoint 后）
- 现象：SSE 流式测试偶发挂起（mock 建连后不回响应，等待超时红测），此前同一场景
  还以 fastfail（0xc0000409）形态出现过；两套测试（unit/adversarial）各自约 1/6 频率。
- 根因（压测 + 全链路打点抓到现场）：两处 MockSseServer 的已回应集合 \`QSet<QTcpSocket*>`
  只增不清。上一条连接的 socket 断开后 deleteLater 释放，下一条连接的新 socket 极大概率
  复用同一地址 → \`m_replied.contains(sock)\` 命中陈旧指针 → 静默早退、永不回响应。
  只咬第 2+ 条连接（重试连接/端到端第 2、3 连）与堆布局敏感两个特征完全吻合。
- 修复：
  - tests/test_mockstream.cpp 与 tests/adversarial/mock_sse_server.h：socket \`destroyed\`
    信号即从集合摘除标记，杜绝陈旧指针误配。
  - src/llm/ChatClient.cpp：析构先 \`cancelActive()\` 再 quit+wait——原实现 curl 仍阻塞在
    工作线程槽里时 quit 排不上队，10s 超时走"故意泄漏"路径，进程带着活线程退出偶发
    fastfail。挂起场景因此从崩溃变成干净的超时红测，才得以抓到根因。
- 证据：修复后对抗 30/30、单测 20/20 连续压测全绿；全量 \`ctest --preset win64-release\`
  4/4 Passed 连续两轮。

## P1 持久化投毒防护 + 对抗资产固化

### P1-5 三场景（src 接线 + tests/adversarial/PersistencePoisoningTest.cpp，
ctest 项 `adversarial.persistence_poisoning`，4 用例 / 71 断言）
- a) L1 收尾改写筛查：`MemoryManager::l1RewriteSuspicious`（外发 URL + 六类凭据特征：
  sk- 前缀/AKIA/Bearer/32 位十六进制/40 位基地串/api_key= 形态）命中即拒绝落盘（旧 L1 原样），
  记 `memory_rewrite_flagged`（actor=agent / authorizer=memory_guard / target=core.md /
  operation=l1_rewrite / outcome=blocked / reason + **新旧 diff 摘要** `l1DiffSummary`：
  字节/行数、±行统计、新增首行样本）；一致性失效跳过原因同步扩了 `poisoned` 口径。
- b) 技能固化门槛：AgentLoop 计数本任务越界/拒绝事件（自动执行拒绝、确认卡用户拒绝、
  手动重跑拒绝三处接线），收尾时"成功但越界"→ 不提案、记 `skill_solidify_denied`
  （authorizer=skill_guard，含 boundary_denies 计数）；干净任务照常 skill_gen + 提案。
- c) failover 权限联动：`switchToFailover` 成功后 Full Access 自动降 Auto Edit，
  记 `permission_downgrade_on_failover`（actor=system，outcome=full_access->auto_edit）。
- 反过度封锁断言：正常中文开发笔记（cmake/ctest 命令行等）不触发筛查；emoji（增补平面
  代理对）放行口径沿用；干净任务的固化路径不受门槛影响（skill_gen 正常发出）。
- 证据：新套件单独压测 **10/10**；端到端断言物理结果——core.md 磁盘内容与拦截前逐字节一致、
  evil-skill 目录不存在、权限档真降为 AutoEdit。

### P1-4 对抗资产固化
- CONTRIBUTING.md 新增硬约束：`tests/adversarial/` 用例不许删、不许 skip、
  `ctest -N` 必须全量可见；安全行为变更必须带对抗用例。
- 现状：4 个对抗套件全部 `ctest -N` 可见（workspace_boundary / read_channel /
  command_whitelist / persistence_poisoning），名字一律含 adversarial。

### P1-6 SECURITY-CRITICAL.txt
- 仓库根入库 16 项安全关键文件（权限门/真实路径/文件与命令工具/投毒筛查/审计双写/
  NetGuard/DPAPI/模板配置/tests/adversarial/），每项附理由；CONTRIBUTING.md 同步两条
  规则：触及清单须人工 review、清单只许追加不许移出。

### P1 验证与遗留
- 全量 `ctest --preset win64-release` → **5/5 Passed**（unit 12s + 4 对抗套件 17.5s）。
- 遗留 / 待裁决：
  - failover 降级后设置页组合框在下次打开时才显示新值（AppContext 非 QObject，
    无信号可订阅；改 QObject 加 signal 属跨层重构，留 Roadmap）；
  - l1RewriteSuspicious 的凭据正则刻意保守（高置信形态），若现误杀再按例收窄；
  - P1 期间新增放行：无（白名单与拒绝面净收紧）。

## 地图确认时已定的方案（你已确认"地图无误"）
- P0-2：把应用自身数据目录（config/ DPAPI 密文、miderforge.db、memory/、logs/、miderforge.lock）
  加入读取侧保护区；read_skill 走 skills/ 子树豁免；session_search 无路径参数不纳入路径清单；
  search_files 根路径真实解析 + 枚举结果逐项过滤敏感名并计数。
- ctest 拆分：tests/adversarial/ 独立可执行，按 TEST_SUITE 拆独立 ctest 项（名字含 adversarial）。
- 新事件六元组以 payload JSON 字段落库（ts 已有列），不改 events 表结构。
