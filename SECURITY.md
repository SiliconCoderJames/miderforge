# 安全策略

## 支持的版本

| 版本 | 支持状态 |
|---|---|
| main 分支 | ✅ 持续修复 |
| 历史 release | ❌ 请升级到最新 main |

## 报告漏洞

**请勿通过公开 Issue 报告安全漏洞。**

Miderforge 是一个会在本机执行命令、读写文件的 Agent，权限模型的缺陷属于高优先级问题。请通过邮件私下披露：

- 📧 **13371891127@139.com**
- 标题前缀：`[SECURITY]`
- 内容请含：漏洞描述、复现步骤/对抗样本、影响评估、（可选）修复建议

收到后会在 72 小时内确认，修复发布前不公开细节；修复后将在 Release Note 中致谢报告人（可要求匿名）。

## 安全设计要点（供审查参考）

- **永不解禁清单**：credential/secret/token/password/id_rsa/.env/密钥证书/.ssh/.aws/.kube 等敏感路径在任何权限档位下硬拒绝（组件级词边界匹配，不误伤 Tokens.cpp 这类正常文件名）。
- **三档权限**：Suggest / Auto Edit / Full Access。写入始终限定在工作区内；命令白名单 + 破坏性 git（reset --hard / clean -f / push 保护分支）硬拦截。
- **沙箱**：QProcess 环境白名单（不透传身份变量）、Job Object（2GB 内存上限 / 超时终止 / 退出连带终止）。
- **网络**：http_fetch 强制 TLS 证书校验；SSRF 防护 = DNS 解析后按 IP 公网性判定 + CURLOPT_RESOLVE 钉住解析结果 + 重定向逐跳重新校验。
- **密钥存储**：API Key / SMTP 授权码仅以 Windows DPAPI 密文落盘，明文只存在于内存。
- **审计**：所有工具调用、权限判定、任务状态变化双写 `logs/events.jsonl` 与 SQLite `events` 表（append-only）。
