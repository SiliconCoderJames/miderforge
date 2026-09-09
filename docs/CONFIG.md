# Miderforge 配置指南

## 0. 密钥卫生硬规则（开源红线，务必先读）

**永不入库清单**：真实 providers.json、任何 API Key / SMTP 授权码、数据库导出、logs/、workspace/ 产物、崩溃转储、DPAPI 密文（per-user 加密对他机无效，且暴露“此环境配置过密钥”的事实）。

- `config/providers.template.json` 必须入库，且只含占位符（`<在此填入你的Key>` 风格）——每次提交前人工过目。
- README / CONFIG.md 中所有示例一律占位符，禁止出现真实 Key 格式的字符串。
- 真实配置只放 `%APPDATA%\Miderforge\config\`，仓库只留模板。

**提交前自检命令**（贡献者必跑）：

```bat
git ls-files | findstr /i "key secret token env pass"
```

命中项须逐一人工确认为普通源码/文档、无敏感值，才允许 push。

## 1. 大模型供应商（providers.json）

配置文件位于 `%APPDATA%\Miderforge\config\providers.json`，首次运行向导会自动生成。结构：

```json
{
  "active": "zhipu",
  "providers": [
    {
      "name": "zhipu",
      "base_url": "https://open.bigmodel.cn/api/paas/v4",
      "api_key_dpapi": "<DPAPI 密文，由应用内写入>",
      "tiers": { "fast": "glm-5.3-flash", "main": "glm-5.3", "flagship": "glm-5.3" },
      "extra_body": { "thinking": { "type": "enabled" }, "reasoning_effort": "medium" }
    },
    {
      "name": "deepseek",
      "base_url": "https://api.deepseek.com",
      "api_key_dpapi": "<DPAPI 密文>",
      "tiers": { "fast": "deepseek-chat", "main": "deepseek-chat", "flagship": "deepseek-reasoner" },
      "role": "failover_backup"
    }
  ]
}
```

要点：

- **API Key 只存 DPAPI 密文**（`CryptProtectData`，per-user）。手工编辑此文件无法直接填 Key——请在应用内通过「工具 → 运行首次配置向导」输入，应用会加密落盘。
- GLM-5.3 系列必须带 `extra_body` 中的 `thinking.type="enabled"` 与 `reasoning_effort`，缺省会直接 API 报错。
- `role: "failover_backup"` 标记故障转移备胎供应商（M4 生效：当前供应商连续 2 次 429/超时/5xx 自动切换）。
- 三档 tiers：`fast`（格式化/简单任务下沉）、`main`（默认）、`flagship`（架构规划/疑难排错上浮），路由在 M4 实装。

### API Key 申请入口

| 供应商 | 入口 | 计费说明 |
|---|---|---|
| 智谱 | https://open.bigmodel.cn → 控制台 → API Key | GLM 系列按 token 计费，新用户有赠送额度 |
| DeepSeek | https://platform.deepseek.com → API Keys | 按 token 计费 |

## 2. 邮件通知（M4 实装）

协议 `smtps://`，默认模板 `smtp.qq.com:465`（可改 163/gmail）。

**授权码 ≠ 邮箱登录密码**，QQ/163 必须用授权码：

1. QQ 邮箱：网页版 → 设置 → 账号 → 开启 SMTP 服务 → 生成授权码（短信验证）
2. 163 邮箱：设置 → POP3/SMTP/IMAP → 开启服务 → 获取授权码

在「设置 → 邮件」填入 SMTP 服务器/端口/发件人/授权码/收件人，点〔发测试邮件〕验证。
中文标题使用 RFC 2047（`=?UTF-8?B?<base64>?=`）编码，正文为 UTF-8 HTML 模板。

## 3. 预算与熔断参数（M1/M4 实装）

| 参数 | 默认 | 说明 |
|---|---|---|
| 轮数上限 | 25 | 单任务最大 LLM 轮数，达到即熔断（halt） |
| 单任务 token 限额 | 500K | 达到即熔断 |
| 连续相同失败 | 3 | 连续 3 次工具报错文本相同 → 判定返工死循环，熔断 |
| L1 记忆上限 | 4000 token | 超限由 Agent 提炼式改写压缩（M2 实装） |

## 4. 沙箱与权限

- 权限三档：Suggest（默认）/ Auto Edit（工作区内写自动）/ Full Access（命令/网络全自动；**写路径与 Auto Edit 同界，仅限工作区内**）
- 命令白名单（可配置）：cmake / ninja / msbuild / git / cl / clang-format 等；白名单内的破坏性 git（reset --hard / clean -f）同样硬拦截
- 永不解禁：credential/secret/token/password 等敏感名（组件级词边界匹配，不误伤 Tokens.cpp 这类正常文件名）、.env 家族、密钥证书（.pem/.p12/.pfx）、敏感目录（.ssh/.aws/.kube/secrets/credentials）、工作区外递归删除、磁盘级操作、git push 到保护分支
- SSRF 防护：http_fetch 仅公网地址——DNS 解析后按 IP 公网性判定并钉住解析结果，重定向逐跳重新校验
- 子进程由 Job Object 管理（内存上限 2GB、单命令 120s 超时、退出连带终止），环境变量显式白名单构造（不继承父进程；身份变量 USERNAME/HOMEDRIVE/HOMEPATH/PROGRAMDATA 不透传）
- 中断分级（M5）：暂停/恢复在轮边界安全点生效；工具执行中取消经取消令牌直达进程 kill；退出/关机时在跑任务自动放回队列，重启后从断点续跑（tasks.context_json）

## 5. 语义检索（M6-A，可选）

`config/providers.json` 顶层 `embedding` 节控制 L3 记忆的语义召回；默认模板已启用并指向 zhipu
（复用其 API Key 与 base_url，**不需要额外的密钥**）：

```json
"embedding": { "enabled": true, "provider": "zhipu", "model": "embedding-3" }
```

- `provider` 必须是 `providers` 数组里已配置 Key 的供应商名；否则语义通道自动关闭（纯 FTS5 检索）
- 关闭方式：`"enabled": false`；改后重启生效
- 嵌入调用从任务收尾的检索路径发出（仅 https 公网端点，SSRF 校验与 http_fetch 同源）；
  嵌入失败不影响检索主路
- 密钥卫生：API Key 仍只以 DPAPI 密文落盘，embedding 节本身不含任何密钥
