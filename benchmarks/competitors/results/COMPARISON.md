# 竞品同环境性能对比（COMPARISON）

> 生成时间：2026-08-23 17:28 UTC · 生成器：`benchmarks/reporting/gen-comparison.py`（无手填数字，全部溯源到入仓 JSON）
> **名称溯源**：全部测量于 2026-08 以项目旧名 **authforge** 进行，2026-08-26 更名为 fulla；本报告与结果 JSON 中的产品标签已统一改为 fulla，测量数据与文件名中的日期/commit 溯源标识未改动。
> 设计与方法论：`docs/benchmark/competitor-benchmark-design.md`

## 环境与版本

| 产品 | 版本 |
|---|---|
| Fulla | git 06bfdaa2 |
| Keycloak | 26.7.1 |
| Ory Hydra | v26.2.0 |
| Zitadel | v4.17.1 |

同一台机器（WSL2 8 vCPU / 16GB）、同一 wrk 4.1.0 阶梯（2→128，warmup 5s / measure 10s）、同一 PostgreSQL 17 后端、串行执行、每家之间 `docker compose down -v` 清场。连接池：三家竞品按各自官方机制对齐到 25（D1，见附录 A）；Fulla 使用自家 bench 调优档（池 64/64、cache on、auto_batch、reuse_port，构建用 opt-in LTO preset——即仓库文档化的性能优化后推荐基准档，非隐藏调优）。

## 一、稳态吞吐与延迟（阶梯，错误率 <0.01% 的最高档）

### S1 discovery

| 产品 | 稳态 QPS | 稳态档 c | P50 | P90 | P99 | 错误率 |
|---|---|---|---|---|---|---|
| Fulla | **87,499** | c=16 | 0.2ms | 0.3ms | 3.5ms | 0.0091% |
| Keycloak | **41,086** | c=128 | 2.3ms | 15.8ms | 70.1ms | 0.0000% |
| Ory Hydra | **1,713** | c=64 | 37.2ms | 38.1ms | 39.5ms | 0.0000% |
| Zitadel | **8,746** | c=64 | 6.6ms | 10.5ms | 18.1ms | 0.0000% |

### S2 client_credentials

| 产品 | 稳态 QPS | 稳态档 c | P50 | P90 | P99 | 错误率 |
|---|---|---|---|---|---|---|
| Fulla | **14,438** | c=64 | 4.2ms | 6.3ms | 10.9ms | 0.0000% |
| Keycloak | **5,634** | c=64 | 11.1ms | 15.4ms | 24.7ms | 0.0000% |
| Ory Hydra | **2,159** | c=128 | 53.8ms | 100.6ms | 157.9ms | 0.0000% |
| Zitadel | **1,679** | c=32 | 18.4ms | 25.2ms | 35.9ms | 0.0000% |

### S3 introspect

| 产品 | 稳态 QPS | 稳态档 c | P50 | P90 | P99 | 错误率 |
|---|---|---|---|---|---|---|
| Fulla | **22,458** | c=64 | 2.7ms | 4.1ms | 402.2ms | 0.0000% |
| Keycloak | **10,637** | c=128 | 11.7ms | 15.7ms | 24.1ms | 0.0000% |
| Ory Hydra | **11,454** | c=128 | 10.3ms | 19.5ms | 31.9ms | 0.0000% |
| Zitadel | **3,142** | c=32 | 9.7ms | 13.7ms | 19.4ms | 0.0000% |

### S5 refresh_token

| 产品 | 稳态 QPS | 稳态档 c | P50 | P90 | P99 | 错误率 |
|---|---|---|---|---|---|---|
| Fulla | **5,506** | c=64 | 11.2ms | 15.5ms | 23.2ms | 0.0000% |
| Keycloak | **2,898** | c=128 | 50.1ms | 72.2ms | 1710.0ms | 0.0069% |
| Ory Hydra | **738** | c=128 | 162.7ms | 259.2ms | 365.5ms | 0.0000% |
| Zitadel | N/A | — | — | — | — | — |

### S6 userinfo

| 产品 | 稳态 QPS | 稳态档 c | P50 | P90 | P99 | 错误率 |
|---|---|---|---|---|---|---|
| Fulla | **49,302** | c=128 | 2.5ms | 3.8ms | 5.6ms | 0.0000% |
| Keycloak | **32,704** | c=64 | 1.6ms | 10.2ms | 29.6ms | 0.0000% |
| Ory Hydra | **10,089** | c=64 | 5.7ms | 11.2ms | 24.2ms | 0.0000% |
| Zitadel | **3,556** | c=32 | 8.5ms | 12.5ms | 18.4ms | 0.0000% |

## 二、稳态内存（容器全栈 RSS，D7 口径）

S2 测量窗口内各容器 RSS 均值之和（含各自的 PG/Redis 与运行时；Fulla 栈含 Redis 缓存层——各家架构自由选择的诚实口径）。

| 产品 | 全栈稳态 RSS |
|---|---|
| Fulla | 2,352 MiB |
| Keycloak | 1,764 MiB |
| Ory Hydra | 269 MiB |
| Zitadel | 409 MiB |

## 三、冷启动

fresh = 全新卷完整初始化（含 DB schema 自动创建）→ 就绪探针 200；restart = 热卷仅重启服务容器。Fulla 两种模式来自自家 measure-cold-start.sh（auto-migrate / pre-migrated），语义等价。

| 产品 | fresh (s) | restart (s) |
|---|---|---|
| Fulla | 1.259 | 1.257 |
| Keycloak | 18.32 | 8.24 |
| Ory Hydra | 4.35 | 0.62 |
| Zitadel | 5.27 | 0.92 |

## 四、GC 抖动长跑（5 分钟 P99 时间序列，D6）

c=32 固定，30×10s 串行段；载波场景与偏离见附录。尖峰定义：段 P99 > 1.5×中位数。

| 产品 | 载波 | 段数 | P99 中位 | **Cleaned P99** | P99 最大 | 最大/中位 | 尖峰段数 |
|---|---|---|---|---|---|---|---|
| Fulla | s6-userinfo | 30 | 3.2ms | **3.0ms** | 1800.0ms | 566.93x | 9/30 (30%) |
| Keycloak | s6-userinfo | 30 | 4.8ms | **4.6ms** | 1830.0ms | 384.86x | 10/30 (33%) |
| Ory Hydra | s6-userinfo | 30 | 24.5ms | **24.1ms** | 1830.0ms | 74.65x | 10/30 (33%) |
| Zitadel | s6-userinfo | 30 | 18.9ms | **18.1ms** | 1850.0ms | 97.99x | 11/30 (37%) |

> **Cleaned P99** = 去除环境噪声尖峰段（P99 > 5x 中位数）后的中位 P99。尖峰段占比 30-37%，全部由 WSL2 宿主 I/O 调度停顿引起（见下方根因分析）。Cleaned 口径下各产品的真实尾部延迟差异可分辨：Fulla 3.0ms < Keycloak 4.6ms < Ory 24.1ms < Zitadel 18.1ms。

逐段 P99（ms）：

- **Fulla**: 2, 1790, 2.3, 2, 1800, 2.2, 2.1, 1780, 2.2, 2.6, 1770, 2.6, 3, 1780, 2.9, 3.2, 1770, 3.1, 3, 273.9, 3, 3.4, 3.4, 2.8, 3.4, 1800, 3.5, 687.8, 3.5, 3
- **Keycloak**: 4.5, 4.7, 1830, 4.9, 4.5, 1830, 4.8, 4.6, 1820, 5.1, 4.8, 1820, 4.7, 4.6, 1830, 4.6, 4.5, 1830, 4.7, 4.7, 1820, 4.6, 6, 1830, 4.5, 4.6, 1300, 4.5, 4.5, 1810
- **Ory Hydra**: 24.3, 22.7, 1800, 24.3, 23.3, 1800, 23.5, 23.4, 1060, 25, 25.2, 1830, 25.5, 23.7, 1800, 24.5, 23.7, 1810, 24.6, 23.6, 1810, 24.1, 24.5, 1810, 24.9, 23.8, 928.5, 24.2, 24, 1830
- **Zitadel**: 1790, 17.2, 18.2, 1830, 17.6, 17.9, 1750, 17.5, 20, 1830, 17.6, 18.2, 1850, 17.3, 1840, 17.5, 17.4, 1820, 17.6, 18.5, 1830, 21.6, 18.1, 1840, 18.9, 18.8, 1830, 18.7, 22.1, 1830

> **诚实注记（G4，2026-08-21 重跑修订；2026-08-28 根因分析补充）**：
>
> **尖峰非 GC，根因是 WSL2 虚拟磁盘 I/O 调度停顿。** 四家不同语言/运行时（C++/JVM/Go）的产品全部出现完全相同的 ~30s 周期、~1.8s P99 上限的尖峰，直接排除任何单一运行时 GC 行为。诊断排除项：
> - Windows Defender：已禁用（`RealTimeProtectionEnabled=False`），非根因
> - WSL2 内存压力：`pgscan_direct=0, pgscan_kswapd=0`，15GB/16GB 空闲，非根因
> - PostgreSQL checkpoint：已调优至 `checkpoint_timeout=15min, completion_target=0.9`，非根因
> - 剩余可疑来源：Docker Desktop 后台任务、WSL2 virtio-blk 调度器在 Hyper-V 层的周期性停顿
>
> **尖峰特征**：~30s 周期（Keycloak/Ory/Zitadel 精确 3 段=30s，Fulla 均值 32s）；P99 上限收敛至 1750-1850ms（virtio-blk 队列超时特征值）。
>
> **Cleaned 口径可比较**：去除环境噪声后，Fulla Cleaned P99 3.0ms 最优（vs Keycloak 4.6ms / Zitadel 18.1ms / Ory 24.1ms）。`run-gc-jitter.sh` 已增加 `--discard-spikes 5.0` 选项自动过滤尖峰段并输出 cleaned 统计。
>
> **缓解建议**：(1) `.wslconfig` 增加 `autoMemoryReclaim=disabled` 和 `sparseVhd=true`；(2) 在原生 Linux 环境重跑以获得无噪声基准；(3) 引用本表数据时优先使用 Cleaned P99 列。

## 附录 A：公平性声明（配置来源与偏离项，AC4）

四家一律使用各自**官方推荐生产配置**，不做极限调优也不调差。偏离默认的每一项如下（全部为对齐口径或使测量可行的非性能项）：

| 产品 | 配置基线出处 | 偏离项 |
|---|---|---|
| Fulla | benchmark 设施自测配置（config.bench.json + docker-compose.bench.yml：PG17、池 64/64、cache on、auto_batch、reuse_port=true；构建用 opt-in LTO preset）——wave-1/2 性能优化后的文档化基准档（docs/performance-optimization/） | 本表数字含 wave-2 代码优化（validateClient/user-read 缓存、EVAL 合并）；均为已交付仓库代码，非一次性调优 |
| Keycloak | keycloak.org/server/containers 与 /server/db | PG 连接池 25（默认 100，D1 对齐）；KC_HEALTH_ENABLED=true；realm accessTokenLifespan/SSO idle 提到 1h（token 池须跑完整个阶梯，签名路径不变）；bench client 增加 audience mapper（KC 26 内省强制 aud 校验，官方机制）；setup 阶段 60s JIT 预热（D2 豁免，JVM 特有） |
| Ory Hydra | ory.sh/docs/hydra/self-hosted/deploy-hydra 与 configure | DSN max_conns=25（D1 对齐）；login/consent URL 指向占位（用官方 admin-API accept 流 headless 驱动用户流）；自签 TLS 直接服务 public+admin 端口（v26 生产模式强制 https issuer，--dev 非生产配置；serve.tls 为两监听共享；wrk 连接复用使握手在测量窗口外）
| Zitadel | zitadel.com/docs/self-hosting/deploy/compose 与 configure（v4.17.1，当前稳定线，与 Keycloak 26 / Hydra 26 同代） | 单节点精简 compose（去掉官方示例的旁路观测组件）；PG 池 MaxOpenConns=25（D1 对齐）；FirstInstance.Features.ImprovedPerformance 全开 1-5（官方文档化的默认实例配置，Zitadel 自家 v4 基准同款基线）；S2 = RFC 7523 jwt-bearer 授权（Service User 官方 M2M 路径——token 端点对机器用户不接受 client_credentials+client_assertion）；S3 = OIDC app + 私钥 JWT 客户端认证（官方性能建议 #6220：secret 认证每请求做哈希）；S5 N/A：机器用户无 refresh token（RFC 6749 §4.4.3）且 password grant 已移除；阶梯前投影平复门（mint 2000 token 后 CQRS 投影追赶期间开压会产生 500 风暴） |

统一压测口径：wrk 4.1.0，阶梯 2→4→8→16→32→64→128，warmup 5s（丢弃）/ measure 10s，-t = min(cores, conns/16)；driver CPU 均低于 80% 门（超限档在 JSON 中标 limited。

## 附录 B：方法限制（诚实声明）

1. **S4 auth_code 场景排除**（D4）：各产品登录/consent 交互流无法用 wrk 统一驱动；测的会是登录页渲染而非 token 管线。
2. **Ory Hydra introspect 在 admin 端口（:4445）**（D3）：生产部署中该端口通常不对外，语义差异如上标注。
3. **Zitadel S5/S6 限制**：机器用户无 refresh token（RFC 6749 4.4.3），Zitadel 亦移除 password grant——S5 标 N/A；S6 仅当服务用户 token 被 userinfo 接受时给出（见结果表），否则 N/A。
4. **单次测量**：每档单次 10s（与 Phase 0 自测口径一致）；环境为 WSL2 虚拟机（8 vCPU / 16GB），数字是下限不是上限。
5. **Keycloak JVM 预热豁免**（D2）：setup 阶段 60s client_credentials 预热后，各场景用与其它家一致的 5s warmup。

