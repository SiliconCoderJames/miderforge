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

## 地图确认时已定的方案（你已确认"地图无误"）
- P0-2：把应用自身数据目录（config/ DPAPI 密文、miderforge.db、memory/、logs/、miderforge.lock）
  加入读取侧保护区；read_skill 走 skills/ 子树豁免；session_search 无路径参数不纳入路径清单；
  search_files 根路径真实解析 + 枚举结果逐项过滤敏感名并计数。
- ctest 拆分：tests/adversarial/ 独立可执行，按 TEST_SUITE 拆独立 ctest 项（名字含 adversarial）。
- 新事件六元组以 payload JSON 字段落库（ts 已有列），不改 events 表结构。
