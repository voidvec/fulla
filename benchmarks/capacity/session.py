#!/usr/bin/env python3
"""Run an owned local fixture serially; formal modes require the measured budget gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

from fixture import Database, Server, create_organizations
from gate import SCOPES, environment_signature, fixture_lock, validate_scope
from protocol import Session
from run import SCENARIOS
from support import Redis, atomic_private_json, source_digest


class Experiment:
    def __init__(self, args, *, leased=False):
        if not leased:
            raise ValueError(
                "reset-capable sessions require the exclusive fixture lease"
            )
        self.args = args
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        if shutil.disk_usage(args.evidence.parent).free < 320 * 1024**2:
            raise ValueError(
                "initialization needs the 192MiB floor plus 128MiB startup headroom"
            )
        self.repo = Path(__file__).resolve().parents[2]
        self.fixture = json.loads(args.fixture.read_text())
        if self.fixture.get("owner") != "fulla-capacity-s07":
            raise ValueError("not an S07 owned fixture")
        self.db = Database(self.fixture)
        self.db.claim()
        self.binary = args.binary.resolve()
        if not self.binary.is_file():
            raise ValueError("build a real Release server first")
        self.binary_hash = hashlib.sha256(self.binary.read_bytes()).hexdigest()
        if self.fixture.get("build", {}).get("binary_sha256") != self.binary_hash:
            raise ValueError("fixture build provenance does not match executable")
        self.out = args.evidence.resolve()
        self.out.mkdir(parents=True, exist_ok=False)
        (self.out / "rows").mkdir()
        (self.out / "logs").mkdir()
        self.runtime = args.runtime.resolve() / ("session-" + uuid.uuid4().hex)
        self.runtime.mkdir(parents=True, mode=0o700, exist_ok=False)
        self.proxy = None
        self.proxy_log = None
        self.server = None
        self.rows = []
        self.current_child = None
        self.monitor_stop = threading.Event()
        self.space_violation = threading.Event()
        self.resource_monitor_error = None
        self.minimum_free_bytes = shutil.disk_usage(self.out).free
        self.monitor = threading.Thread(
            target=self.monitor_resources, daemon=True, name="s07-session-resources"
        )
        self.manifest = {
            "schema": 1,
            "session_id": self.runtime.name,
            "mode": args.mode,
            "status": "incomplete",
            "driver_sha256": source_digest(),
            "binary_sha256": self.binary_hash,
            "product_commit": self.fixture["build"]["git_commit"],
            "fixture_nonce_sha256": hashlib.sha256(
                self.fixture["owner_nonce"].encode()
            ).hexdigest(),
            "environment": {
                "uname": list(os.uname()),
                "cpu_max": Path("/sys/fs/cgroup/cpu.max").read_text().strip(),
                "memory_max": Path("/sys/fs/cgroup/memory.max").read_text().strip(),
                "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
                "persistent_free_bytes": shutil.disk_usage(self.out).free,
                "driver_placement": "same Linux node/cgroup; no production capacity ceiling claim",
            },
        }
        self.save_manifest()

    def monitor_resources(self):
        try:
            self.watch_resources()
        except Exception as exc:  # noqa: BLE001 - a dead resource monitor invalidates the session
            self.resource_monitor_error = type(exc).__name__
            self.space_violation.set()
            child = self.current_child
            if child:
                try:
                    child.send_signal(signal.SIGINT)
                except OSError:
                    pass
            server = self.server
            if server and server.process:
                try:
                    server.process.terminate()
                except OSError:
                    pass

    def ensure_resources(self):
        if (
            self.space_violation.is_set()
            or shutil.disk_usage(self.out).free < 208 * 1024**2
        ):
            raise ValueError(
                "shared persistent space cannot support the next bounded operation"
            )

    def watch_resources(self):
        while not self.monitor_stop.wait(0.05):
            free = shutil.disk_usage(self.out).free
            self.minimum_free_bytes = min(self.minimum_free_bytes, free)
            if (
                free < 192 * 1024**2
                or shutil.disk_usage(self.runtime).free < 64 * 1024**2
            ):
                self.space_violation.set()
                # Only interrupt processes created by this session. The driver
                # handles SIGINT through finally and preserves its partial data.
                child = self.current_child
                if child and child.poll() is None:
                    child.send_signal(signal.SIGINT)
                server = self.server
                if server and server.process and server.process.poll() is None:
                    server.process.terminate()
                return

    def save_manifest(self):
        self.manifest["rows"] = self.rows
        (self.out / "session.json").write_text(
            json.dumps(self.manifest, indent=2) + "\n"
        )

    def save_fixture(self):
        atomic_private_json(self.args.fixture, self.fixture)

    def start_proxy(self):
        redis = Redis(self.fixture["redis_port"], self.fixture["redis_password"])
        try:
            owner = redis.command("GET", "fulla:s07:owner")
            if owner is None and redis.command("DBSIZE") == 0:
                redis.command("SET", "fulla:s07:owner", self.fixture["owner_nonce"])
            elif owner != self.fixture["owner_nonce"]:
                raise ValueError("refuse to adopt a nonempty/unowned Redis instance")
        finally:
            redis.close()
        control = self.runtime / "proxy.sock"
        self.proxy_log = (self.runtime / "proxy-private.log").open("xb")
        self.proxy = subprocess.Popen(
            [
                sys.executable,
                str(Path(__file__).with_name("proxy.py")),
                "--port",
                str(self.fixture["db_proxy_port"]),
                "--upstream-port",
                str(self.fixture["db"]["port"]),
                "--control-socket",
                str(control),
            ],
            stdout=self.proxy_log,
            stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 10
        while not control.exists():
            if self.proxy.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("owned proxy did not start")
            time.sleep(0.05)
        self.fixture["proxy_control_socket"] = str(control)
        self.fixture["proxy_pid"] = self.proxy.pid
        self.save_fixture()

    def proxy_control(self, action):
        if action not in ("pause", "resume", "status"):
            raise ValueError("unsupported proxy action")
        with socket.socket(socket.AF_UNIX) as stream:
            stream.settimeout(3)
            stream.connect(self.fixture["proxy_control_socket"])
            stream.sendall(action.encode() + b"\n")
            return json.loads(stream.recv(1024))

    def initialize(self):
        self.ensure_resources()
        self.start_proxy()
        self.db.guard = self.ensure_resources
        server = Server(
            self.fixture, self.repo, self.binary, self.runtime / "initialize"
        )
        self.server = server
        try:
            server.migrate()
            self.ensure_resources()
            self.db.seed(self.repo)
            self.ensure_resources()
            server.start("default")
            self.save_fixture()
            create_organizations(
                self.fixture, save=self.save_fixture, guard=self.ensure_resources
            )
        finally:
            server.stop()
        self.save_fixture()
        self.manifest["backend_preflight"] = self.db.stats()
        self.save_manifest()

    def reset(self):
        self.ensure_resources()
        if self.server and self.server.process:
            raise ValueError("reset requires the candidate to be stopped")
        self.db.verify()
        # All rows are in the leased, disposable database. Keep account/org/client fixture data.
        self.db.sql(
            "TRUNCATE oauth2_refresh_tokens,oauth2_access_tokens,oauth2_codes,oauth2_user_consents,organization_consents,audit_logs;"
        )

    def start_server(self, profile, row):
        self.ensure_resources()
        self.server = Server(self.fixture, self.repo, self.binary, self.runtime / row)
        config = self.server.start(profile)
        self.save_fixture()

        def sanitize(value, key=""):
            if key.lower() in ("password", "passwd", "secret", "client_secret"):
                return "<redacted>"
            if isinstance(value, dict):
                return {k: sanitize(v, k) for k, v in value.items()}
            if isinstance(value, list):
                return [sanitize(v, key) for v in value]
            return value

        (self.out / "logs" / (row + "-config.json")).write_text(
            json.dumps(sanitize(config), indent=2) + "\n"
        )
        return config

    def run_row(self, profile, scenario, vus, round_number, kind, warmup, duration):
        row = (
            f"{len(self.rows) + 1:03}-{profile}-{scenario}-v{vus}-r{round_number}-"
            + uuid.uuid4().hex[:8]
        )
        self.reset()
        before = self.db.stats()
        free_before = shutil.disk_usage(self.out).free
        self.start_server(profile, row)
        item = {
            "id": row,
            "profile": profile,
            "scenario": scenario,
            "vus": vus,
            "round": round_number,
            "kind": kind,
            "status": "running",
            "postgres_before": before,
            "persistent_free_before": free_before,
        }
        self.rows.append(item)
        self.save_manifest()
        print(json.dumps({"event": "row_start", "id": row, "kind": kind}), flush=True)
        command = [
            sys.executable,
            str(Path(__file__).with_name("run.py")),
            "--fixture",
            str(self.args.fixture.resolve()),
            "--output",
            str(self.out / "rows" / row),
            "--scenario",
            scenario,
            "--vus",
            str(vus),
            "--kind",
            kind,
            "--warmup",
            str(warmup),
            "--duration",
            str(duration),
            "--max-attempts",
            str(self.args.max_attempts),
        ]
        if kind in ("steady", "soak"):
            command += ["--budget-plan", str(self.args.budget_plan.resolve())]
        if profile == "soak":
            command += ["--production-pacing", "--drain", "300"]
        try:
            with (self.out / "logs" / (row + ".log")).open("xb") as log:
                self.current_child = subprocess.Popen(
                    command,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
                try:
                    code = self.current_child.wait(timeout=duration + warmup + 2000)
                except subprocess.TimeoutExpired:
                    self.current_child.send_signal(signal.SIGINT)
                    try:
                        self.current_child.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        self.current_child.kill()
                        self.current_child.wait(timeout=5)
                    raise
                finally:
                    self.current_child = None
            result_path = self.out / "rows" / row / "result.json"
            result = (
                json.loads(result_path.read_text())
                if result_path.exists()
                else {"status": "missing_result"}
            )
            item.update(
                status=result["status"],
                exit_code=code,
                result_path=str(result_path.relative_to(self.out)),
                postgres_after=self.db.stats(),
                persistent_free_after=shutil.disk_usage(self.out).free,
            )
            self.save_manifest()
            print(
                json.dumps({"event": "row_end", "id": row, "status": item["status"]}),
                flush=True,
            )
            if code or result["status"] != "passed":
                raise RuntimeError("row failed; evidence retained and session stopped")
        finally:
            self.server.stop()
            item["shutdown"] = self.server.shutdown_events
            item["postgres_after_shutdown"] = self.db.stats()
            self.save_manifest()
        return item

    def verify_hodor(self):
        config = self.start_server("soak", "hodor-proof")
        limiter = next(
            p["config"]
            for p in config["plugins"]
            if p["name"] == "drogon::plugin::Hodor"
        )
        statuses = []
        peer = None
        client = Session(self.fixture["target"])
        try:
            for _ in range(64):
                response = client.request(
                    "bounded_limiter_probe",
                    "GET",
                    "/.well-known/openid-configuration",
                    expected=(200, 429),
                )
                statuses.append(response["status"])
                if client.connection and client.connection.sock:
                    peer = client.connection.sock.getsockname()[0]
        finally:
            client.close()
            self.server.stop()
        proof = {
            "requests": len(statuses),
            "rejected": statuses.count(429),
            "observed_source_ip": peer,
            "policy_sha256": hashlib.sha256(
                json.dumps(limiter, sort_keys=True).encode()
            ).hexdigest(),
            "statuses": statuses,
            "separate_from_throughput": True,
        }
        (self.out / "hodor-proof.json").write_text(json.dumps(proof, indent=2) + "\n")
        if peer != "127.0.0.1" or proof["rejected"] == 0:
            raise ValueError(
                "production limiter was not exercised on untrusted loopback"
            )
        self.fixture["hodor_proof"] = proof
        self.save_fixture()

    def execute(self):
        try:
            self.monitor.start()
            if self.args.mode in ("matrix", "soak", "faults"):
                gate = json.loads(self.args.budget_plan.read_text())
                kind = {"matrix": "steady", "soak": "soak", "faults": "fault"}[
                    self.args.mode
                ]
                scope = SCOPES[kind]
                environment_hash = environment_signature(self.fixture)
                for profile in scope["profiles"]:
                    for vus in scope["vus"]:
                        validate_scope(
                            gate,
                            self.fixture,
                            kind=kind,
                            profile=profile,
                            vus=vus,
                            warmup=scope.get("warmup"),
                            duration=scope.get("duration"),
                            drain=scope.get("drain"),
                            environment_hash=environment_hash,
                        )
                if shutil.disk_usage(self.out).free < gate["required_free_bytes"]:
                    raise ValueError(
                        "not enough real persistent space for the complete evidence and peak"
                    )
            self.initialize()
            self.manifest["environment_sha256"] = environment_signature(self.fixture)
            self.save_manifest()
            if self.args.mode == "smoke":
                for profile in ("default", "tuned"):
                    for scenario in SCENARIOS:
                        self.run_row(
                            profile,
                            scenario,
                            4,
                            1,
                            "smoke",
                            0.2,
                            self.args.smoke_seconds,
                        )
            elif self.args.mode == "pilot":
                for profile in ("default", "tuned"):
                    for scenario in SCENARIOS:
                        self.run_row(
                            profile, scenario, 4, 1, "pilot", 2, self.args.pilot_seconds
                        )
            elif self.args.mode == "matrix":
                for round_number in (1, 2, 3):
                    order = (
                        ("default", "tuned")
                        if round_number % 2
                        else ("tuned", "default")
                    )
                    for scenario in SCENARIOS:
                        for vus in (1, 4):
                            for profile in order:
                                self.run_row(
                                    profile,
                                    scenario,
                                    vus,
                                    round_number,
                                    "steady",
                                    30,
                                    120,
                                )
                if len(self.rows) != 108:
                    raise RuntimeError("formal matrix row count mismatch")
            elif self.args.mode == "limiter-smoke":
                self.verify_hodor()
            elif self.args.mode == "soak":
                self.verify_hodor()
                self.run_row("soak", "mixed", 4, 1, "soak", 30, 7200)
            elif self.args.mode in ("faults", "fault-smoke"):
                from faults import run_fault

                for fault in ("redis", "database", "restart"):
                    item = run_fault(self, fault, smoke=self.args.mode == "fault-smoke")
                    self.manifest.setdefault("faults", []).append(item)
                    if item["status"] != "passed":
                        raise RuntimeError("fault recovery was not proven")
            self.ensure_resources()
            self.manifest["status"] = "passed"
        except Exception as exc:
            self.manifest["error_type"] = type(exc).__name__
            raise
        finally:
            if self.server:
                try:
                    self.server.stop()
                except Exception:  # noqa: BLE001 - preserve worker/cleanup failures in evidence
                    self.manifest["cleanup_error"] = "server_shutdown"
                    self.manifest["status"] = "incomplete"
            try:
                if self.proxy:
                    self.proxy.terminate()
                    try:
                        self.proxy.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        self.proxy.kill()
                        self.proxy.wait(timeout=3)
                if self.proxy_log:
                    self.proxy_log.close()
            except Exception as exc:  # noqa: BLE001 - preserve cleanup failures
                self.manifest["proxy_cleanup_error"] = type(exc).__name__
                self.manifest["status"] = "incomplete"
            self.monitor_stop.set()
            if self.monitor.ident is not None:
                self.monitor.join(timeout=2)
            self.manifest["minimum_persistent_free_bytes"] = self.minimum_free_bytes
            if self.space_violation.is_set() or self.monitor.is_alive():
                self.manifest["status"] = "incomplete"
                self.manifest["resource_gate_failure"] = True
            if self.resource_monitor_error:
                self.manifest["resource_monitor_error"] = self.resource_monitor_error
            self.save_manifest()
        return self.manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument(
        "--mode",
        choices=(
            "smoke",
            "pilot",
            "matrix",
            "soak",
            "limiter-smoke",
            "faults",
            "fault-smoke",
        ),
        required=True,
    )
    parser.add_argument("--budget-plan", type=Path)
    parser.add_argument("--redis-control-socket", type=Path)
    parser.add_argument("--max-attempts", type=int, default=2000000)
    parser.add_argument("--smoke-seconds", type=float, default=1)
    parser.add_argument("--pilot-seconds", type=float, default=10)
    args = parser.parse_args()
    if args.mode in ("matrix", "soak", "faults") and not args.budget_plan:
        parser.error("formal modes require --budget-plan")
    if args.mode in ("faults", "fault-smoke") and not args.redis_control_socket:
        parser.error("fault modes require an explicit owned Redis control socket")
    if args.max_attempts <= 0 or args.pilot_seconds < 5 or args.smoke_seconds <= 0:
        parser.error("invalid duration/budget")
    with fixture_lock(args.fixture):
        result = Experiment(args, leased=True).execute()
    print(json.dumps({"status": result["status"], "evidence": str(args.evidence)}))


if __name__ == "__main__":
    main()
