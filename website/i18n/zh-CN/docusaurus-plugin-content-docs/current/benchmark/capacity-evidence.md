# 容量证据协议（S07）

S07 提供可复现、保留失败的容量工具。**本变更不发布已验收的当前产品容量基线**；功能检查不代表容量成绩、SLO 或 S07 完成。
执行方法见[工具说明](https://github.com/voidvec/fulla/blob/master/benchmarks/capacity/README.md)。
[历史竞品报告](https://github.com/voidvec/fulla/blob/master/benchmarks/competitors/results/COMPARISON.md)保留原版本、日期和适用边界。

## 前置条件与报告状态

每次运行记录具体产品 commit、编译器、C++ 模式、Release 参数、LTO/插桩状态、CPU/内存限制和依赖版本，使用原生 Linux 和真实 PostgreSQL + Redis。共机 Python 驱动测得容量下界，必须报告驱动 CPU 和共享 cgroup 预算。即使 build type 为 Release，也拒绝 sanitizer/coverage 构建。

真实双配置协议 smoke 全部通过后才能 pilot。新签发 token 返回 inactive、subject/client/组织绑定失败、缺少观察器或触发空间底线，都会使前置验证失败。保留失败，不加等待、不放宽校验、不重试覆盖来获得漂亮样本。所需产品修复与压测工具分别评审。

非授权资源 preflight 可以指出持久化空间不足，但不能批准正式测量。只有同环境成功 pilot 才能提供基于实际 raw/数据/WAL 增长的预测。108 行矩阵、两小时长跑和正式故障各自需要完整证据和独立验收，短时功能检查不能替代。环境与诊断工件须经仓库所有者授权并审阅凭据/内部数据后才能公开。

## 范围与配置边界

本项修改仅限压测工具、脱敏证据和文档，不修改服务端、SDK、迁移、ORM、API、锁文件；不调查已知 #300 退出问题或 macOS runner 变化，也不把历史竞品数字改称当前成绩。

两臂使用同一产品二进制、fixture、安全设置和持久化数据库。默认配置保留自动线程 `0`、PG/Redis 各 10 连接和 DEBUG 日志；公开 tuned 增量为 2 线程、两池各 16 连接、开启 cache、WARN 日志。两臂同样限制日志保留，为私有 fixture 关闭 relaunch/bootstrap-admin，使用回环监听。这些 fixture/日志差异必须公开，不能称为逐字节出厂默认配置。密码哈希保持 PBKDF2-SHA256/310000，TTL 和持久化不变。

参考实验 PG 参数为 shared_buffers 64MB、max_connections 64、min/max WAL 32/128MB、checkpoint_timeout 60s，开启 checkpoint 日志以及 fsync、同步提交、full-page writes。这是实验参数，不是 PG 出厂或生产默认值。独立实例隔离进程、统计和故障控制，但仍与 workspace 共用 overlay 容量；WAL 可以超过软阈值。保留实际 checkpoint 次数、时间和事件，不能假定 120 秒必有两次检查点。受限应用/运行时日志可放在已披露的 tmpfs；PG 数据保持持久化。

## 工作负载与执行顺序

四个 VU 各自独占账号、cookie 会话和一次性 RT 链。认证流程每次生成新 PKCE、OAuth state、OIDC nonce、服务端 consent CSRF，校验 subject、redirect、client 和真实组织绑定。ID token 只核对 nonce/subject 关联，不宣称验证 JWT 签名。RT 响应丢失时将链标为不确定，通过重新认证恢复，不重放旧 RT；恢复请求仍计入原始记录。

| 阶段 | 固定协议 | 验收边界 |
|---|---|---|
| 工具检查 | 协议、截止、隔离、计数、脱敏、预算、租约测试 | 真实 smoke 前通过；替身不能代替真实栈 |
| 真实 smoke | 默认/tuned × 九场景，4 VU，预热 0.2s + 测量 1s | 全部通过才能做容量 pilot；准备失败同样留档 |
| Pilot | 同样 18 臂，4 VU，2s + 10s | 全部通过；实际字节/增长用于绑定预算 |
| 稳态矩阵 | 九场景 × 两配置 × VU 1/4 × 三轮 | 108 个唯一样本，每行 30s + 120s；第二轮反转配置顺序 |
| 长跑 | 混合、4 VU、真实生产 Hodor，30s + 7200s | 保留 raw、清理/检查点/资源序列，无未解释趋势或失败 |
| 故障 | 独立 tuned 诊断：Redis、候选 DB 代理、后端重启 | 30s 基线、至少 30s 中断、恢复后 300s 内连续三次业务成功 |

九场景为零 DB discovery 负控、完整密码/PKCE/consent+userinfo、Basic CC、活 token 复用内省、精确冷缓存控制、RT 轮换、userinfo、真实组织上下文和混合。混合固定 12 槽并按 seed 洗牌：密码 1、组织 1、CC 1、refresh 1、userinfo 3、复用内省 2、冷控制 1、discovery 2。非长跑行另有 0.2Hz discovery 同窗负控，流量不计业务 QPS。三轮全部保留。

## 计时与结果解释

逻辑 flow 在 Redis 控制或 pacing 之前准入，首 HTTP 前的失败也计数。HTTP 在 pacing 后开始，有独立准入/完成事件。预热停止准入并排空后再测量；start cohort 为 `[start, stop)`，stop 后不准入新 flow，已有 flow 有界排空 60s（长跑 300s）。绝对截止也覆盖准备、health、metrics。未完成请求、观察器早死、次数预算耗尽、清理失败或触发空间底线都不能 passed，不使用无限退出等待。

窗口完成 QPS 只取窗口内完成的成功数除以 120s。cohort 计数与延迟保留窗口内开始、排空期完成的项；cohort 吞吐用实际完整墙钟分母，不能把窗口外完成数除以 120。HTTP 与业务 flow 失败分别呈现，包含 429、inactive token、超时和控制失败。全尝试 nearest-rank P50/P95/P99/max 不裁尾。闭环模型存在 coordinated omission，共机 Python 驱动竞争限制了生产外推。CSV 逐行导出，不挑最好轮次，不以 cleaned P99 为主指标。

## 资源、长跑和故障观察

正式入口是串行 `session.py`，同一 canonical 私有 fixture 文件由独占锁保护。发送凭据前验证 DB/Redis 归属、监听/PID/二进制、运行配置、真实持久化依赖和 readiness。pilot 预算绑定源码/二进制/nonce/boot、CPU/内存/内核、PG 参数和配置源文件。每种 kind 的 profile/VU/预热/窗口/drain 范围固定，不能用长跑预算跑超长稳态行。保留全部 raw、归档副本、数据/WAL 余量及 192MiB 持久化保留空间；初始化至少需 320MiB 空闲，准备、pilot、故障也持续监测。非授权 preflight 不能替代成功 pilot。

pilot 和正式窗口只运行候选栈、驱动、固定观察器；停止编译、测试、源码扫描和审查 agent。每 5s 采样进程 RSS/PSS（可用时）、fd/线程/TCP、cgroup CPU/内存、PG 连接/锁等待/数据/令牌行/WAL/检查点及 Redis 内存。不宣称有应用内部排队指标。从独立 PG 日志保留原始 checkpoint 事件，不发布无关或私有 SQL 日志。

长跑从真实 JSONC 生产配置复制 Hodor，先对不受信任的回环来源做有限 burst 并实际观察 429，再重启候选。所有 HTTP、health、metrics 共用每 60s 滚动配额：总 8、login 1、token 2。资源读取不额外调用 HTTP。这是低速生产策略长跑，不是峰值持续容量。报告实际 TTL/清理周期覆盖；两小时不能证明 30 天 RT 保留有界或无泄漏。

故障只操作 nonce 验证后的本轮资源：Redis 中介校验具体容器 ID/卷，DB 代理切断已有/新候选连接且上游 PG 保持可用，重启仅影响本轮候选。记录命令、实际中断、恢复、首次成功和三次稳定成功时刻，以及每次探针和实际间隔。pacing 可能使恢复分辨率远粗于 1s；正式故障要求足够采样覆盖，降级或恢复失败不消失。默认配置短时控制 smoke 与 tuned 正式故障分开。

## 验收与后续处理

S07 须同时满足四层：工具测试通过、真实协议 smoke 成功、完整矩阵/长跑/故障有 raw 且无未解释失败、独立评审确认报告与数据一致。本工具变更没有发布已验收的正式基线；未完成运行必须标为 blocked，不能凭功能检查推进 S08。

先解决真实协议失败并补回归测试，不放宽校验契约；确保真实持久化空间足够，再运行 smoke 和新 pilot，由 pilot 确定完整空间需求。矩阵前冻结已评审的源码/二进制/环境，长跑后复核真实 raw/清理趋势。后续吞吐下降超过 10% 或 P99 上升超过 20% 可触发三轮回归复核，但只是告警参考，不是承诺的生产 SLO。
