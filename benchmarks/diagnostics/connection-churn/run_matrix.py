#!/usr/bin/env python3
"""Serial, bounded Linux-only #303 matrix with raw per-attempt evidence."""

import argparse
import gzip
import hashlib
import http.client
import json
import platform
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from collections import Counter
from pathlib import Path
from types import SimpleNamespace

import probe


def command(argv, timeout=5):
    try:
        result = subprocess.run(argv, check=False, text=True, capture_output=True, timeout=timeout)
        return {"argv": argv, "returncode": result.returncode,
                "stdout": result.stdout, "stderr": result.stderr}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"argv": argv, "error": str(exc)}


def read(path):
    try:
        return Path(path).read_text()
    except OSError as exc:
        return f"unavailable: {exc}"


def kernel_counters():
    values = {}
    for name in ("netstat", "snmp"):
        lines = read(f"/proc/net/{name}").splitlines()
        for index in range(0, len(lines) - 1, 2):
            keys, data = lines[index].split(), lines[index + 1].split()
            for key, value in zip(keys[1:], data[1:]):
                try:
                    values[keys[0] + key] = int(value)
                except ValueError:
                    pass
    return values


def proc_sockets(pid, port):
    """Read the IPv4 namespace table once; attribute live sockets by inode."""
    states = {"01": "ESTAB", "02": "SYN-SENT", "03": "SYN-RECV", "04": "FIN-WAIT-1",
              "05": "FIN-WAIT-2", "06": "TIME-WAIT", "07": "CLOSE", "08": "CLOSE-WAIT",
              "09": "LAST-ACK", "0A": "LISTEN", "0B": "CLOSING"}
    owned = set()
    for fd in Path(f"/proc/{pid}/fd").iterdir():
        try:
            target = fd.readlink().as_posix()
        except FileNotFoundError:
            continue  # A connection can close between directory read and readlink.
        match = re.fullmatch(r"socket:\[(\d+)\]", target)
        if match:
            owned.add(match.group(1))
    rows = {"server": [], "client": []}
    for line in Path(f"/proc/{pid}/net/tcp").read_text().splitlines()[1:]:
        fields = line.split()
        local_port = int(fields[1].split(":")[1], 16)
        peer_port = int(fields[2].split(":")[1], 16)
        state = states.get(fields[3], "UNKNOWN_" + fields[3])
        row = {"state": state, "owned": fields[9] in owned, "raw": line}
        if local_port == port:
            rows["server"].append(row)
        if peer_port == port:
            rows["client"].append(row)
    return rows


def snapshot(pid, port, deep=False):
    root = Path(f"/proc/{pid}")
    result = {"utc": probe.utc_now(), "monotonic": time.monotonic(), "pid": pid,
              "kernel_counters": kernel_counters()}
    try:
        status = dict(line.split(":", 1) for line in (root / "status").read_text().splitlines())
        result["process"] = {key: status.get(key, "").strip() for key in
                             ("State", "Threads", "VmRSS", "VmSize")}
        result["fd_count"] = len(list((root / "fd").iterdir()))
        result["stat"] = (root / "stat").read_text()
    except OSError as exc:
        result["process_error"] = str(exc)
    # Periodic samples avoid starting two ss processes and serializing thousands
    # of TIME-WAIT rows. Read the same namespace directly and join live inodes.
    periodic_start = time.monotonic()
    if not deep:
        try:
            views = proc_sockets(pid, port)
        except (OSError, ValueError, IndexError) as exc:
            result["process_error"] = f"socket table unavailable: {exc}"
            views = {"server": [], "client": []}
    # sport selects only the server's half; dport selects the client's half.
    for side, endpoint in (("server", "sport"), ("client", "dport")):
        if not deep:
            rows = views[side]
            result[side + "_states"] = dict(Counter(row["state"] for row in rows))
            if side == "server":
                result["server_owned_states"] = dict(Counter(
                    row["state"] for row in rows if row["owned"]))
            result[side + "_sockets"] = {
                "source": f"/proc/{pid}/net/tcp", "returncode": 0,
                "stdout": "\n".join(row["raw"] for row in rows if row["state"] != "TIME-WAIT"),
                "omitted_time_wait_rows": sum(row["state"] == "TIME-WAIT" for row in rows)}
            continue
        raw = command(["ss", "-Htanp", f"{endpoint} = :{port}"])
        lines = raw.get("stdout", "").splitlines()
        result[side + "_states"] = dict(Counter(
            line.split()[0] for line in lines if line.split()))
        if side == "server":
            owned = [line for line in lines if re.search(rf"\bpid={pid},", line)]
            result["server_owned_states"] = dict(Counter(line.split()[0] for line in owned))
        result[side + "_sockets"] = raw
    if not deep:
        result["socket_sample_duration_s"] = time.monotonic() - periodic_start
        if result["socket_sample_duration_s"] > 5:
            result["process_error"] = "periodic socket sampling exceeded five seconds"
    if deep:
        result["threads"] = [{"tid": task.name, "comm": read(task / "comm"),
                              "wchan": read(task / "wchan"), "stack": read(task / "stack")}
                             for task in sorted((root / "task").glob("*"))]
        result["fdinfo"] = {fd.name: read(root / "fdinfo" / fd.name)
                            for fd in (root / "fd").glob("*")}
        result["userspace_stacks"] = command(
            ["gdb", "-q", "-batch", "-ex", "set pagination off", "-ex",
             "thread apply all bt", "-p", str(pid)], timeout=10)
    return result


def ping(port):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
    try:
        conn.request("GET", "/ping", headers={"Connection": "close"})
        response = conn.getresponse()
        body = response.read(1025)
        if response.status != 200 or body != b"pong":
            raise RuntimeError("unexpected readiness response")
        return {"threads": int(response.getheader("X-Diagnostic-Threads")),
                "reuse_port": response.getheader("X-Diagnostic-Reuse-Port"),
                "handled": int(response.getheader("X-Diagnostic-Handled"))}
    finally:
        conn.close()


def write_json(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def cell(args, directory, threads, reuse, mode):
    port = args.port
    # Do not accidentally probe an existing local service.
    with socket.socket() as guard:
        # Previous cells legitimately leave TIME-WAIT sockets on this port.
        guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        guard.bind(("127.0.0.1", port))
    directory.mkdir()
    stop_monitor = threading.Event()
    sample_lock = threading.Lock()
    failure_count = 0
    peak_fd = 0
    close_wait_samples = []
    sample_errors = []
    server = None
    monitor_thread = None
    started = time.monotonic()
    outcome = {"threads": threads, "reuse_port": reuse, "mode": mode,
               "started_at": probe.utc_now(), "valid": False}
    with (directory / "server.log").open("x") as log, gzip.open(
            directory / "snapshots.jsonl.gz", "xt", encoding="utf-8") as samples, gzip.open(
            directory / "attempts.jsonl.gz", "xt", encoding="utf-8") as attempts:
        try:
            server = subprocess.Popen([str(args.binary), str(port), str(threads), reuse],
                                      stdout=log, stderr=subprocess.STDOUT)

            def collect(kind, deep=False, failure=None):
                nonlocal peak_fd
                with sample_lock:
                    sample = snapshot(server.pid, port, deep)
                    sample["kind"] = kind
                    if "process_error" in sample:
                        sample_errors.append({"process_error": sample["process_error"]})
                    if failure is not None:
                        sample["failure"] = failure
                    peak_fd = max(peak_fd, sample.get("fd_count", 0))
                    close_wait_samples.append({"kind": kind, "utc": sample["utc"],
                                               "count": sample.get("server_owned_states", {}).get(
                                                   "CLOSE-WAIT", 0)})
                    for side in ("server", "client"):
                        raw = sample[side + "_sockets"]
                        if raw.get("returncode") != 0:
                            sample_errors.append(raw)
                    samples.write(json.dumps(sample, allow_nan=False) + "\n")
                    samples.flush()
                    return sample

            for _ in range(100):
                if server.poll() is not None:
                    raise RuntimeError(f"server exited during readiness: {server.returncode}")
                try:
                    ready = ping(port)
                    break
                except (OSError, http.client.HTTPException):
                    time.sleep(.1)
            else:
                raise RuntimeError("server readiness timed out")
            if ready["threads"] != threads or ready["reuse_port"] != reuse:
                raise RuntimeError(f"effective configuration mismatch: {ready}")
            time.sleep(args.warmup)
            baseline = collect("baseline", deep=True)
            # Drogon 1.9.13's Linux ListenerManager creates one listener per
            # IO loop even when app.reuse_port=false (that option controls the
            # startup port-availability check, not SO_REUSEPORT on these sockets).
            expected_listeners = threads
            listeners = baseline["server_owned_states"].get("LISTEN", 0)
            if listeners != expected_listeners:
                raise RuntimeError(f"expected {expected_listeners} listeners, observed {listeners}")
            outcome["effective"] = ready
            outcome["listeners"] = listeners
            io_threads = sum(task["comm"].strip() == "DrogonIoLoop"
                             for task in baseline.get("threads", []))
            if io_threads != threads:
                raise RuntimeError(f"expected {threads} IO threads, observed {io_threads}")
            outcome["io_threads"] = io_threads
            socket_options = re.findall(r"listener_fd=(\d+) so_reuseport=(-?\d+) rc=(-?\d+)",
                                        (directory / "server.log").read_text())
            outcome["listener_socket_options"] = socket_options
            if len(socket_options) != threads or any(value != "1" or rc != "0"
                                                     for _, value, rc in socket_options):
                raise RuntimeError(f"unexpected Linux SO_REUSEPORT readings: {socket_options}")
            outcome["baseline_fd"] = baseline.get("fd_count")

            def monitor():
                while not stop_monitor.wait(args.sample_interval):
                    try:
                        collect("periodic")
                    except Exception as exc:  # noqa: BLE001 -- persist any collector failure as invalid
                        sample_errors.append({"monitor_error": repr(exc)})
                        return

            def on_failure(record):
                nonlocal failure_count
                # Only the first failure is instrumented to limit perturbation.
                with failure_lock:
                    failure_count += 1
                    first = failure_count == 1
                if first:
                    try:
                        collect("first_failure", deep=True, failure=dict(record))
                    except Exception as exc:  # noqa: BLE001 -- retain failures in evidence collection
                        sample_errors.append({"failure_capture_error": repr(exc)})

            failure_lock = threading.Lock()
            attempt_lock = threading.Lock()

            def on_attempt(record):
                with attempt_lock:
                    attempts.write(json.dumps(record, separators=(",", ":")) + "\n")

            monitor_thread = threading.Thread(target=monitor, daemon=True)
            monitor_thread.start()
            parameters = SimpleNamespace(port=port, mode=mode, duration=args.duration,
                                         concurrency=args.concurrency, timeout=args.timeout,
                                         rate_per_worker=args.rate_per_worker,
                                         max_requests_per_worker=args.max_requests_per_worker)
            try:
                report = probe.run(parameters, on_failure=on_failure, on_attempt=on_attempt,
                                   cancel_event=args.cancel_event)
            finally:
                stop_monitor.set()
                monitor_thread.join()
            measured_end = collect("measurement_end", deep=True)
            time.sleep(args.drain)
            drained = collect("drained", deep=True)
            outcome["final_ping"] = ping(port)
            outcome["summary"] = report["summary"]
            outcome["failures_by_stage"] = dict(Counter(
                row["stage"] for row in report["attempts"] if row["error"]))
            outcome["failure_errors"] = dict(Counter(
                row["error"] for row in report["attempts"] if row["error"]))
            outcome["peak_fd"] = peak_fd
            outcome["end_fd"] = measured_end.get("fd_count")
            outcome["drained_fd"] = drained.get("fd_count")
            outcome["server_close_wait"] = close_wait_samples
            outcome["kernel_delta"] = {
                key: value - baseline["kernel_counters"].get(key, value)
                for key, value in drained["kernel_counters"].items()}
            outcome["sample_errors"] = sample_errors
            write_json(directory / "probe-summary.json", {
                key: value for key, value in report.items() if key != "attempts"})
            outcome["valid"] = (report["summary"]["duration_s"] >= args.duration * .99
                                and not sample_errors and server.poll() is None)
        except Exception as exc:  # noqa: BLE001 -- diagnostic boundary must save partial evidence
            outcome["error"] = repr(exc)
        finally:
            stop_monitor.set()
            if monitor_thread is not None:
                monitor_thread.join()
            if server is not None:
                if server.poll() is None:
                    server.send_signal(signal.SIGTERM)
                    try:
                        server.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()
                        outcome["shutdown_forced"] = True
                outcome["shutdown_returncode"] = server.returncode
            outcome["wall_duration_s"] = time.monotonic() - started
            outcome["completed_at"] = probe.utc_now()
            write_json(directory / "summary.json", outcome)
    return outcome


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--duration", type=float, default=600)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--first-round", type=int, default=1)
    scenarios = {f"{threads}-{reuse}-{mode}": (threads, reuse, mode)
                 for threads, reuse in ((1, "false"), (4, "false"), (4, "true"))
                 for mode in ("fresh", "keepalive")}
    parser.add_argument("--scenario", choices=list(scenarios), action="append",
                        help="Select a cell configuration; repeat to select several (default: all)")
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument("--rate-per-worker", type=float, default=50)
    parser.add_argument("--max-requests-per-worker", type=int, default=40000)
    parser.add_argument("--timeout", type=float, default=5)
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--sample-interval", type=float, default=5)
    parser.add_argument("--warmup", type=float, default=2)
    parser.add_argument("--drain", type=float, default=10)
    args = parser.parse_args()
    args.cancel_event = threading.Event()

    def interrupt(signum, _frame):
        args.cancel_event.set()
        raise InterruptedError(f"matrix interrupted by signal {signum}")

    signal.signal(signal.SIGTERM, interrupt)
    signal.signal(signal.SIGINT, interrupt)
    import math
    bounds = {"duration": (.1, 600), "rounds": (1, 3), "first_round": (1, 3), "concurrency": (1, 64),
              "rate_per_worker": (.1, 1000), "max_requests_per_worker": (1, 100000),
              "timeout": (.05, 30), "port": (1024, 65535), "sample_interval": (1, 60),
              "warmup": (0, 60), "drain": (0, 60)}
    for name, (minimum, maximum) in bounds.items():
        if not math.isfinite(getattr(args, name)) or not minimum <= getattr(args, name) <= maximum:
            parser.error(f"{name} must be between {minimum} and {maximum}")
    if args.concurrency * args.max_requests_per_worker > 500000:
        parser.error("total attempt limit exceeds 500000")
    if args.max_requests_per_worker < args.duration * args.rate_per_worker:
        parser.error("request budget must cover the whole requested duration")
    if args.first_round + args.rounds > 4:
        parser.error("last round must not exceed 3")
    if args.scenario and len(set(args.scenario)) != len(args.scenario):
        parser.error("duplicate scenarios are not allowed")
    if sys.platform != "linux" or not shutil.which("ss"):
        parser.error("Linux and ss are required")
    args.binary = args.binary.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    write_json(args.output / "environment.json", {
        "created_at": probe.utc_now(), "platform": platform.platform(),
        "python": platform.python_version(), "parameters": {key: str(value) if isinstance(value, Path)
                 else value for key, value in vars(args).items() if key != "cancel_event"},
        "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        "source_sha256": {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                          for name in ("run_matrix.py", "probe.py", "server.cc")},
        "cpu": read("/proc/cpuinfo"), "memory": read("/proc/meminfo"),
        "cgroup_cpu_max": read("/sys/fs/cgroup/cpu.max"),
        "cgroup_memory_max": read("/sys/fs/cgroup/memory.max"),
        "compiler": command(["g++", "--version"]), "cmake": command(["cmake", "--version"]),
        "ss": command(["ss", "--version"]), "gdb": command(["gdb", "--version"]),
        "scope": "standalone request-path diagnostics; not a production performance benchmark"})
    outcomes = []
    try:
        for round_number in range(args.first_round, args.first_round + args.rounds):
            for scenario in args.scenario or scenarios:
                threads, reuse, mode = scenarios[scenario]
                name = f"round-{round_number}-{threads}loops-reuse-{reuse}-{mode}"
                print(json.dumps({"event": "start", "cell": name, "utc": probe.utc_now()}), flush=True)
                if shutil.disk_usage(args.output).free < 80 * 1024 * 1024:
                    raise RuntimeError("less than 80 MiB free; refusing a new cell")
                outcome = cell(args, args.output / name, threads, reuse, mode)
                outcomes.append({"cell": name, **outcome})
                print(json.dumps({"event": "complete", "cell": name,
                                  "valid": outcome["valid"], "summary": outcome.get("summary"),
                                  "error": outcome.get("error")}), flush=True)
                if not outcome["valid"]:
                    return 2
    except Exception as exc:  # noqa: BLE001 -- preserve matrix state on any setup/collection failure
        outcomes.append({"valid": False, "error": repr(exc)})
        return 2
    finally:
        write_json(args.output / "matrix-summary.json", outcomes)
    return 1 if any(outcome["summary"]["failures"] for outcome in outcomes) else 0


if __name__ == "__main__":
    raise SystemExit(main())
