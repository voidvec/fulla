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
