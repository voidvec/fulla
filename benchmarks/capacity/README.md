# Capacity evidence harness (S07)

This standard-library Python harness targets an **owned loopback PostgreSQL +
Redis fixture on native Linux**. It runs real OAuth sessions, records failures
and complete latency tails, and refuses formal runs without a matching pilot
budget. It changes no product code, schema, migration or public API.

**Status:** reusable tooling; no accepted current-product capacity baseline is
published here. See the [protocol and acceptance boundaries](../../docs/benchmark/capacity-evidence.md).
Functional smoke is not a capacity result. Historical competitor results remain
unchanged. Review and authorize any environment/diagnostic artifact before
publishing it.

## Prepare a disposable fixture

Requirements: Linux `/proc` and cgroup v2, Python 3.11+, `psql`, PostgreSQL 16,
Redis 7, and a verified, non-instrumented Release server. Use the repository's
normal build instructions; keep build logs, CMake cache and binary hash.
Sanitizer/coverage builds are rejected even if their build type is Release.

Use an empty, **dedicated durable PostgreSQL cluster**. The storage profile used
in this lab is `shared_buffers=64MB`, `max_connections=64`,
`min_wal_size=32MB`, `max_wal_size=128MB`, `checkpoint_timeout=60s`,
`log_checkpoints=on`; `fsync`, `full_page_writes` and `synchronous_commit` stay on.
These are disclosed lab settings, not PostgreSQL factory or production defaults.
Keep data on persistent storage, not tmpfs. `max_wal_size` is a soft checkpoint
trigger, not a disk cap; record actual checkpoints, WAL and filesystem space.
Do not share a production or another task's Redis instance.

Generate credentials in a private directory outside the tracked tree:

```bash
umask 077
python3 benchmarks/capacity/lease.py \
  --binary /absolute/build/apps/server/fulla-server \
  --build-cache /absolute/build/CMakeCache.txt \
  --product-commit <40-character-product-commit> \
  --output /secure/s07/fixture.json --sql-output /secure/s07/provision.sql
```

This only creates private files; it does not contact databases or Docker.
Review the CREATE-only SQL, then apply it as an administrator to the new cluster:

```bash
psql -X -w -v ON_ERROR_STOP=1 -p 25433 -U postgres -d postgres \
  -f /secure/s07/provision.sql
```

Create the Redis container from the private lease. This example uses Linux host
networking; the driver/server/PG must share the same loopback namespace. Use a
new `--redis-container fulla-s07-<name>` at generation if that name already exists.

```python
import json, subprocess

f = json.load(open("/secure/s07/fixture.json"))
i = f["infrastructure"]
subprocess.run(
    [
        "docker",
        "run",
        "-d",
        "--name",
        i["redis_container"],
        "--network",
        "host",
        "--mount",
        "type=volume,source=" + i["redis_volume"] + ",target=/data",
        "redis:7-alpine",
        "redis-server",
        "--bind",
        "127.0.0.1",
        "--port",
        str(f["redis_port"]),
        "--requirepass",
        f["redis_password"],
    ],
    check=True,
    stdout=subprocess.DEVNULL,
)
```

Default Redis RDB persistence remains enabled. The fixture seed deliberately
uses development clients and four random PBKDF2-SHA256/310000 accounts; isolate
it from real traffic. Organization clients are created and transferred through
the real APIs. No token is fabricated in SQL. Reuse **one canonical fixture file**
for all sessions; do not copy a lease to run it concurrently in another directory
or container. The exclusive file lock protects reset/start for that canonical
lease. A reset truncates only its nonce-verified disposable token/consent/audit
tables while the candidate is stopped. It preserves account/org/client fixture data.

## Run and retain evidence

Set task-specific paths, then use the serial orchestrator. Run it where the
server, `psql`, fixture and loopback dependencies are accessible:

```bash
S07_FIXTURE=/secure/s07/fixture.json
S07_BINARY=/absolute/build/apps/server/fulla-server
S07_RUNTIME=/secure/s07/runtime
S07_EVIDENCE=/persistent/s07-evidence
python3 benchmarks/capacity/session.py --fixture "$S07_FIXTURE" \
  --binary "$S07_BINARY" --runtime "$S07_RUNTIME" \
  --evidence "$S07_EVIDENCE/smoke-01" --mode smoke
python3 benchmarks/capacity/session.py --fixture "$S07_FIXTURE" \
  --binary "$S07_BINARY" --runtime "$S07_RUNTIME" \
  --evidence "$S07_EVIDENCE/limiter-01" --mode limiter-smoke
```

`smoke` runs default/tuned × nine scenarios at 4 VU, with 0.2s warmup and 1s
measurement. `limiter-smoke` copies the actual JSONC production Hodor policy and
requires a real 429 in a bounded discovery burst from untrusted loopback. Each
candidate is restarted after that proof. Neither mode measures capacity.

After smoke passes, stop builds, tests, review agents and unrelated load for
**both pilot and formal windows**:

```bash
python3 benchmarks/capacity/session.py --fixture "$S07_FIXTURE" \
  --binary "$S07_BINARY" --runtime "$S07_RUNTIME" \
  --evidence "$S07_EVIDENCE/pilot-01" --mode pilot
python3 benchmarks/capacity/budget.py --fixture "$S07_FIXTURE" \
  --pilot "$S07_EVIDENCE/pilot-01" --output "$S07_EVIDENCE/budget-01.json"
python3 benchmarks/capacity/session.py --fixture "$S07_FIXTURE" \
  --binary "$S07_BINARY" --runtime "$S07_RUNTIME" \
  --evidence "$S07_EVIDENCE/matrix-01" --mode matrix \
  --budget-plan "$S07_EVIDENCE/budget-01.json"
python3 benchmarks/capacity/session.py --fixture "$S07_FIXTURE" \
  --binary "$S07_BINARY" --runtime "$S07_RUNTIME" \
  --evidence "$S07_EVIDENCE/soak-01" --mode soak \
  --budget-plan "$S07_EVIDENCE/budget-01.json"
```

Pilot: 18 rows, 2s warmup + 10s measurement; all must pass. Matrix: 108 unique
rows, 30s + 120s, VU 1/4, three rounds, reversed profile order in round 2.
Soak: 30s + 7200s, four VU, production Hodor policy. One rolling pacer includes
every HTTP, health and metrics request: all=8/login=1/token=2 per 60s. It is a
low-rate production-policy soak, not a sustained peak-capacity claim.

Budget fingerprints bind source, binary, fixture, boot, kernel/cgroup resources,
PostgreSQL settings and base/production config sources. Windows and drains are
fixed per kind. The forecast includes all compressed raw, an archive copy,
dataset/WAL headroom and a 192MiB persistent reserve. Startup needs 320MiB free;
continuous guards cover migration, setup, pilot, all rows and faults. A separate
non-authorizing preflight is available as `budget.py --preflight --fixture ...
--output ...`; it always reports blocked pending the successful pilot.

Use only `session.py` for formal runs/reset. `run.py` is its single-row child;
calling it alone cannot establish the paired matrix or S07 acceptance.
Output directories/files refuse replacement. On failure, preserve the entire
directory and choose a new name after fixing the cause. Do not retry away failed
rows, filter tails, or run unrelated work alongside measurement.

## Fault controls and exports

Start the host-side Redis mediator with a host-readable private lease; its Unix
socket must be in a private directory shared with the driver. It validates the
exact dedicated container ID and nonce-derived volume. It supports only that
container's stop/start/status; no Docker socket is mounted into the driver.

```bash
python3 benchmarks/capacity/redis_control.py --fixture "$S07_FIXTURE" \
  --socket /secure/s07/redis-control.sock
```

In another shell, use the same `session.py` arguments plus `--mode fault-smoke
--redis-control-socket /secure/s07/redis-control.sock`. This validates 2s controls
against the default profile. Formal `--mode faults` additionally requires the
passed budget and uses tuned: 30s baseline, >=30s outage, <=300s recovery.
It stops only the owned Redis; DB failure closes/refuses candidate proxy streams,
while the upstream cluster stays up; restart affects only the owned server.
All business attempts remain recorded. Recovery requires three consecutive
validated CC + active introspection flows. HTTP pacing can make observation
gaps much longer than the nominal sleep; use actual probe/event timestamps and
reported maximum gaps, not a claimed 1s recovery resolution. Resources sample
every 5s; failed coverage or cleanup invalidates a result. Stop the mediator
normally to trigger restoration; a killed host/process needs operator state checks.

```bash
python3 -m unittest discover -s benchmarks/capacity -v
python3 benchmarks/capacity/report.py --session "$S07_EVIDENCE/matrix-01" \
  --output "$S07_EVIDENCE/matrix-01.csv"
```

CSV exports every row separately, including incomplete/failed rows, all-attempt
P50/P95/P99/max, window completions and cohort counts. No automatic S07 pass or
production SLO is inferred. Retain `session.json`, row results, raw attempt and
telemetry gzip, effective sanitized configs and checkpoint events together.
Raw records allow only timing/stage/status/counters, excluding URLs, bodies,
tokens, cookies, passwords and OAuth state. **Private leases, generated SQL,
runtime config/key files and server console/app logs contain credentials and must
stay private.** The runtime's bounded logs are not part of the public raw archive.
