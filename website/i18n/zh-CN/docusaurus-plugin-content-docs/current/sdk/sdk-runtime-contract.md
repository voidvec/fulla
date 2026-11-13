# SDK Runtime Contract

尚未发布的 S03 刷新加固增加了仓储迁移要求，见
[刷新令牌与撤销生效边界](../domains/token-refresh-and-revocation.md)。
仅实现旧 ITokenRepository 接口的仓储仍可编译，但在实现新的查询、原子轮换与确认撤销端口前，
刷新会失败关闭。

对外契约声明：Fulla SDK（`fulla::common` / `fulla::oauth2` /
`fulla::identity` / `fulla::storage-*` / `fulla::drogon`）在 v1.x
期间对消费者承诺的线程模型、ABI、异常、日志与依赖边界。

本文档是对外承诺的单一出处；SDK 头文件注释与本文冲突时以本文为准并修头。

---

## 1. 线程模型

- Domain 服务（`libs/oauth2`、`libs/identity`）**不自持事件循环**；所有异步
  操作经回调返回。
- **回调可能在任意 Drogon IO 线程触发，不保证是调用线程**。消费者不得假设
  线程亲和性；需要回到特定线程时由消费者自行投递。
- 只读单例（如 `JwkManager`）遵循 **init-once-then-read-only**：在服务开始
  接受请求前完成一次性 `init()`，之后以 `shared_ptr<const T>` 发布，运行期
  不再变更。消费者在 SDK 装配完成后并发读取是安全的。
- 服务对象持有 `shared_ptr` 仓储句柄保证生命周期；异步续体一律捕获
  `auto self = shared_from_this()`，禁止 `[this]` / `[&]`。

## 2. ABI 稳定性

- v1.x **仅支持 `find_package` 源码集成，不承诺二进制 ABI**。
- 语义化版本只覆盖**源码级 API**：公共头 `include/fulla/**` 遵循
  SemVer，破坏性变更必须升 major（由 api-diff 工具在 CI 强制）。
- 跨编译器 / 跨 STL 混用预编译二进制不在支持范围；进入 Conan 二进制包
  分发阶段后再单独制定 ABI 策略。
- 弃用流程：`[[deprecated]]` 标注 + 至少一个 minor 周期过渡后方可移除。

## 3. 异常安全约定

- Domain 公共 API 以 `Result<T, Error>` 返回**可预期错误**，不用异常表达
  业务失败。
- 仅在不可恢复的编程错误（契约违反、断言级问题）抛异常。
- 存储底层异常（如 `DrogonDbException`）**必须在 Adapter 层
  （`libs/storage-*`、`libs/drogon`）捕获并转为 `Error`，不得穿透到
  Domain 回调**。消费者在 Domain 回调内无需 try/catch 存储异常。

## 4. 日志抽象

- Domain 代码经 `common::ports::ILogger` 端口输出日志，**不直接使用
  Drogon `LOG_*` 宏**（arch-guard 强制 Domain 层不 include drogon 头）。
- SDK 默认提供 Drogon 日志适配实现（`libs/drogon` Adapter）；脱离 Drogon
  宿主的消费者可注入自己的 `ILogger` 实现替换。

## 5. 依赖声明

- `with_social` / `with_webauthn` 映射到 CMake 的 `WITH_SOCIAL` /
  `WITH_WEBAUTHN`，裁剪对应服务与 Controller 的编译面。
- `with_identity=False` / `WITH_IDENTITY=OFF` **不受支持**：产品与
  Postgres 适配器仍需要 `fulla::identity`，配置阶段会明确报错；
  配置仓库时应保持 identity 启用。
- WebAuthn 自 v1.1.0（#142）起实现真实 W3C Level 2 CBOR/COSE 和 ES256 验证，
  只读强类型 CborReader 消费 libcbor。关闭 WebAuthn 后，Conan 不再要求 libcbor，
  CMake 不再查找/链接它，相关服务、加密实现与 CBOR 测试不参与编译，导出的 SDK
  配置也不再要求它。OpenSSL 与 jsoncpp 仍是其他特性的共享依赖。

### 5.1 受支持的组合

所有受支持配置都保持 identity ON。两个可选开关默认均为 ON，应使用对应 Conan
选项，不要只覆盖 CMake 开关。

| Social | WebAuthn | 编译面与依赖边界 |
|---|---|---|
| ON | ON | 完整 provider/WebAuthn 服务与 Controller，需要 libcbor |
| OFF | ON | 去除 provider 登录/绑定路由与 social 服务，保留 WebAuthn/libcbor |
| ON | OFF | 去除 WebAuthn 路由、实现和测试，不再要求 libcbor |
| OFF | OFF | 保留核心 identity/MFA/TOTP/OAuth2，不要求两组可选服务或 libcbor |

Social OFF 仍保留 `GET /api/auth/providers`，返回空 provider 列表；共享的
`POST /api/auth/providers/{provider}/authorize` 路由也保留，在不可用时返回 404。
关闭 social 不承诺移除共享 HTTP 或存储适配器。头文件继续安装；哪些声明可用仍由
既有 `WITH_*` guards 决定。OFF 库不提供被关闭服务的实现。应链接 SDK imported
targets，使用其导出的 feature 定义，不要
自行定义 `WITH_*`。

编译开关不会删除已保存的 WebAuthn 凭据或社交绑定。部署裁剪变体前，应给依赖
被关闭特性的账号准备可用登录方式；不会自动迁移认证因子或数据。

仓库中的 Conan lock 是受支持配置的超集；libcbor 条目继续锁定 ON 构建，OFF 图
不消费该条目。构建树和安装树消费者都必须解析所选依赖闭包。不同 feature 配置
之间不承诺二进制 ABI 兼容。

### 5.2 复现矩阵检查

按[测试指南](../contribute/testing-guide.md)准备隔离的 PostgreSQL/Redis fixture，
沿用普通测试入口的 `FULLA_DB_*` 与 `FULLA_REDIS_*` 环境覆盖。然后运行一个组合
（需要 Python 3.10+、Conan、CMake 与原生工具链）：

```bash
python3 tools/feature-matrix/run.py \
  --social ON --webauthn OFF \
  --build-dir build/social-on-webauthn-off \
  --evidence-dir docs-local/feature-matrix/social-on-webauthn-off
```

每次尝试使用新的证据目录，不覆盖已有结果；消费者/安装目录也必须全新，若上次
尝试已生成它们，应选择新的构建目录。其余三个组合依次重复。脚本解析
锁定依赖图，以 `FULLA_WERROR=ON` 构建，检查配置源文件/flags/exports，运行适用的
完整 CTest（含完整服务 binary），并从构建树及新安装前缀分别构建/运行 SDK 宿主。
OFF 消费者显式禁找 libcbor，包目录和 imported target 路径也要匹配预期前缀。
必需测试须已注册且通过，不能有 CTest skip，并验证 PG/Redis 实际就绪；不支持的
identity OFF 组合必须在 Conan 和 CMake 两处失败。
既有 gtest/example 警告策略保持，Werror 应用于使用项目 warnings helper 的目标。

全开构建保留原 OAuth2/Admin 端点脚本；裁剪构建改跑独立 feature-surface 进程探针，
检查公共端点并断言被裁剪路由返回 404；启用 social 时仍运行浏览器绑定 state 的
独立进程检查。默认完整 route golden 和 OpenAPI fingerprint 原文不变，仅裁剪
构建的预期集合按开关缩减。Optional Feature Matrix CI 跑三个裁剪 Linux 组合，
普通跨平台 CI 覆盖 ON/ON。

## 6. 插件注册与配置契约（宿主集成）

- `OAuth2Plugin` 类名与 config `plugins[].name` 反射加载契约**保持稳定**
  （方案 A）：各 config（`config.{json,dev,ci,prod,bench}.json`）中
  `"plugins":[{"name":"OAuth2Plugin","config":{...}}]` 的类名字符串与
  `config{}` 块 schema 是产品配置契约的一部分，v1.x 不改名。
- `fulla::drogon` 当前是包含插件源文件的 **STATIC 库**；原独立
  OBJECT 库已被合并。
- 宿主必须保留实际使用的 Controller/插件注册代码。参考
  `examples/full-stack-host/`：编译产品 bootstrap 源文件、显式注册
  Controller 并装配插件依赖。配置中的类名字符串本身不能强制链接器
  抽取静态库目标文件。
- 仅依赖未引用静态自注册的封装须显式保留相关目标文件（如在适用链接器上
  使用 whole-archive），并自行验证注册冒烟测试。
- HTTP 宿主集成见 `examples/full-stack-host/`；仅引擎集成见
  `examples/third-party-host/`。
