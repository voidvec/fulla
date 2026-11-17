"""Fixed low-rate observations; no workload, build or source scan in the sampler."""

from __future__ import annotations

import gzip
import json
import os
import resource
import threading
import time
from pathlib import Path

from fixture import Database
from protocol import Session
from support import Redis


def process_stats(pid):
    base = Path("/proc") / str(pid)
    result = {}
    wanted = {"VmRSS", "VmSize", "RssAnon", "RssFile", "Threads"}
    for line in (base / "status").read_text().splitlines():
        key, _, value = line.partition(":")
        if key in wanted:
            number = int(value.strip().split()[0])
            result[key + ("_bytes" if key != "Threads" else "")] = number * (
                1024 if "kB" in value else 1
            )
    try:
        for line in (base / "smaps_rollup").read_text().splitlines():
            if line.startswith("Pss:"):
                result["Pss_bytes"] = int(line.split()[1]) * 1024
    except (OSError, PermissionError):
        result["Pss_bytes"] = None
    descriptors = list((base / "fd").iterdir())
    result["fd_count"] = len(descriptors)
    inodes = set()
    for descriptor in descriptors:
        try:
            value = os.readlink(descriptor)
            if value.startswith("socket:["):
                inodes.add(value[8:-1])
        except OSError:
            pass
    states = {}
    for name in ("tcp", "tcp6"):
        for line in (base / "net" / name).read_text().splitlines()[1:]:
            fields = line.split()
            if len(fields) > 9 and fields[9] in inodes:
                states[fields[3]] = states.get(fields[3], 0) + 1
    result["tcp_state_hex_counts"] = states
    stat = (base / "stat").read_text().rsplit(")", 1)[1].split()
    result["process_cpu_seconds"] = (int(stat[11]) + int(stat[12])) / os.sysconf(
        "SC_CLK_TCK"
    )
    for name in ("cpu.stat", "memory.current", "memory.events"):
        path = Path("/sys/fs/cgroup") / name
        if path.exists():
            result["cgroup_" + name.replace(".", "_")] = path.read_text().strip()
    return result


class Sampler:
    def __init__(self, fixture, output, *, interval=5, pacer=None):
        import queue

        self.fixture, self.output, self.interval, self.pacer = (
            fixture,
            output,
            interval,
            pacer,
        )
        self.stop_event = threading.Event()
        self.initialized = threading.Event()
        self.error_lock = threading.Lock()
        self.errors = self.samples = 0
        self.failure_type = None
        self.session = self.control_session = None
        self.metrics_thread = None
        self.metrics_queue = queue.SimpleQueue()
        self.thread = threading.Thread(
            target=self.work, name="s07-observer", daemon=True
        )

    def fail(self, category):
        with self.error_lock:
            self.errors += 1
            self.failure_type = self.failure_type or category

    def start(self):
        self.thread.start()
        if not self.initialized.wait(5):
            raise RuntimeError("observer initialization timed out")
        if self.failure_type:
            raise RuntimeError("observer initialization failed")

    def stop(self):
        self.stop_event.set()
        for session in (self.session, self.control_session):
            if session:
                session.interrupt()
        self.thread.join(timeout=30)
        if self.thread.is_alive():
            raise RuntimeError("observer did not stop within its deadline")

    def metrics_work(self):
        pace = (
            (lambda path: self.pacer.wait(path, cancel_event=self.stop_event))
            if self.pacer
            else None
        )
        session = Session(self.fixture["target"], timeout=5, pace=pace)
        self.session = session
        try:
            while not self.stop_event.wait(60):
                row = {
                    "type": "metrics",
                    "start_ns": time.perf_counter_ns(),
                    "wall_ns": time.time_ns(),
                }
                try:
                    response = session.request("observer_metrics", "GET", "/metrics")
                    text = response["body"].decode()
                    row["metrics"] = [
                        line
                        for line in text.splitlines()
                        if line.startswith(("fulla_cache_", "oauth2_", "fulla_"))
                    ]
                except Exception as exc:  # noqa: BLE001 - preserve worker/cleanup failures in evidence
                    if self.stop_event.is_set():
                        break
                    self.fail(type(exc).__name__)
                    row["sample_error_type"] = type(exc).__name__
                row["end_ns"] = time.perf_counter_ns()
                self.metrics_queue.put(row)
        except BaseException as exc:  # noqa: BLE001 - preserve worker/cleanup failures in evidence
            self.fail(type(exc).__name__)
        finally:
            session.close()

    def work(self):
        try:
            self.collect()
        except BaseException as exc:  # noqa: BLE001 - preserve worker/cleanup failures in evidence
            self.fail(type(exc).__name__)
        finally:
            self.initialized.set()

    def collect(self):
        import queue

        db = Database(self.fixture)
        redis = Redis(self.fixture["redis_port"], self.fixture["redis_password"])
        control = Session(self.fixture["target"], timeout=5)
        self.control_session = control
        stream = self.output.open("xb")
        zipped = gzip.GzipFile(fileobj=stream, mode="wb", compresslevel=3, mtime=0)
        self.metrics_thread = threading.Thread(
            target=self.metrics_work, name="s07-metrics", daemon=True
        )
        self.metrics_thread.start()
        self.initialized.set()
        previous = None
        boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        try:
            while not self.stop_event.is_set():
                start = time.monotonic()
                cpu_before = resource.getrusage(resource.RUSAGE_THREAD)
                row = {
                    "type": "resources",
                    "monotonic_ns": time.perf_counter_ns(),
                    "wall_ns": time.time_ns(),
                    "queue_scope": "PostgreSQL lock waits and TCP states; internal app queue not exposed",
                }
                try:
                    if (
                        Path("/proc/sys/kernel/random/boot_id").read_text().strip()
                        != boot
                    ):
                        raise RuntimeError("environment_boot_changed")
                    if previous:
                        mono = (row["monotonic_ns"] - previous["monotonic_ns"]) / 1e9
                        wall = (row["wall_ns"] - previous["wall_ns"]) / 1e9
                        if mono > self.interval * 3 or abs(wall - mono) > 2:
                            raise RuntimeError("observation_clock_gap")
                    row["process"] = process_stats(self.fixture["server_pid"])
                    row["postgres"] = db.stats()
                    info = redis.command("INFO", "memory")
                    stats = {}
                    for line in info.splitlines():
                        k, _, v = line.partition(":")
                        if k in ("used_memory", "used_memory_rss", "maxmemory"):
                            stats[k] = int(v)
                    row["redis"] = stats
                    if self.fixture["profile"] != "soak":
                        begin = time.perf_counter_ns()
                        response = control.request(
                            "same_window_discovery",
                            "GET",
                            "/.well-known/openid-configuration",
                        )
                        value = json.loads(response["body"])
                        if not value.get("issuer") or not value.get("jwks_uri"):
                            raise ValueError("control discovery shape")
                        row["same_window_control"] = {
                            "start_ns": begin,
                            "end_ns": time.perf_counter_ns(),
                            "status": response["status"],
                        }
                except Exception as exc:  # noqa: BLE001 - preserve worker/cleanup failures in evidence
                    if self.stop_event.is_set():
                        break
                    self.fail(type(exc).__name__)
                    row["sample_error_type"] = type(exc).__name__
                cpu_after = resource.getrusage(resource.RUSAGE_THREAD)
                row["observer_cpu_seconds"] = (
                    cpu_after.ru_utime
                    + cpu_after.ru_stime
                    - cpu_before.ru_utime
                    - cpu_before.ru_stime
                )
                row["observer_wall_seconds"] = time.monotonic() - start
                zipped.write((json.dumps(row, separators=(",", ":")) + "\n").encode())
                previous = row
                self.samples += 1
                while True:
                    try:
                        item = self.metrics_queue.get_nowait()
                    except queue.Empty:
                        break
                    zipped.write(
                        (json.dumps(item, separators=(",", ":")) + "\n").encode()
                    )
                zipped.flush()
                self.stop_event.wait(max(0, self.interval - (time.monotonic() - start)))
        finally:
            self.stop_event.set()
            if self.session:
                self.session.interrupt()
            self.metrics_thread.join(timeout=6)
            if self.metrics_thread.is_alive():
                self.fail("metrics_shutdown_timeout")
            control.close()
            redis.close()
            zipped.close()
            stream.close()
