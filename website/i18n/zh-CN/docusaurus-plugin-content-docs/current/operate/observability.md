# OAuth2 可观测性设计文档 (Observability)

本系统集成了完整的 Prometheus 监控指标与结构化上下文日志，支持生产环境的实时监控与问题排查。

## 1. Prometheus Metrics

系统通过 Exporter 暴露标准的 Prometheus 指标。

### 1.1 指标列表

| 指标名称 | 类型 | 标签 (Labels) | 说明 |
|----------|------|--------------|------|
| `oauth2_requests_total` | Counter | `endpoint`, `status` | OAuth2 请求总数 |
| `oauth2_login_failures_total` | Counter | `reason` | 登录失败次数 |
| `oauth2_introspect_requests_total` | Counter | `client_id` | Token Introspection 请求总数 |
| `oauth2_introspect_errors_total` | Counter | `client_id`, `error` | Introspection 错误次数 |
| `oauth2_revocation_requests_total` | Counter | `client_id` | Token Revocation 请求总数 |
| `oauth2_revocation_errors_total` | Counter | `client_id`, `error` | Revocation 错误次数 |
| `oauth2_latency_seconds` | Histogram | `operation`, `storage` | 关键步骤（含存储后端）耗时分布 |
| `oauth2_active_tokens` | Gauge | — | 当前活跃（未过期）Token 估算值 |

> 指标由 `libs/drogon/src/observability/Metrics.cc` 与 `libs/drogon/src/adapters/DrogonMetrics.cc` 统一发射（经 `LOG_INFO` 的 `[METRIC]` 结构化日志，供 PromExporter/日志采集端消费），定义见 `libs/drogon/include/fulla/drogon/observability/Metrics.h`。

### 1.2 监控面板示例 (Grafana)

建议配置以下面板：

- **QPS & Error Rate**: `rate(oauth2_requests_total[1m])` vs `rate(oauth2_login_failures_total[1m])`
- **P99 Latency**: `histogram_quantile(0.99, rate(oauth2_latency_seconds_bucket[1m]))`
- **Business**: Active Tokens trend.

## 2. Structured Logging (结构化日志)

系统采用上下文感知的结构化日志，便于 Splunk/ELK 收集分析。

### 2.1 Audit Logs (审计日志)

关键安全操作（如颁发 Token）会输出带有 `[AUDIT]` 标记的日志。

**格式**:
`[AUDIT] Action={Action} User={UserId} Client={ClientId} Success={True/False} IP={RemoteAddr}`

**示例**:

```
2026-01-18 10:00:00 INFO [AUDIT] Action=IssueToken User=admin Client=fulla-portal Success=True
2026-01-18 10:05:00 WARN [AUDIT] Action=ExchangeCode User=admin Client=fulla-portal Success=False Reason="Replay Detected"
```

### 2.2 Contextual Logs (上下文日志)

在请求处理链路中，所有日志自动附带 `RequestId`，用于关联分布式追踪。

```cpp
LOG_INFO << "Processing request"; // 输出: [ReqId: abc-123] Processing request
```

## 3. 配置与集成

### 3.1 开启 Metrics

默认情况下，Metrics Exporter 监听 `/metrics` 端点（需在 Drogon 配置文件中开启 Exporter）。

### 3.2 日志级别

系统统一使用六级日志，等级与 Drogon/Trantor 的内置级别一一对应（trace < debug < info < warn < error < fatal）：

| 等级 | 含义 | 典型场景 |
|------|------|----------|
| `trace` | 最细粒度追踪 | 函数入参 / 出参、循环迭代、逐行执行轨迹，用于深度调试定位 |
| `debug` | 调试信息 | 变量值、分支走向、内部状态变化，开发阶段排查问题用 |
| `info` | 常规信息 | 服务启动 / 停止、关键流程节点、用户登录、任务完成等正常业务事件 |
| `warn` | 警告 | 可恢复的异常、降级处理、配置使用默认值、资源接近阈值等，系统仍能正常运行 |
| `error` | 错误 | 功能失败、请求异常、数据库连接失败等，影响单次操作但服务整体可用 |
| `fatal` | 致命错误 | 导致服务崩溃、无法继续运行的严重故障，需立即告警并人工介入 |

**约定**：

- 生产环境默认开启 `info` 及以上级别；`trace`/`debug` 按需动态开启。
- 等级越高输出越少，`fatal` 应极少出现。
- `apps/server/config/config.prod.json` 默认为 `INFO`，`config.dev.json` 默认为 `DEBUG`，`config.json`/`config.ci.json` 默认为 `DEBUG`。

**动态调整**：修改配置文件 `app.log.log_level` 字段即可，可选值为 `TRACE`/`DEBUG`/`INFO`/`WARN`/`ERROR`/`FATAL`（不区分大小写）：

```json
"app": {
    "log": {
        "log_level": "INFO"
    }
}
```

**代码约定**：

- Domain 层（`libs/common`、`libs/oauth2`、`libs/identity`）**不得**直接使用 Drogon 的 `LOG_*` 宏，必须经 `fulla::common::ports::ILogger` 端口，以便单元测试可用 `FakeLogger` 捕获并断言。
- Adapter / 基础设施层（`libs/drogon`、`libs/storage-*`、`apps/server`）可直接使用 `LOG_*` 宏。
- 六级对应关系：`LogLevel::Trace`→`LOG_TRACE`、`Debug`→`LOG_DEBUG`、`Info`→`LOG_INFO`、`Warn`→`LOG_WARN`、`Error`→`LOG_ERROR`、`Fatal`→`LOG_FATAL`。

> 关于测试输出的最小化策略（ctest 默认仅打印失败用例与汇总），见 [testing-guide.md](../contribute/testing-guide.md)。
