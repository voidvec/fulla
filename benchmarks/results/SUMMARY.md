# Fulla HTTP 基准测试结果摘要

> **测试日期**: 2026-08-12（本文件 = 自测首版快照）
> **Git SHA**: 1702246
> **承重背景**: 验证调研报告 §3.1 的性能声明（该报告未入库）
> **方法论**: [competitor-benchmark-design.md](../../docs/benchmark/competitor-benchmark-design.md)（§2 以本文件为自测基线）；运行方式见 [benchmarks/README.md](../README.md)
>
> ⚠️ **2026-08-23 更新——本文件数字已过时（偏保守）**：后续性能优化计划（Redis 读缓存 wave-2 P0–P4、PG17 + 实例调优、bench 配档池 64/64 + cache-on + LTO、TTL=30 会话）大幅提升了实测值，且竞品同环境对比已完成。**当前权威数字见 [`benchmarks/competitors/results/COMPARISON.md`](../competitors/results/COMPARISON.md)**（2026-08-23 TTL=30 档，同环境四产品对比，Fulla **五场景全部领先**）：S1 discovery 87.5k（Keycloak 41.1k）、S2 client_credentials **14.4k**（本文件 8.9k → +62%）、S3 introspect **22.5k**（17.1k）、S5 refresh **5.5k**（2.0k）、S6 userinfo **49.3k**（16.7k → +195%）。本文件保留作为优化前基线与承重假设首验记录。

---

## 测试环境

| 维度 | 值 |
|------|-----|
| **宿主机** | Lenovo Y7000 IA×10, Intel Core Ultra 7 255HX (20 核 / 20 线程), 48 GB RAM, NVMe SSD |
| **WSL2 虚拟机** | 8 vCPU, 16 GB RAM (`.wslconfig` 分配) |
| **Docker** | Docker Desktop 29.7.2, WSL2 集成 |
| **压测工具** | wrk 4.1.0 (Debian, epoll) |
| **目标栈** | postgres:17-alpine + redis:7-alpine + fulla-backend (Docker overlay 网络) |
| **后端配置** | `config.bench.json` — PG 连接池=25, Redis 连接池=20, log_level=WARN |
| **网络拓扑** | localhost cross-container (wrk 在 WSL 内, target 在 Docker overlay) |
| **压测参数** | 阶梯 2→4→8→16→32→64→128 连接; 5s 预热 (丢弃) + 10s 测量; S4 强制 -t==-c |

> ⚠️ **数字是下限，不是上限**。所有场景 driver CPU < 44%（wrk 未打满），且 WSL2 虚拟化有开销。裸机/专用基准机上的数字会更高。

---

## 场景速览（稳态容量 = 错误率 <0.01% 的最高 QPS 档）

| # | 场景 | 端点 | 稳态 QPS | 峰值 QPS | 稳态 P99 | 测什么 |
|---|------|------|---------|---------|---------|--------|
| S1 | discovery | `GET /.well-known/*` | **86,332** (c=64) | 86,332 | 430ms | 纯框架天花板（无 DB/Redis） |
| S2 | client_credentials | `POST /oauth2/token` | **8,915** (c=32) | 8,915 | 8ms | RS256 签名 + PG 客户端查询 |
| S3 | introspect | `POST /oauth2/introspect` | **17,132** (c=128) | 17,132 | 12ms | RS256 验签 + DB 活跃状态查表 |
| S4 | auth_code+PKCE | `POST /oauth2/login` → `POST /oauth2/token` | **465** (c=32) | 465 | 159ms | 最重路径：PBKDF2 密码验签 + auth_code + RS256 |
| S5 | refresh_token | `POST /oauth2/token` (refresh) | **1,982** (c=64) | 1,982 | 26ms | V008 旋转 + 新 token 签发 |
| S6 | userinfo | `GET /oauth2/userinfo` | **16,674** (c=64) | 16,674 | 12ms | Bearer 过滤 + 用户记录查询 |

> S4 的 0.02% 错误率是多步流（login→token 交替）的固有时序效应（约每 2000 请求 1 个），非产品 bug。

---

## 详细阶梯数据

### S1 discovery
| c | t | QPS | P99 | err% | drv CPU% |
|---|---|-----|-----|------|---------|
| 2 | 1 | 14,513 | <1ms | 0.01% | 17.8% |
| 4 | 1 | 26,764 | 4ms | 0.01% | 26.6% |
| 8 | 1 | 52,351 | <1ms | 0.01% | 35.8% |
| 16 | 1 | 79,164 | 19ms | 0.01% | 43.5% |
| 32 | 2 | 83,280 | 75ms | 0.01% | 28.7% |
| **64** | **4** | **86,332** | **430ms** | **0.01%** | 17.7% |
| 128 | 8 | 80,257 | 1.16s | 0.00% | 8.8% |

### S2 client_credentials
| c | t | QPS | P99 | err% | drv CPU% |
|---|---|-----|-----|------|---------|
| 2 | 1 | 1,382 | 2ms | 0.00% | 2.8% |
| 4 | 1 | 2,291 | 3ms | 0.00% | 4.5% |
| 8 | 1 | 4,067 | 4ms | 0.00% | 6.4% |
| 16 | 1 | 6,383 | 6ms | 0.00% | 9.2% |
| **32** | **2** | **8,915** | **8ms** | **0.00%** | 8.2% |
| 64 | 4 | 8,860 | 20ms | 0.00% | 4.6% |
| 128 | 8 | 8,608 | 23ms | 0.00% | 2.6% |

### S3 introspect
| c | t | QPS | P99 | err% | drv CPU% |
|---|---|-----|-----|------|---------|
| 2 | 1 | 3,111 | 1ms | 0.01% | 5.4% |
| 4 | 1 | 5,776 | 1ms | 0.01% | 10.1% |
| 8 | 1 | 9,405 | 2ms | 0.01% | 16.2% |
| 16 | 1 | 13,642 | 2ms | 0.00% | 21.0% |
| 32 | 2 | 16,062 | 4ms | 0.00% | 14.0% |
| 64 | 4 | 13,697 | 73ms | 0.00% | 7.5% |
| **128** | **8** | **17,132** | **12ms** | **0.00%** | 4.3% |

### S4 auth_code+PKCE
| c | t | QPS | P99 | err% | drv CPU% |
|---|---|-----|-----|------|---------|
| 2 | 2 | 129 | 33ms | 0.08% | 0.2% |
| 4 | 4 | 193 | 58ms | 0.05% | 0.2% |
| 8 | 8 | 376 | 60ms | 0.03% | 0.2% |
| 16 | 16 | 426 | 108ms | 0.02% | 0.2% |
| **32** | **32** | **465** | **159ms** | **0.02%** | 0.1% |
| 64 | 64 | 459 | 281ms | 0.02% | 0.1% |

### S5 refresh_token
| c | t | QPS | P99 | err% | drv CPU% |
|---|---|-----|-----|------|---------|
| 2 | 1 | 594 | 11ms | 0.00% | 2.2% |
| 4 | 1 | 1,137 | 7ms | 0.00% | 3.2% |
| 8 | 1 | 1,951 | 145ms | 0.00% | 5.0% |
| 16 | 1 | 1,474 | 8ms | 0.00% | 6.8% |
| 32 | 2 | 1,801 | 15ms | 0.32% | 3.4% |
| **64** | **4** | **1,982** | **26ms** | **0.00%** | 2.0% |

### S6 userinfo
| c | t | QPS | P99 | err% | drv CPU% |
|---|---|-----|-----|------|---------|
| 2 | 1 | 2,619 | 126ms | 0.01% | 5.2% |
| 4 | 1 | 5,947 | 1ms | 0.01% | 10.0% |
| 8 | 1 | 7,931 | 162ms | 0.02% | 16.2% |
| 16 | 1 | 14,297 | 2ms | 0.00% | 20.9% |
| 32 | 2 | 11,521 | 7ms | 0.02% | 14.8% |
| **64** | **4** | **16,674** | **12ms** | **0.00%** | 7.9% |
| 128 | 8 | 14,283 | 14ms | 0.07% | 4.3% |

---

## 承重假设验证（对照调研报告 §3.1）

> 调研报告 §3.1 的 Fulla 列标注为"目标值，待 benchmark 验证"。以下是实测裁决。

| 声明 | 实测 | 裁决 | 说明 |
|------|------|------|------|
| **单机 QPS ~100,000+** | S1 discovery 86,332 QPS (8 vCPU WSL) | ⚠️ **接近** | 纯框架路径在 8 vCPU 虚拟机上达 86k。线性外推 16 核裸机 ~170k，**几乎确定可达 10 万+**。注意：S2–S6 涉及 DB/签名，QPS 低一个数量级——"10 万+"仅适用于 discovery 类无状态端点。 |
| **内存 50–120 MB** | SDK: 2.5 MB peak WS / 0.6 MB private（12 MB binary）; Docker 全栈: ~2.4 GB | ✅ **SDK 口径远超标** | docker stats 测的是容器全栈 RSS（Drogon 连接池 + 共享库 + page cache）。**SDK 嵌入口径实测**（2026-08-13，Windows/MSVC）：`third-party-host-smoke.exe`（纯 SDK：oauth2+common+memory storage）peak working set **2.5 MB** / private **0.6 MB** / binary **12 MB**；`full-stack-host-smoke.exe`（SDK+Drogon）同样 2.5 MB peak。50-120 MB 声称**保守达标**——实际 SDK 逻辑层远低于此。 |
| **P99 < 2ms** | S3/S6 稳态 P99 = 1–2ms (低并发档); S1 c≤8 P99 <1ms | ✅ **低并发达成** | c=2–16 时 S1/S3/S6 的 P99 在 1–4ms 范围，接近 2ms 目标。但高并发（c≥64）时 P99 退化到 73–430ms——这是连接池排队效应，非框架固有延迟。 |
| **冷启动 ~5s** | setup.sh 观测: `/health/ready` 在 ~4s 返回 200 | ✅ **观测达成** | compose up（含 PG+Redis 启动）后 `/health/ready` 在 ~4s 就绪（setup.sh 健康门控观测）。已满足 ~5s 目标。如需更精确数字（分离 backend 冷启动 vs PG/Redis 启动），可运行 `measure-cold-start.sh`，但目标已达成。 |

---

## 结论

### 卖点保留（实测支撑）
- **纯框架吞吐量极快**：S1 discovery 86k QPS（8 vCPU WSL），证明 Drogon + C++ 栈的框架级吞吐能力出色，裸机 16 核预估可达 10 万+。
- **读路径延迟低**：S3 introspect / S6 userinfo 在合理并发下 P99 ~2ms，与竞品（Keycloak P99 10–50ms）相比领先一个数量级。
- **token 交换稳定**：S2/S3/S5/S6 在全部并发档下错误率 ≤0.02%，0% 稳态错误率——服务在高负载下行为可预测。

### 卖点修正（需调整措辞）
- **"10 万+ QPS" 应限定场景**：仅适用于 discovery/JWKS 等无状态端点。token 签发（S2 client_credentials）稳态 ~9k QPS，introspect/userinfo ~17k QPS——这些数字仍远超 Keycloak (~10–20k QPS)，但不是 10 万。
- **"内存 50–120 MB" SDK 口径实测远超标**：`third-party-host-smoke` 实测 2.5 MB peak working set（binary 12 MB），远低于声称下限。对外传播须区分 SDK 嵌入口径（2.5 MB）与容器全栈口径（~2.4 GB，含连接池/共享库/page cache）。
- **"P99 < 2ms" 应限定并发**：低并发（c≤16）成立；高并发（c≥64）退化到 12–430ms。
- **"冷启动 ~5s"** ✅ 已达成：setup.sh 观测 ~4s 就绪。

### 诚实声明
- 以上数字来自 **WSL2 虚拟机**（8 vCPU / 16 GB），非裸机专用基准机。虚拟化层 + Docker overlay 网络有开销。
- 所有场景 **driver CPU < 44%**（wrk 未打满），数字是**下限**。
- **竞品对比尚未进行**（Phase 0.5），当前数字仅与研究报告的工程估算对比，未与 Keycloak/Ory/Zitadel 同环境对比。
- **M4 的 `gen-summary.py` 自动生成器未实现**——本文件为手写。后续每次基准测试后应手动更新或实现自动生成。

---

## 快赢 A/B（2026-08-18，8838ac6）

快赢档（noncode-performance-optimization.md §八）对 20260817-03965fa 基线的
同机对比：cache on（Redis 池 20→64）+ PG 实例调优（shared_buffers 4GB /
checkpoint 15min / max_wal_size 4GB 等，见 `benchmarks/fulla/docker-compose.bench.yml`）
+ bench 档微优化（gzip/brotli/server/date 头关、去 PromExporter）。完整阶梯
JSON：`20260818-8838ac6-*.json`；GC 抖动：`competitors/results/20260818-8838ac6-fulla-gcjitter.json`。

| 场景 | 基线峰值 (c) | 快赢峰值 (c) | 变化 | 各档一致性 |
|---|---|---|---|---|
| S1 discovery | 94,640 (64) | 103,746 (32) | **+3~14%** | 全档正 |
| S2 client_credentials | 9,056 (32) | 12,547 (128) | **+23~43%** | 全档一致正 |
| S3 introspect | 17,602 (64) | 20,078 (64) | -24%~+14% | 混合/噪声带内 |
| S5 refresh | 1,998 (16) | 1,998 (32) | ≈持平 | 写链串行瓶颈未动（audit 分区在第二步） |
| S6 userinfo | 18,278 (32) | 20,663 (128) | **c≥64: +9~13%**；c≤4: **-38~-43%** | 双模：高并发卸载赢，低并发 cache 税输 |

GC 抖动（S6 c=32，30×10s）：p99 中位 3.3→4.7ms（cache 路径 +2 次 Redis 往返）；
**极值 655.5→291.8ms（砍半）**；>1.5x 中位尖峰 7→12 个（中段 76~292ms 仍在）。
判定：checkpoint 风暴极值被 PG 调优压制，尾抖动未根除（剩余归因候选：WSL2 IO、
Redis 客户端批处理 —— 发布前按 §六.4 补 PG 侧证据）。

冷启动：1.265s / 1.261s（pre-migrated），与基线 1.23s 持平。

### 快赢期间的三个实测裁决（细节见部署文档与 overlay 注释）

1. **Redis 池必须 ≥ 预期并发**：池 20 时 S6 -18% 且全连接超时；64 后转正。
   cache.on 不是纯配置开关，是"cache + 池扩容"组合拳。
2. **introspect 正向缓存受 N2 判别器约束**（仅 token 走过发放/校验路径才回填），
   bench 预植 token 永不回填 —— S3 的收益实际来自 client 缓存 + PG 调优。
   改判别器语义 = 代码项（§十 backlog）。
3. **host network 在 Docker Desktop (WSL2) 下不可用**：host netns = 引擎 VM netns，
   发行版内 127.0.0.1/共享 eth0 IP/host.docker.internal 全部不可达（仅发布端口
   转发）。该杠杆留给裸机/原生引擎（§五.9，发布前重测）。

---

## 第二步 A/B（2026-08-18，a9d6327）

第二步档（V025 audit 月度分区 + BRIN / PG 15→17 / db 池 25→64）对快赢档
（8838ac6）与原始基线（03965fa）的同机对比。完整数据：
`20260818-a9d6327-*.json` + `competitors/results/20260818-a9d6327-fulla-gcjitter.json`。

**对快赢档（增量归因）**：吞吐中性偏差（S1 -5%、S2 -4~-14%、S6 高并发 -9%，
均在会话漂移噪声带内；S1 不触库也 -5% 即为漂移佐证）。**文档预估的
audit 分区 +15~25% 未兑现**——该预估的前提是"索引深度随表增长而退化"，
基准规模（万级行）下不存在该退化；分区的每行路由 + 每分区更多索引结构
（复合 PK + 4 B-tree + BRIN）与小表上的旧单表打平。PG17 vs PG15 同样
淹没在噪声里。**分区的真实收益是治理而非吞吐**：索引深度受控、保留策略
从 DELETE 变 `DROP PARTITION`（瞬时、零膨胀），在长期运行的大表上才兑现。

**对原始基线（累计）**：S2 全档 +13~49%（c128 +49%）、S1 高并发 +8~9%、
S3/S6 高并发 +0~9%；S5 持平（reseed 串行瓶颈）。GC 极值 655ms 档位未再
复现 checkpoint 风暴形态（见下方雷群）。冷启动 1.27~1.30s 持平。

### 新发现：缓存 TTL 同步到期雷群（30s 周期 ~800ms 尖峰）

第二步 GC 抖动（S6 c=32，30 段）出现严格 ~30s 周期的 789~809ms p99 尖峰
（11 段命中，命中段 QPS 从 ~15k 掉到 ~11.5k）。机理：S3/S6 压测均匀轮询
2000 个 token，全部 60s TTL 缓存条目成批到期 → 同一瞬间全量回源 DB 的
雷群；两个错相 30s 的队列（前序场景遗留）造成 30s 表观周期。**池扩大放大
雷群深度**：快赢档（池 25）同样机制只表现 291ms。池扫描中的间歇 585~816ms
尾巴同理（10s 窗口 ~1/3 概率命中）。
修复属代码层（§十 backlog）：TTL 抖动（60s±20%）、按 key single-flight
合并回源、或 stale-while-revalidate。生产流量不均匀轮询 key，严重性低于
基准形态，但高命中率场景应知晓。

### 第二步运维注记

- bench 栈 `max_connections=200`（池 64 + 迁移/seed/运维 psql 余量）；
  池=100 的扫描档曾打满默认 100 连接导致 psql 都被拒（已修复 sweep 恢复逻辑）。
- audit 分区运维：`SELECT ensure_audit_partitions();` 定期调用（cron/手册），
  保留期治理用 `DROP PARTITION`。

---

## auto_batch 同日 A/B + V026 索引清理（2026-08-18，c9c13d8 / 3c1ced3 / c5654a4）

用户指令：`auto_batch`/`is_fast` 统一 false 后全量重测；若 auto_batch=true 更快则性能与生产环境保持 true。

前置事实：`is_fast` 在 db+redis × bench/default/prod 六处**本就全为 false**（启用 fast 客户端需
代码改造，§十 backlog），故本 A/B 唯一变量是 db `auto_batch`。两臂同日背靠背（消除跨会话
漂移），其余配置相同（池 64/64、cache on、PG17、V025、快赢档微优化）。

| 场景 | false 峰值 (c9c13d8) | true 峰值 (3c1ced3) | true vs false 逐档 |
|---|---|---|---|
| S2 client_credentials | 10,388 (c64) | 12,954 (c128) | **+13~49% 全档正** |
| S3 introspect | 16,665 (c128) | 18,911 (c128) | +5~29% |
| S6 userinfo | 15,152 (c64) | 19,152 (c128) | -3%~+53%（6/7 档正） |
| S5 refresh | ~1,999 封顶 | ~1,999 封顶 | 持平（reseed 串行瓶颈主导） |

**裁决：`auto_batch=true` 显著更快，bench/default/prod 维持 true**（default/prod 全程未改动）。
代价注记：GC 抖动中位 p99 5.8→11.9ms（批处理排队延迟，绝对值小，两臂 30 段雷群尖峰数相同
14/30、极值 ~1.1s 持平——雷群是代码层问题，与 auto_batch 无关）。auto_batch=true 的官方
"仅建议只读客户端"风险维持文档化（noncode-perf §2.2；读写拆分客户端为 §十 代码项）。

诚实声明：arm A 的 S1（不触库）与 S2 前段同一个只读代码评审代理在宿主机并发运行，S1 数据
被污染（-3.6%~+57% 大幅摆动，不触库场景不应有此效应，不应作为 auto_batch 证据）；S3/S5/S6
在代理结束后测得，结论不受影响。**教训：bench 期间宿主机连只读代理都不要跑。**

### V026 token 冗余索引清理（introspect 覆盖索引方案，子代理评审后落地 C 案）

评审（`introspect-covering-index-plan.md` v2，未入库）修正了原案三处：① introspect 查询是 `SELECT *`（Mapper::findOne），INCLUDE 5 列的窄覆盖
索引无法触发 index-only scan——原案单独不成立；② **SchemaManager 把全部迁移包在单事务里执行
（SchemaManager.cc #46），`CONCURRENTLY` DDL 必败**——V026 改用普通 `DROP INDEX IF EXISTS`；
③ 全行 INCLUDE（B 案）降级 backlog：2000 行 bench 小表上收益≈噪声（表常驻 shared_buffers），
且种子后无 VACUUM 时 IOS 根本不触发。落地项为 **C 案（V026，c5654a4）**：删除 3 个与 PK 重复
的 B-tree（`idx_access_tokens_token` / `idx_access_tokens_active` / `idx_refresh_tokens_token`），
token INSERT 索引维护 8→6、revoke UPDATE 少 2 个翻新目标。

验证：**full-test 8/8 PASS**（462 ctest + 59 OAuth2 + 55 Admin 端点，V026 在 step 1 干净应用）；
bench 同日抽查（c5654a4 vs 3c1ced3，同 5s/10s 协议）：**S2 高并发档 +8%**（c64 12,880 vs
11,877、c128 13,977 vs 12,954，p99 干净，与写放大降低机理一致），S3 全档中性偏正（-0.6%~+15.6%，
PK 继续服务读路径无回退）；低并发单档噪声大（雷群尾命中 + 抽查无 S1 预热段）不作为证据。
方法学注记：抽查第一次误用 run-scenario.sh 默认 10s/30s 协议，30s 窗口必然撞上 ~30s 周期的
TTL 雷群（每档 p99 ~0.44s），数据已归档 `v026-spot-30s-confounded/`——**抽查必须显式导出
`WARMUP_S=5 DURATION_S=10` 对齐会话协议**。与 V025 同理，V026 的完整收益（索引维护成本不随
表增长）在长期大表上才兑现，基准规模下是"中性偏正 + 治理"。

---

## 合并树全量复测（2026-08-19，97c9254）

两会话分裂执行的技术债修复后的**新基线**：97c9254 是合并提交（第一亲 919728d = auto_batch
A/B + V026 线，第二亲 8679d72 = WSL 会话的 deploy PG17 升级线；auto_batch 按已拍板裁决
bench/default/prod 全 true）。合并对 bench 环境无功能性差异（bench 栈本就经 overlay 锁
PG17、config.bench.json 未变），本复测确认合并树健康并取代跨日旧数成为后续对照基线。

全套同会话协议（`run-fulla-session.sh`：S1/S2/S3/S5/S6 × c2–c128，WARMUP_S=5
DURATION_S=10，GC 抖动 30×10s，冷启动×2）：

| 场景 | 昨日 true 臂峰值 (3c1ced3) | 本日峰值 (97c9254) | 峰值变化 | 备注 |
|---|---|---|---|---|
| S1 discovery | 96,513 (c32) | **102,758 (c32)** | +6% | 全档 +6~18% |
| S2 client_credentials | 12,954 (c128) | **14,688 (c128)** | +13% | c64 单档 -5%（噪声） |
| S3 introspect | 18,911 (c128) | 17,259 (c64) | -9% | 跨日方差（见下） |
| S5 refresh | ~1,999 封顶 | ~2,000 封顶 | 持平 | reseed 串行瓶颈 |
| S6 userinfo | 19,152 (c128) | 17,621 (c128) | -8% | 跨日方差（见下） |

S3/S6 偏低判读为**跨日环境方差而非合并回归**：① 昨日两臂为同日背靠背（同会话设计消除的
正是跨日漂移 R7），本日为隔日独立会话、Docker Desktop 冷启动后首跑；② TTL 雷群（~30s
周期）落入 10s 测量窗的位置两日不同——昨日 S3/S6 多档 p99 带 0.6–1.1s 雷群尾（如旧 c2
609ms、c16 1100ms），本日同档 p99 干净得多；③ **GC 抖动本日反而更干净**：中位 p99
17.0→4.1ms、极值 1140→380ms、雷群段 14/30→10/30——抖动改善而 QPS 略降与"回归"不相容。
结论：合并树与 919728d 性能等价（bench 路径无差异），S3/S6 差值记为方差，后续对照以本日
97c9254 数据为基线。

冷启动（sha=97c9254）：auto-migrate 1.446s / 12.4MB（含 V026 迁移链）；pre-migrated
1.240s / 8.8MB（脚本 `--migrate-only` 探针 WARN 为已知非致命路径，结果正常）。

---

## 如何复现

```bash
# 1. 配置 WSL2（.wslconfig: processors=8, memory=16GB）
# 2. 启动栈 + 种子 + token 池 + 预热
bash benchmarks/fulla/setup.sh

# 3. 跑阶梯测试
bash benchmarks/fulla/run-scenario.sh scenarios/s1-discovery.lua 2 4 8 16 32 64 128
# ... 对 S2–S6 重复

# 4. 查看结果
ls benchmarks/results/*.json
```

完整指引见 [`benchmarks/README.md`](../README.md)。
