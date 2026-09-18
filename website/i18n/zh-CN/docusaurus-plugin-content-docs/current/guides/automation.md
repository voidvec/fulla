# 使用 Fulla 做自动化（无人工参与）

Fulla 说标准 OAuth 2.0，脚本、CLI 与服务间集成使用与浏览器相同的协议。
本指南覆盖两条受支持的自动化路径。让*您的用户*登录请见
[在 Fulla 上构建应用](build-an-app.md)。

## 1. 脚本与 CLI——设备授权码流（Device Flow）

适用于浏览器可能可用也可能不可用的场景（笔记本 CLI、带浏览器的 CI 任务、
终端设备）。

1. 请管理员（或在已启用自助的部署中通过 门户 → **我的应用** 自行注册）
   为工具创建客户端。设备流需要
   `urn:ietf:params:oauth:grant-type:device_code` 授权类型。
2. 发起流程：

```bash
curl -X POST https://your-fulla.example/oauth2/device_authorization \
  -d "client_id=YOUR_CLIENT_ID"
# -> { "device_code": "...", "user_code": "ABCD-EFGH",
#      "verification_uri": "https://your-fulla.example/oauth2/device",
#      "interval": 5, "expires_in": 600 }
```

3. 向用户展示 `verification_uri` + `user_code`；用户在浏览器中批准。
4. 轮询令牌端点（遵守 `interval` 与 `slow_down`）：

```bash
curl -X POST https://your-fulla.example/oauth2/token \
  -d "grant_type=urn:ietf:params:oauth:grant-type:device_code" \
  -d "device_code=DEVICE_CODE" \
  -d "client_id=YOUR_CLIENT_ID"
```

等待期间返回 `authorization_pending`；批准后获得 `access_token`
（配 `offline_access` 类 scope 时附 `refresh_token`）。

## 2. 服务间——client_credentials 授权

适用于无用户上下文的机器集成（无终端用户授权、无 OIDC 身份）。
需要授权类型含 `client_credentials` 的 **CONFIDENTIAL** 客户端。

```bash
curl -X POST https://your-fulla.example/oauth2/token \
  -u "YOUR_CLIENT_ID:YOUR_CLIENT_SECRET" \
  -d "grant_type=client_credentials" \
  -d "scope=read"
```

要点：

- 得到的令牌代表*客户端*而非任何用户（`sub` 为客户端身份）。
- scope 受客户端已登记 scope 限制；只申请所需的最小集合。
- 在管理界面（自助应用在 门户 → 我的应用）轮换密钥；轮换后旧密钥立即失效。

## 为什么没有个人 API 令牌

个人长期 API 令牌会绕过标准授权已有的 scope 治理、吊销与审计路径。
设备流 + client_credentials 覆盖同样的场景且安全特性更好；权衡讨论见
`docs-local/productization-evolution/` 的 v1.4.0 设计笔记。
