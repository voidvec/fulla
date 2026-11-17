# Capacity evidence protocol (S07)

S07 adds a reproducible, failure-preserving capacity harness. **No accepted
current-product capacity baseline is published by this change.** Functional
checks do not constitute a capacity result, an SLO, or S07 completion.
See the [operator instructions](https://github.com/voidvec/fulla/blob/master/benchmarks/capacity/README.md).
The dated [competitor report](https://github.com/voidvec/fulla/blob/master/benchmarks/competitors/results/COMPARISON.md)
retains its original version, dates and applicability.

## Prerequisites and reporting status

Record the exact product commit, compiler, C++ mode, Release flags, LTO state,
instrumentation state, CPU/memory limits and dependency versions in each run.
Use native Linux and real PostgreSQL + Redis. A colocated Python driver measures
a capacity lower bound; its CPU and the shared cgroup budget must be reported.
Sanitizer/coverage builds are rejected even if their build type says Release.

All real paired protocol smokes must pass before the pilot. A fresh issued token
returning inactive, a failed subject/client/organization binding, a missing
observer or a space guard failure invalidates the prerequisite. Retain the
failure; do not add waits, weaken validation or retry it away for a favorable row.
Review any required product fix independently from benchmark-only tooling.

A non-authorizing resource preflight can show that available persistent space
is insufficient, but cannot approve formal measurement. Only the successful
same-environment pilot supplies the measured raw/data/WAL-growth forecast.
The full 108-row matrix, two-hour soak and formal faults require their own
complete evidence and independent acceptance; short functional checks cannot
substitute for them. Publish environment/diagnostic artifacts only with the
repository owner's authorization, after reviewing them for credentials and
internal data.

## Scope and configuration boundaries

The change is confined to the benchmark harness, redacted evidence and docs.
It does not modify the server, SDK, SQL migrations, ORM, API or lock files.
It does not investigate the known shutdown issue #300 or macOS runner changes.
No historical competitor result is relabeled as a current measurement.

Both arms use the same product binary, fixture, security settings and durable
database. The base profile keeps auto threads (`0`), 10 PG/Redis connections
and DEBUG logs. The published tuned delta is two threads, 16 connections in
each pool, cache enabled and WARN logs. Both bound log retention identically,
disable relaunch/bootstrap-admin for the owned fixture, and use private loopback
listeners. Those fixture/retention deltas must not be described as byte-identical
factory defaults. Password hashing remains PBKDF2-SHA256/310000; token TTLs and
durability are preserved.

The reference lab PG profile uses 64MB shared buffers, 64 connections, 32/128MB min/max WAL,
60s checkpoint timeout and checkpoint logging. It keeps fsync, synchronous commit
and full-page writes on. These are lab parameters, not factory/production defaults.
The owned cluster isolates processes, stats and fault controls; it still shares
physical overlay capacity with the workspace. WAL can exceed its soft threshold.
Keep actual checkpoint counts, timings and events; 120s does not guarantee two
checkpoints. Bounded app/runtime logs may be on disclosed tmpfs; PG data is durable.

## Workload and execution order

Each of four VU owns a distinct account, cookie session and one-use RT chain.
Authorization flows generate new PKCE, OAuth state, OIDC nonce and server consent
CSRF; they verify subject, redirect, client and actual organization binding.
ID-token nonce/subject correlation is checked; this is not JWT signature validation.
Lost RT responses mark the chain uncertain; recovery authenticates afresh instead
of replaying the old RT. All such recovery attempts remain visible.

| Stage | Fixed protocol | Acceptance boundary |
|---|---|---|
| Tool checks | Protocol, deadlines, isolation, counts, redaction, budgets and lease tests | Pass before real smoke; no mocked pass substitutes for the stack |
| Real smoke | Default/tuned × nine scenarios, 4 VU, 0.2s + 1s | All pass before a capacity pilot; failed setup is retained |
| Pilot | Same 18 arms, 4 VU, 2s + 10s | All pass; measured bytes/growth bind the budget |
| Steady matrix | Nine scenarios × two profiles × VU 1/4 × three rounds | 108 unique rows, each 30s warmup + 120s measurement; round 2 reverses arm order |
| Soak | Mixed, 4 VU, real production Hodor, 30s + 7200s | Full raw, cleanup/checkpoint/resource series; no unexplained trends/failures |
| Faults | Separate tuned diagnostics: Redis, candidate DB proxy, backend restart | 30s baseline, at least 30s outage, three consecutive business successes within 300s recovery |

Nine scenarios cover discovery (zero-DB control), full password/PKCE/consent plus
userinfo, Basic client credentials, active-token cached introspection, precise
cold-cache control, RT rotation, userinfo, actual organization context and mixed.
Mixed uses 12 deterministic shuffled slots: password 1, organization 1, CC 1,
refresh 1, userinfo 3, introspection reuse 2, cold control 1, discovery 2.
A separate 0.2Hz discovery observer supplies same-window negative control in
non-soak rows; its traffic is not business QPS. Keep all three rounds.

## Timing and result interpretation

A logical flow is admitted before Redis control or pacing; pre-HTTP failures
still count. HTTP starts after pacing and has its own admission/completion.
Warmup stops admission and drains before measurement. The measured start cohort
is `[start, stop)`; no new flow enters after stop. Existing flows get a bounded
60s drain (300s soak). Absolute deadlines also cover setup, health and metrics.
An unfinished attempt, dead observer, exhausted attempt budget, failed cleanup or
space guard prevents a passed result. No unlimited shutdown waits are used.

Window-completion QPS counts successes completed inside the window divided by
120s. Cohort counts and latencies retain starts inside the window including
completions during drain; cohort throughput uses the actual full wall duration.
Do not divide drain completions by 120. Report HTTP and business-flow failures
separately, including 429, inactive tokens, timeouts and control failures.
All-attempt nearest-rank P50/P95/P99/max retain every tail. Closed-loop demand,
coordinated omission and shared Python-driver CPU limit production extrapolation.
Raw CSV preserves each row; no best-round selection or cleaned P99 is a headline.

## Resources, soak and fault observation

Formal entry is the serial `session.py`, using one canonical private fixture
file and its exclusive lock. It verifies DB/Redis ownership, listener/PID/binary,
runtime config, durable dependencies and real readiness before credentials.
Pilot budgets bind source/binary/nonce/boot, CPU/memory/kernel, PG settings and
source configs. Kind-specific profile/VU/warmup/window/drain scopes are exact;
an oversized steady window cannot borrow the soak budget. Retain all raw plus
an archive copy, data/WAL headroom and a 192MiB persistent reserve. Initialization
needs 320MiB free; continuous guards also cover setup, pilot and faults. A
non-authorizing preflight cannot replace a successful pilot.

During pilot and formal windows, run only the candidate stack, driver and fixed
observers. Stop compilation, tests, source scans and review agents. Resources
sample every 5s: process RSS/PSS when available, fd/threads/TCP, cgroup CPU/memory,
PG connections/lock waits/data/token rows/WAL/checkpoints and Redis memory.
No internal application queue metric is claimed. Retain raw checkpoint events
from the dedicated PG log; do not publish unrelated/private SQL logs.

The soak copies the real JSONC production Hodor policy and first proves a 429
from non-trusted loopback with a bounded burst, then restarts the candidate.
All HTTP, health and metrics share rolling quotas: all 8, login 1, token 2 per
60s. Resource reads are not extra HTTP. This low-rate production-policy run
observes two hours, not peak sustained capacity. Report actual TTL/cleanup
coverage; two hours cannot establish bounded 30-day RT retention or absence of leaks.

Fault controls affect only nonce-verified owned resources. The Redis host mediator
checks exact container ID/volume; the DB proxy cuts existing/new candidate streams
while upstream PG stays up. Restart affects only the owned candidate. Record
command, active, restore, first-success and three-stable timestamps, every probe
and actual probe gaps. Pacing can make recovery resolution much coarser than 1s.
Formal fault sampling requires adequate coverage; fallback/recovery failures remain
evidence. Short default-profile control smoke is distinct from tuned formal faults.

## Acceptance and follow-up

S07 is accepted only when all four layers agree: tested tools; successful real
protocol smoke; the complete matrix/soak/fault protocol with retained raw and no
unexplained failures; and an independently reviewed report matching that evidence.
This tools change publishes no accepted formal baseline. Mark incomplete runs
blocked and do not advance to S08 on the strength of functional checks.

Resolve and regression-test any real protocol failures without relaxing the
validation contract. Ensure enough real persistent headroom, then run smoke and
a fresh pilot; the pilot determines the full space requirement. Freeze the reviewed
source/binary/environment before the matrix. Repeat controls and review the
actual raw/cleanup trends after the long run. Throughput -10% or P99 +20% can
trigger a three-round regression review later; those are alerts, not promised SLOs.
