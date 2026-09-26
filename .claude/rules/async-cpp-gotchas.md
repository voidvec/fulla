---
description: 异步/DB 编码的实测补充坑——db-operations 规则之外的 Drogon/C++/MSVC 陷阱清单
globs:
  - "libs/**"
  - "apps/server/**"
  - "tests/**"
---

# 异步 C++ 补充坑（db-operations 规则的增量）

核心异步/DB 硬规则（async callback + Mapper + Criteria 三件套、Mapper 构造独立 try-catch、
sharedCb 模式、`[this]` 分层约定）在 `.claude/rules/db-operations.md` 与
`.claude/skills/project-conventions` —— 本文件**只收录规则没写、但实测反复咬人的坑**。

## 回调与对象生命周期

- **同一 callback 只能被 move 一次**。对同一 `std::function` double `std::move`（如错误分支与
  成功分支都 move）在 MSVC 下静默产出空回调 → 请求挂死无响应（DeviceCodeService 实案，A-2）。
  多分支共用时先 `auto sharedCb = std::make_shared<...>(std::move(cb))` 再按值捕获。
  同一调用表达式的两个兄弟 lambda 各 move 同一 callback 的形状由 CI 守卫拦截
  （`tools/arch-guard/double_move_guard.py --selftest` + 全树扫描，#183；嵌套串行链、
  互斥分支、直调 move 是合法形状不报）。
- **Drogon `Session::insert` 是 `std::map::insert` 语义：key 已存在静默不覆盖**。一切可重写
  session 值必须 `erase(key)` + `insert(...)`。新增可重写键后要 grep 全部可重写键的使用点，
  而非只看当前 diff 块。
- 会话轮换公开 API = `session->changeSessionIdToClient()`（每次新鲜认证调用；旧 id 保留 10s）。
- `execSqlAsync` 参数顺序 = `(sql, 成功cb, 失败cb, args...)`——写反成功/失败回调不会有任何
  编译期提示。

## Drogon 细节

- `authforge::drogon` 命名空间会遮蔽全局 `::drogon`：在 `authforge::*` 内写裸 `drogon::Foo`
  先解析到 SDK 适配层。**一律显式 `::drogon::` 前缀**。
- drogon `CHECK`/`REQUIRE` 测试宏不能写裸 `||`（宏展开重结合，静默错判）——拆开或套括号。
- `HttpResponse` 的 cookie 在 `getCookies()`，不在 headers。
- RedisClient 指向死端口时命令只进缓冲队列、异常回调**不触发**（重连期间）——故障注入测试须用
  stub（RedisClient 全 public 纯虚可继承）。
- `LOG_FATAL` 在 Windows 服务进程**不 abort**（只记日志）、在 Linux abort——启动路径的致命检查
  可能 Windows 假过、Linux 真死，两端都要验。

## 构建器差异（推送前自查）

- CI 三平台矩阵带 `-DFULLA_WERROR=ON` 且互为盲区：MSVC 抓 C4458（捕获遮蔽）/C4389
  （enum==unsigned）；GCC/Clang 抓 unused-function/参数；**C4819（代码页 936 无法表示字符）
  只有本机简中 Windows 会报，CI 任何腿都看不见——新写源码注释只用 ASCII**（em-dash/箭头/
  中文都算）。
- MSVC 增量构建**不感知头文件变更**（改 `.h` 后依赖方不重编 → 幽灵 405/路由未注册）。改
  `libs/*/include/**` 后用 `--clean-first` 或 touch 所有包含方。

## DB 层判定

- 本机 PG 是中文 locale：服务端错误消息输出 **GBK 字节**（`LC_MESSAGES=C` 无法覆盖安装内置
  目录），英文子串判定（"duplicate key"）失效。唯一 locale 无关判据 = 错误消息里的**约束名**
  （如 `oauth2_subject_mappings_provider_subject_key`）；drogon 异步错误路径不携带 SQLSTATE。
- `psql -c` **不做** `:变量` 插值（参数化查询必须 stdin `-tAf -` 走 `-v`）；PG 对"角色不存在"
  与"密码错误"返回同一错误（防枚举），脚本分类只能靠退出码 + SQL 结果。
- PG 对"角色不存在"与"密码错误"不可区分（见上）；OAuth2 层同理：防枚举场景统一
  `AUTH_INVALID_CREDENTIALS`（ADR-0007），不要为"用户不存在/密码错"拆分错误码。
- SchemaManager 把全部迁移包在**单事务**执行 → `CREATE INDEX CONCURRENTLY` 必败；迁移里的
  索引用普通 DDL。
