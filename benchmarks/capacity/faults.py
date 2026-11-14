"""Separate, paced dependency-fault probes; never part of capacity throughput."""

from __future__ import annotations

import gzip
import hashlib
import json
import shutil
import socket
import threading
import time
from itertools import pairwise

from fixture import Server, verify_candidate
from observe import process_stats
from protocol import ProtocolError, Session, User
from support import Pacer, Recorder, Redis


def redis_control(experiment, action):
    path = experiment.args.redis_control_socket
    if path is None:
        raise ValueError("Redis fault requires an explicit owned host control socket")
    value = {
        "action": action,
        "proof": hashlib.sha256(experiment.fixture["owner_nonce"].encode()).hexdigest(),
    }
    with socket.socket(socket.AF_UNIX) as stream:
        stream.settimeout(25)
        stream.connect(str(path))
        stream.sendall(json.dumps(value).encode())
        result = json.loads(stream.recv(1024))
    if "error" in result or result.get("paused") != (action == "pause"):
        raise RuntimeError("owned Redis controller failed")


def run_fault(experiment, fault, *, smoke=False):
    """Retain every probe, including failures; only stable recovery passes."""
    if fault not in ("redis", "database", "restart"):
        raise ValueError("unsupported fault")
    seconds = 2 if smoke else 30
    recovery_limit = 20 if smoke else 300
    name = ("smoke-" if smoke else "") + "fault-" + fault
    out = experiment.out / name
    out.mkdir(exist_ok=False)
    profile = "default" if smoke else "tuned"
    result = {
        "status": "incomplete",
        "kind": "fault_smoke" if smoke else "fault",
        "fault": fault,
        "outage_seconds_minimum": seconds,
        "events": [],
        "probes": [],
        "pacing": "disabled for smoke"
        if smoke
        else "all=8/login=1/token=2 per rolling 60s",
        "profile": profile
        + "; fault diagnostics are separate from production-limit soak",
        "poll_resolution_seconds": 0.2 if smoke else 1,
        "recovery_definition": "three consecutive validated CC + active introspection flows",
        "no_hidden_retries": True,
    }
    lock = threading.Lock()
    stop = threading.Event()
    restoration = threading.Event()
    paced = Pacer(not smoke)
    recorder = Recorder(out / "attempts.jsonl.gz")
    observation_errors = []
    sample_count = []
    deadline_state = {"end_ns": None}
    observer_start_ns = None

    def record(stage, start, end, status, error):
        recorder.write(
            {
                "type": "http",
                "phase": "fault",
                "vu": 0,
                "stage": stage,
                "start_ns": start,
                "end_ns": end,
                "status": status,
                "error": error,
            }
        )

    client = Session(experiment.fixture["target"], timeout=3, record=record, pace=paced)
    user = User(client, experiment.fixture["accounts"][0], experiment.fixture)

    def event(kind):
        with lock:
            result["events"].append(
                {
                    "event": kind,
                    "monotonic_ns": time.perf_counter_ns(),
                    "wall_ns": time.time_ns(),
                }
            )

    def observations():
        with gzip.open(out / "telemetry.jsonl.gz", "xt") as stream:
            while not stop.is_set():
                row = {"monotonic_ns": time.perf_counter_ns()}
                try:
                    row["process"] = process_stats(experiment.fixture["server_pid"])
                except (OSError, ValueError):
                    row["process_unavailable"] = True
                try:
                    row["postgres"] = experiment.db.stats()
                except (OSError, ValueError, RuntimeError):
                    row["postgres_unavailable"] = True
                redis = Redis(
                    experiment.fixture["redis_port"],
                    experiment.fixture["redis_password"],
                    timeout=2,
                )
                try:
                    row["redis_ping"] = redis.command("PING") == "PONG"
                except (OSError, ValueError):
                    row["redis_ping"] = False
                finally:
                    redis.close()
                stream.write(json.dumps(row) + "\n")
                stream.flush()
                sample_count.append(row["monotonic_ns"])
                if stop.wait(5):
                    break

    def observe_safely():
        try:
            observations()
        except Exception as exc:  # noqa: BLE001 - a dead observer must invalidate the run
            observation_errors.append(type(exc).__name__)

    observer = threading.Thread(target=observe_safely, daemon=True)

    def watch_requests():
        while not stop.wait(0.05):
            client.interrupt(expired_only=True)
            deadline = deadline_state["end_ns"]
            reason = (
                "recovery_deadline"
                if deadline and time.perf_counter_ns() > deadline
                else None
            )
            if shutil.disk_usage(out).free < 192 * 1024**2:
                reason = "persistent_space_floor"
            if reason:
                observation_errors.append(reason)
                paced.cancel.set()
                client.interrupt()
                stop.set()

    def watchdog():
        try:
            watch_requests()
        except Exception as exc:  # noqa: BLE001 - resource/deadline observer failure invalidates evidence
            observation_errors.append("watchdog_" + type(exc).__name__)
            stop.set()
            paced.cancel.set()
            client.interrupt()

    watcher = threading.Thread(target=watchdog, daemon=True)
    error = []

    def controller():
        try:
            if stop.wait(0.5 if smoke else 30):
                return
            event("fault_command_start")
            if fault == "redis":
                redis_control(experiment, "pause")
            elif fault == "database":
                if not experiment.proxy_control("pause")["paused"]:
                    raise RuntimeError("proxy did not pause")
            else:
                experiment.server.stop()
            event("fault_active")
            if fault == "redis":
                redis = Redis(
                    experiment.fixture["redis_port"],
                    experiment.fixture["redis_password"],
                    timeout=2,
                )
                try:
                    redis.command("PING")
                except OSError:
                    result["boundary_proof"] = {
                        "owned_redis_unreachable": True,
                        "postgres_still_available": bool(experiment.db.stats()),
                    }
                else:
                    raise RuntimeError("owned Redis remained reachable during fault")
                finally:
                    redis.close()
            elif fault == "database":
                with socket.create_connection(
                    ("127.0.0.1", experiment.fixture["db_proxy_port"]), 2
                ) as stream:
                    stream.settimeout(2)
                    try:
                        closed = stream.recv(1) == b""
                    except ConnectionResetError:
                        closed = True
                if not closed:
                    raise RuntimeError("paused proxy accepted a usable stream")
                result["boundary_proof"] = {
                    "proxy_stream_closed": True,
                    "upstream_postgres_still_available": bool(experiment.db.stats()),
                }
            else:
                if experiment.server.process is not None:
                    raise RuntimeError("candidate did not stop")
                result["boundary_proof"] = {"owned_candidate_process_stopped": True}
            if stop.wait(seconds):
                return
            event("restore_command_start")
            if fault == "redis":
                redis_control(experiment, "resume")
            elif fault == "database":
                experiment.proxy_control("resume")
            else:
                experiment.server = Server(
                    experiment.fixture,
                    experiment.repo,
                    experiment.binary,
                    experiment.runtime / (name + "-restored"),
                )
                experiment.server.start(profile, pacer=paced)
                experiment.save_fixture()
            event("restore_command_complete")
            deadline_state["end_ns"] = time.perf_counter_ns() + int(
                recovery_limit * 1e9
            )
            restoration.set()
        except (OSError, ValueError, RuntimeError, ProtocolError) as exc:
            error.append(type(exc).__name__)
            restoration.set()

    control = threading.Thread(target=controller, daemon=True)
    watcher.start()
    try:
        experiment.ensure_resources()
        experiment.reset()
        experiment.start_server(profile, name)
        verify_candidate(experiment.fixture)
        if fault == "redis":
            redis_control(experiment, "status")
        # Real successful business baseline before the fault, without auth/token
        # fixtures generated outside the running backend.
        user.credentials()
        user.introspect()
        event("baseline_validated")
        observer_start_ns = time.perf_counter_ns()
        observer.start()
        control.start()
        stable = 0
        deadline = time.monotonic() + (1 if smoke else 30) + seconds + recovery_limit
        deadline_state["end_ns"] = time.perf_counter_ns() + int(
            ((1 if smoke else 30) + seconds + recovery_limit + 60) * 1e9
        )
        while time.monotonic() < deadline and not error and not stop.is_set():
            start = time.perf_counter_ns()
            completed_after_restore = restoration.is_set()
            category = ""
            try:
                user.credentials()
                user.introspect()
                ok = True
            except ProtocolError as exc:
                category = exc.kind
                ok = False
            end = time.perf_counter_ns()
            result["probes"].append(
                {
                    "start_ns": start,
                    "end_ns": end,
                    "ok": ok,
                    "error": category,
                    "started_after_restore": completed_after_restore,
                }
            )
            if completed_after_restore:
                if ok and stable == 0:
                    event("first_success_after_restore")
                stable = stable + 1 if ok else 0
                if stable == 3:
                    event("three_stable_successes")
                    break
            if stop.wait(0.2 if smoke else 1):
                break
        if stable != 3 or error or not result.get("boundary_proof"):
            raise RuntimeError("stable recovery not proven")
        result["status"] = "passed"
    finally:
        stop.set()
        paced.cancel.set()
        client.interrupt()
        if control.ident is not None:
            control.join(timeout=30)
        # Always restore the exact owned boundary, even on failed probes.
        try:
            if fault == "redis":
                redis_control(experiment, "resume")
            elif fault == "database":
                experiment.proxy_control("resume")
        except Exception as exc:  # noqa: BLE001 - restoration failure must be retained
            error.append("restoration_" + type(exc).__name__)
        if observer.ident is not None:
            observer.join(timeout=30)
        watcher.join(timeout=1)
        for operation in (client.close, recorder.close):
            try:
                operation()
            except Exception as exc:  # noqa: BLE001 - still attempt every cleanup and save result
                error.append("cleanup_" + type(exc).__name__)
        if experiment.server:
            try:
                experiment.server.stop()
            except Exception as exc:  # noqa: BLE001 - preserve the result on shutdown failure
                error.append("server_cleanup_" + type(exc).__name__)
        if len(result["probes"]) > 1:
            times = [p["start_ns"] for p in result["probes"]]
            result["observed_max_probe_start_gap_seconds"] = (
                max(b - a for a, b in pairwise(times)) / 1e9
            )
        result["control_errors"] = error
        result["observer_still_running"] = observer.is_alive()
        result["observation_errors"] = observation_errors
        result["sample_count"] = len(sample_count)
        if not smoke and observer_start_ns:
            finish = time.perf_counter_ns()
            required = int((finish - observer_start_ns) / 1e9 / 5 * 0.8)
            points = [observer_start_ns] + sample_count + [finish]
            largest_gap = max(b - a for a, b in pairwise(points)) / 1e9
            result["observer_required_samples"] = required
            result["observer_max_gap_seconds"] = largest_gap
            if len(sample_count) < required or largest_gap > 15:
                observation_errors.append("observer_coverage_incomplete")
        if (
            control.is_alive()
            or observer.is_alive()
            or error
            or observation_errors
            or not sample_count
        ):
            result["status"] = "incomplete"
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result
