# Connection churn diagnostic for #303

This standalone fixture isolates HTTP connection handling from Fulla, PostgreSQL,
Redis and authentication. It is diagnostic infrastructure, not evidence that
#303 is fixed. The known upstream static-destructor issue #300 is out of scope.

Use the repository's Drogon 1.9.13 Conan toolchain in an isolated Linux test
environment. Configure this directory separately; it is not in the product build:

```sh
cmake -S benchmarks/diagnostics/connection-churn -B build/connection-churn \
  -DCMAKE_TOOLCHAIN_FILE=/absolute/path/to/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/connection-churn
./build/connection-churn/fulla-connection-probe 18080 4 true
```

In another terminal, run each mode, using a fresh result filename:

```sh
python3 benchmarks/diagnostics/connection-churn/probe.py \
  --mode fresh --duration 600 --concurrency 8 --max-requests-per-worker 40000 \
  --output /tmp/churn-fresh-4loops-reuse-on-1.json
python3 benchmarks/diagnostics/connection-churn/probe.py \
  --mode keepalive --duration 600 --concurrency 8 --max-requests-per-worker 40000 \
  --output /tmp/churn-keepalive-4loops-reuse-on-1.json
```

Repeat with server arguments `18080 1 false` and `18080 4 false`, and repeat
each cell three times. Record OS, compiler, build type, Drogon version and
effective settings with the raw results. The probe only contacts
`127.0.0.1:/ping`, never follows redirects and never retries a failed request.
Each worker uses its own persistent connection in keepalive mode. The rate,
duration, concurrency, body size, request timeout and retained attempt count
are bounded. A nonzero exit indicates observed failed attempts; argument/file
errors also fail rather than overwriting an existing result file.

Every attempt records the phase, timing, status/error class, whether a connection
was opened and the server's handled counter. Preserve failures in the report;
do not replace them with the result of a successful retry. These timings include
connection setup, and are not a production IAM throughput benchmark.

During a failure collect `ss -tanp` for the fixture port, server fd counts,
thread stacks, process CPU/RSS and kernel listen-drop counters. Compare changes
against pre-run values. The client report alone cannot establish whether an
unserviced connection originated in the server, client or kernel. No failure in
a bounded run is a "not reproduced" result, not proof of a fix. Keep process
shutdown outcomes separate from request-path observations.

## Serial Linux matrix

After building the fixture, use the standard-library-only runner:

```sh
python3 benchmarks/diagnostics/connection-churn/test_probe.py
FULLA_CONNECTION_PROBE_BINARY="$PWD/build/connection-churn/fulla-connection-probe" \
  python3 benchmarks/diagnostics/connection-churn/test_matrix.py
python3 benchmarks/diagnostics/connection-churn/run_matrix.py \
  --binary ./build/connection-churn/fulla-connection-probe \
  --output /tmp/connection-matrix-unique
```

The output directory must not exist. The default run takes approximately three
hours: six configurations, three rounds, 600 seconds per cell, eight workers at
up to 50 requests/second each, a five-second request timeout, two-second startup settle,
and ten-second drain. In-flight requests retain their full timeout after the
launch window ends. This is a bounded diagnostic load, not a maximum-throughput
test; absence of failures at this rate cannot refute failures at higher rates.
Use actual attempt counts and elapsed time to report achieved rates; the pacer
does not guarantee the requested cap is reached.
Run without concurrent builds, tests or other benchmark clients on the machine.

**Linux configuration semantics matter.** In Drogon 1.9.13,
`ListenerManager::createListeners()` creates one `HttpServer` per IO loop, and
`HttpServer` uses Trantor's default `reUsePort=true`. On Linux, the application
`reuse_port=false` setting adds a startup port-availability check; it does not
disable `SO_REUSEPORT` on the listeners. Consequently these six configurations
are not an on/off experiment for the kernel socket option. The fixture logs
`getsockopt(SO_REUSEPORT)` before listening; the runner verifies these readings,
effective thread settings, actual IO thread names and PID-owned listeners before
measurement. It never patches upstream or changes production configuration.

Each cell writes:

- `attempts.jsonl.gz`: every completed attempt, written as it finishes under a
  lock; failures are retained without retry. A later request may reconnect.
- `snapshots.jsonl.gz`: baseline, five-second samples, first-failure diagnostics,
  measurement end and drained snapshots. Server `sport` and client `dport` views
  are separate; PID-owned server states are counted separately from port-wide
  states. Periodic samples read `/proc/PID/net/tcp` (IPv4, matching this fixture)
  once and join socket inodes from `/proc/PID/fd`, retaining non-TIME-WAIT rows
  and counts of every state. This avoids serializing thousands of TIME-WAIT rows
  through two `ss` child processes every five seconds. Baseline, measurement-end,
  drained and failure snapshots retain complete `ss` dumps. A periodic socket
  sample exceeding five seconds invalidates the cell, as does an `ss` timeout.
- `summary.json`, `probe-summary.json` and `server.log`: validity, request/error
  counts, latency, new connections, fd high-water/drained counts, CLOSE-WAIT
  samples, kernel counter deltas, and the separate shutdown result.

`environment.json` and `matrix-summary.json` describe the run. Also retain the
build's CMake cache/flags, exact Drogon and Trantor Conan recipe/package revisions,
image ID and source revision with the evidence. The runner refuses a new cell
with less than 80 MiB free; continuously streamed gzip data bounds disk usage.
Cleanly interrupted cells remain invalid. Abrupt termination can truncate the
last gzip stream; retain it as partial evidence and never count it as a complete
cell. There is no resume/overwrite mode: start a separate output directory.
To replace invalid cells, select configurations with repeatable `--scenario`
(for example `--scenario 4-true-fresh --rounds 1 --first-round 1`) and always
use a new output directory. `--first-round` labels the first round; the last
round cannot exceed three. Preserve invalid runs separately and explicitly list
which valid replacement supplies each cell. These options never change workload,
sampling limits or validity criteria; the default remains the full matrix.
The real-fixture smoke checks deliberately pause a fixture briefly to verify
failure capture, and send SIGTERM to verify cancellation and cleanup. These
injected failures are tool validation, never evidence of reproducing #303.

Deep snapshots capture thread names, wait channels, accessible kernel stacks,
epoll fdinfo, and a bounded `gdb thread apply all bt` attempt. Missing tools and
permission errors remain visible; a failed stack capture is not evidence about
the connection's root cause. Kernel counters are network-namespace-wide, not
attributed to one process. First-failure capture perturbs the failed cell and must
be disclosed when interpreting its timings. An unexpected monitor/process error
invalidates a cell. A valid cell can still contain request failures.

Exit codes: `0` means all cells valid with no observed request failures, `1` means
valid cells contain failures, and `2` means an invalid cell stopped the matrix.
Argument, filesystem and startup errors also return nonzero. No result closes
#303 automatically. Keep raw evidence local unless deliberately publishing a
sanitized, reproducible report.

See the [2026-11-07 Linux investigation](results/2026-11-07-linux.md) for a completed
18-cell run, preliminary failures, build provenance and the remaining limits.
