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
  只读强类型 CborReader 消费 libcbor。`libcbor` 当前仍是
  **无条件依赖**，关闭 WebAuthn 后也保留，因为 `WebAuthnCrypto.cc`
  及其测试仍会构建。关闭特性不代表已经移除其全部第三方依赖。

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
