---
name: benchmark-methodology
description: fulla 基准测试方法论——wrk Lua 线程模型、场景有效性约束、A/B 实验纪律、测量伪影分类学、性能主张的诚实呈现红线、bench 环境运维
---

# 基准测试方法论

## wrk Lua 线程模型（写场景脚本前必读）

- `setup(thread)` 只在主线程跑（每线程对象一次）；`init(args)` 在每个 worker 线程跑且整个
  脚本**重新执行**——模块级 local 在 worker 里是 nil，数据文件必须在 `init()` 里加载。
- `thread:set("k",v)` 的值在 worker 里变成全局变量。
- **同线程内所有连接共享一份 Lua 状态**（无连接亲和性）→ 带跨请求状态的多步流（S4
  auth_code：login 的 code 喂下一次 token）必须 `-t == -c`（每线程一连接），否则状态互踩。
- wrk Lua **无 SHA256/crypto**——PKCE 对预生成（`base64url(sha256(verifier))`）进文件。
- `response(status,headers,body)` 可捕获响应抽取变量存文件级 local 供下次 request 用。

## 场景有效性约束（测的是真路径才算数）

- 目标必须是 **postgres+redis 全栈**（memory 模式无用户存储，login/userinfo/refresh 不可达，
  只能测无状态 discovery/JWKS）。
- 认证头 `read("*l")` 原样读（gsub 剥空格 → 401）；S2 用 HTTP Basic 不是 body secret
  （backend-svc 声明 client_secret_basic）。
- introspect 必须用**活 token**（畸形 token 走早退快路径，吞吐虚高）；错误路径脚本
  （端点测试）不能当性能场景来源——只有成功路径的请求形状可复用。
- 种子用户不能全用 admin/admin（渐进锁定 5/10/15/20 次）；refresh 池按 VU 独立、每 token 用
  一次。
- PKCE 对所有 authorization_code 客户端强制（见 frontend-contract rule）——no-PKCE 变体需临时改
  `require_pkce`。
- docker compose 栈**不会自动 seed**（initdb 不递归子目录；MigrationRunner 只跑 schema）——
  新脚本要自己 `psql -f seed/*.sql`（带重试循环，/health/ready=200 不代表表已建）。
- 冷启动计时从 `compose up` 命令**前**起算（up 会阻塞到依赖 healthy，晚起算 = 数字虚低）。

## A/B 实验纪律

- **同机同 session 背靠背**（跨日方差可达 ±9%；竞品侧同样有）；晚间时段噪声地板差，重要 A/B
  避开。
- **bench 期间宿主机零负载**——full_test/构建/子代理/只读评审代理/grep 风暴全部禁止（实测只读
  代理造成 -3.6~+57% 摆动）。
- 跨臂实验必须带**负控**（零 PG 查询的场景做天然负控）+ **复跑臂**（排除环境快窗假阳性）。
- 抽查前对齐会话协议：显式 `export WARMUP_S=5 DURATION_S=10` 等（默认 30s 窗口会撞 TTL 雷群，
  系统性偏低）。
- 每个 A/B 臂的镜像打专用标签（`p0-ref`...），**保留到下一杠杆 A/B 完成后再退役**。
- 观察类结论的窗口必须 > 机制周期 ×1.1（如 TTL=3600 时跑 3 分钟"零衰减" ≠ 无界泄漏——先短
  TTL 验证淘汰机制再外推）。

## 测量伪影分类学（先排除再下结论）

| 伪影 | 实案 | 识别/修法 |
|---|---|---|
| 测量预算伪影 | S5 "2k 饱和"实为 rt-count 20000÷10s 窗口预算 | 池预算 ≥ 真实容量（60k）重测 |
| TTL 雷群 | 均匀轮询 × 60s TTL → 30s 周期 ~800ms p99 尖峰 | 代码层 TTL 抖动/single-flight |
| 分配器留存 | discovery 风暴后 backend RSS 不回落 | 区分泄漏 vs 留存（arena 停放布局） |
| 口径不匹配 | docker stats（全栈 RSS）≠ SDK 轻量主张 | 按主张选 PSS/口径并声明 |
| 跨日方差 | 同配置隔日 -8~9% | 只用同日对 |
| 驱动未饱和 | wrk CPU <44% → 数字是下界 | 如实标"lower bound" |
| 构建类型未声明 | ASan 镜像覆写 latest → 吞吐 86k→18k 全错 | 绝对数据必须声明构建类型；诊断构建**绝不走 compose 管道** |

## 诚实呈现红线（对外材料）

- 被证伪的主张显式关闭并留档（"zero GC jitter" 因四家同款宿主噪声尖峰关闭）；GC/尾延迟类
  主张需跨产品同机同段证据。
- 竞品数字只从 COMPARISON.md 取且带复现命令（可复现是护城河）；内存主张区分 SDK 口径与
  全栈口径；输的场景如实保留（S6 曾输 Keycloak，缓存化后反超要写清演变）。
- 竞品版本要当代稳定线 + 官方性能 flags（用旧两代版本测竞品 = 错误呈现）。

## bench 环境运维

- full_test 与 bench 串行；Windows Redis 中途死亡两次以上——挂 30s 看门狗
  （`.claude/hooks/redis-watchdog.ps1`）。
- `(wrk &)` 后台化在 wsl.exe 退出即死——用 harness 后台任务或 `nohup setsid`。
- WSL perf 被 stock 内核阻断（无 PMU）；gdb 栈采样三关：帧指针构建、`gdb -ex 'set sysroot /'`、
  attach fork 出的 worker（PID 1 是空闲监督进程）；gdb `call malloc_stats()` 会打死多线程活
  进程，LSan 在 ptrace 下直接 abort——用信号钩子替代。
- `docker ps --filter ancestor=` 会圈进同镜像的 bench 容器——按容器名精确停。
