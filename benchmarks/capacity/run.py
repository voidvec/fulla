#!/usr/bin/env python3
"""Run one bounded, isolated protocol workload and retain all raw attempts."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import resource
import threading
import time
import uuid
from pathlib import Path

from fixture import Database, verify_candidate
from observe import Sampler
from protocol import ProtocolError, Session, User, json_body
from support import Pacer, Recorder, Redis, source_digest, summarize

SCENARIOS = (
    "discovery",
    "password_consent",
    "client_credentials",
    "introspect_reuse",
    "introspect_cold",
    "refresh",
    "userinfo",
    "org_context",
    "mixed",
)
MIX = (
    "password_consent",
    "org_context",
    "client_credentials",
    "refresh",
    "userinfo",
    "userinfo",
    "userinfo",
    "introspect_reuse",
    "introspect_reuse",
    "introspect_cold",
    "discovery",
    "discovery",
)


class PhaseStop(Exception):
    pass


def cpu_quota_cores(*, required, quota_path=Path("/sys/fs/cgroup/cpu.max")):
    """Functional smoke can run without quota telemetry; measurements cannot."""
    try:
        quota = quota_path.read_text().split()
    except FileNotFoundError:
        if required:
            raise
        return None
    if len(quota) != 2:
        raise ValueError("invalid cgroup CPU quota")
    cores = int(quota[0]) / int(quota[1]) if quota[0] != "max" else os.cpu_count()
    if cores is None or cores <= 0:
        raise ValueError("invalid cgroup CPU budget")
    return cores


class Run:
    def __init__(self, args):
        self.args = args
        self.fixture = json.loads(args.fixture.read_text())
        args.target = args.target or self.fixture["target"]
        if args.target.rstrip("/") != self.fixture["target"].rstrip("/"):
            raise ValueError("workload/observer fixture target mismatch")
        if self.fixture.get("owner") != "fulla-capacity-s07" or not self.fixture.get(
            "owner_nonce"
        ):
            raise ValueError("not an owned S07 fixture")
        if args.vus > len(self.fixture["accounts"]):
            raise ValueError("each VU needs a distinct seeded account")
        if not 1 <= args.vus <= 16:
            raise ValueError("supported VU range: 1..16")
        if args.kind == "steady" and (args.warmup < 30 or args.duration < 120):
            raise ValueError("formal rows require >=30s warmup and >=120s measurement")
        if args.kind in ("steady", "soak", "fault"):
            if not all(
                self.fixture.get(k)
                for k in ("db", "server_pid", "server_start_ticks", "server_executable")
            ) or not all(a.get("expected_sub") for a in self.fixture["accounts"]):
                raise ValueError(
                    "formal modes require a verifiable candidate and seeded subjects"
                )
            if args.reserve_mb < 192 or args.timeout > 10:
                raise ValueError("formal reserve/deadline bounds may not be weakened")
        if args.kind in ("steady", "soak", "fault"):
            if not args.budget_plan:
                raise ValueError(
                    "formal measurement requires a passed pilot budget plan"
                )
            gate = json.loads(args.budget_plan.read_text())
            from gate import environment_signature, validate_scope

            validate_scope(
                gate,
                self.fixture,
                kind=args.kind,
                profile=self.fixture["profile"],
                vus=args.vus,
                warmup=args.warmup,
                duration=args.duration,
                drain=args.drain,
                environment_hash=environment_signature(self.fixture),
            )
            self.budget_plan_sha256 = hashlib.sha256(
                args.budget_plan.read_bytes()
            ).hexdigest()
            import shutil

            if (
                shutil.disk_usage(args.output.parent).free
                < gate["per_row_required_free_bytes"]
            ):
                raise ValueError(
                    "shared persistent space does not meet the measured pilot budget"
                )
        if args.kind == "soak" and (args.duration < 7200 or not args.production_pacing):
            raise ValueError("soak requires >=7200s and production pacing")
        if args.kind == "soak":
            from support import load_config

            config = load_config(
                Path(self.fixture["runtime_directory"]) / "config.json"
            )
            prod = load_config(
                Path(__file__).resolve().parents[2]
                / "apps/server/config/config.prod.json"
            )
            policy = next(
                p["config"]
                for p in prod["plugins"]
                if p["name"] == "drogon::plugin::Hodor"
            )
            actual = [
                p["config"]
                for p in config["plugins"]
                if p["name"] == "drogon::plugin::Hodor"
            ]
            proof = self.fixture.get("hodor_proof", {})
            digest = hashlib.sha256(
                json.dumps(policy, sort_keys=True).encode()
            ).hexdigest()
            if (
                self.fixture["profile"] != "soak"
                or actual != [policy]
                or proof.get("policy_sha256") != digest
                or proof.get("observed_source_ip") != "127.0.0.1"
                or not proof.get("rejected")
            ):
                raise ValueError(
                    "soak requires the exact production Hodor policy and a real bounded 429 proof"
                )
        if self.fixture.get("db"):
            from urllib.parse import urlsplit

            parsed = urlsplit(args.target)
            if (parsed.port or (443 if parsed.scheme == "https" else 80)) != int(
                self.fixture["port"]
            ):
                raise ValueError("fixture listener port does not match target")
        self.cpu_budget_cores = cpu_quota_cores(required=args.kind != "smoke")
        self.out = args.output.resolve()
        self.out.mkdir(parents=True, exist_ok=False)
        self.writer = Recorder(self.out / "attempts.jsonl.gz")
        self.pacer = Pacer(args.production_pacing)
        if args.production_pacing:
            now = time.monotonic()
            for key, values in self.fixture.get("startup_pacing", {}).items():
                if key in self.pacer.queues:
                    self.pacer.queues[key].extend(
                        float(v) for v in values if 0 <= now - float(v) < 60
                    )
        self.lock = threading.Lock()
        self.attempts = 0
        self.flow_admissions = 0
        self.cancel = threading.Event()
        self.incomplete = []
        self.watchdog_stop = threading.Event()
        self.watchdog = threading.Thread(
            target=self.watch_sockets, name="s07-deadlines", daemon=True
        )
        self.users = []
        self.vu_state = []
        self.phase_counts = {}
        self.phase = "setup"
        self.setup_deadline_ns = time.perf_counter_ns() + 900_000_000_000
        self.phase_stop_ns = 0
        self.drain_ns = 0
        self.telemetry = None
        if self.fixture.get("server_pid") and self.fixture.get("db"):
            self.telemetry = Sampler(
                self.fixture, self.out / "telemetry.jsonl.gz", pacer=self.pacer
            )
        import shutil

        self.minimum_free_bytes = shutil.disk_usage(self.out).free
        self.meta = {
            "schema": 1,
            "run_id": uuid.uuid4().hex,
            "kind": args.kind,
            "scenario": args.scenario,
            "vus": args.vus,
            "seed": args.seed,
            "warmup_seconds": args.warmup,
            "measurement_seconds": args.duration,
            "fixture_sha256": hashlib.sha256(args.fixture.read_bytes()).hexdigest(),
            "driver_sha256": source_digest(),
            "http_success_definition": "expected HTTP status and step checks; flow result is authoritative for full business completion",
            "profile": self.fixture["profile"],
            "build": self.fixture.get("build", {}),
            "cache_enabled": self.fixture["cache_enabled"],
            "production_pacing": args.production_pacing,
            "driver_topology": "colocated; shared cgroup budget; capacity lower bound",
            "latency_scope": "client end-to-end; closed-loop; no coordinated-omission correction",
            "quotas_60s": {"all_including_observer": 8, "login": 1, "token": 2}
            if args.production_pacing
            else None,
            "mixed_slots": list(MIX),
            "status": "incomplete",
            "max_attempts": args.max_attempts,
        }
        if hasattr(self, "budget_plan_sha256"):
            self.meta["budget_plan_sha256"] = self.budget_plan_sha256
        for index in range(args.vus):
            state = {
                "first": None,
                "first_http": None,
                "flow": 0,
                "phase": "setup",
                "control_ns": 0,
            }
            self.vu_state.append(state)

            def before_send(index=index, state=state):
                now = time.perf_counter_ns()
                if self.cancel.is_set():
                    raise PhaseStop()
                if state["phase"] in ("warmup", "measure"):
                    if now >= self.drain_ns:
                        raise ProtocolError("drain_deadline", "driver")
                    if state["first_http"] is None:
                        state["first_http"] = now
                with self.lock:
                    if self.attempts >= args.max_attempts:
                        self.incomplete.append("attempt_budget_exhausted")
                        self.cancel.set()
                        raise PhaseStop()
                    self.attempts += 1

            def record(stage, start, end, status, error, index=index, state=state):
                self.writer.write(
                    {
                        "type": "http",
                        "phase": state["phase"],
                        "vu": index,
                        "stage": stage,
                        "start_ns": start,
                        "end_ns": end,
                        "status": status,
                        "error": error,
                        "flow": state["flow"],
                    }
                )

            def record_start(stage, start, index=index, state=state):
                self.writer.write(
                    {
                        "type": "http_admit",
                        "phase": state["phase"],
                        "vu": index,
                        "stage": stage,
                        "start_ns": start,
                        "end_ns": start,
                        "flow": state["flow"],
                    }
                )

            session = Session(args.target, args.timeout, record, self.pacer)
            session.record_start = record_start
            session.before_send = before_send
            user = User(session, self.fixture["accounts"][index], self.fixture)
            self.users.append(user)

    def watch_sockets(self):
        while not self.watchdog_stop.wait(0.05):
            if (
                self.phase == "setup"
                and time.perf_counter_ns() > self.setup_deadline_ns
            ):
                self.incomplete.append("setup_deadline")
                self.cancel.set()
                self.pacer.cancel.set()
            for user in self.users:
                user.session.interrupt(expired_only=True)
            if self.telemetry:
                for session in (self.telemetry.session, self.telemetry.control_session):
                    if session:
                        session.interrupt(expired_only=True)

    def metrics(self):
        session = Session(self.args.target, self.args.timeout, pace=self.pacer)
        try:
            raw = session.request("metrics", "GET", "/metrics")["body"].decode()
        finally:
            session.close()
        result = {}
        import re

        for line in raw.splitlines():
            if line.startswith("fulla_cache_total{") and 'repo="token"' in line:
                match = re.search(r'outcome="(hit|miss|error)"', line)
                if match:
                    result[match[1]] = result.get(match[1], 0) + float(
                        line.rsplit(" ", 1)[1]
                    )
        return result

    def perform(self, user, scenario, redis, state):
        control_ns = 0
        recovery = (
            scenario == "refresh" and (user.refresh_uncertain or not user.refresh)
        ) or (scenario == "userinfo" and user.expires <= time.monotonic() + 30)
        if recovery:
            user.session.stage_prefix = "recovery."
            try:
                if user.refresh and not user.refresh_uncertain:
                    user.rotate()
                else:
                    user.accept_pair(user.authorize_consent())
                user.userinfo(expected_subject=user.subject)
            finally:
                user.session.stage_prefix = ""
        if scenario == "discovery":

            def validate(response):
                value = json_body(response, "discovery")
                if not value.get("issuer") or not value.get("jwks_uri"):
                    raise ProtocolError(
                        "discovery_shape", "discovery", response["status"]
                    )

            user.session.request(
                "discovery", "GET", "/.well-known/openid-configuration", check=validate
            )
        elif scenario == "password_consent":
            user.accept_pair(user.authorize_consent())
            user.userinfo(expected_subject=user.subject)
        elif scenario == "client_credentials":
            user.credentials()
        elif scenario in ("introspect_reuse", "introspect_cold"):
            if scenario == "introspect_cold" and self.fixture["cache_enabled"]:
                before = time.perf_counter_ns()
                try:
                    redis.evict_token(user.cc_access, self.fixture["owner_nonce"])
                finally:
                    state["control_ns"] += time.perf_counter_ns() - before
                control_ns = state["control_ns"]
            user.introspect()
        elif scenario == "refresh":
            user.rotate()
        elif scenario == "userinfo":
            user.userinfo(expected_subject=user.subject)
        elif scenario == "org_context":
            if not user.account.get("org"):
                raise ProtocolError("organization_fixture_missing", "org")
            pair = user.authorize_consent(
                user.account["org_client"], user.account["org"]
            )
            user.org_access = pair["access_token"]
            user.userinfo(user.org_access, user.subject, user.account["org_id"])
            user.introspect(
                user.org_access, user.account["org_client"], user.account["org_id"]
            )
        else:
            raise ValueError("unknown scenario")
        return control_ns

    def phase_run(self, phase, duration):
        self.phase = phase
        for state in self.vu_state:
            state.update(phase=phase, first=None, first_http=None, control_ns=0)
        ready = threading.Barrier(self.args.vus + 1)
        worker_errors = []

        def work(index):
            user, state = self.users[index], self.vu_state[index]
            redis = Redis(
                self.fixture["redis_port"],
                self.fixture.get("redis_password", ""),
                self.args.timeout,
            )
            rng = random.Random(self.args.seed + index)
            slots = []
            ready.wait(timeout=10)
            try:
                while (
                    time.perf_counter_ns() < self.phase_stop_ns
                    and not self.cancel.is_set()
                ):
                    state["first"] = time.perf_counter_ns()
                    if state["first"] >= self.phase_stop_ns:
                        break
                    with self.lock:
                        if self.flow_admissions >= self.args.max_attempts:
                            self.incomplete.append("flow_budget_exhausted")
                            self.cancel.set()
                            break
                        self.flow_admissions += 1
                    state["first_http"] = None
                    state["control_ns"] = 0
                    state["flow"] += 1
                    scenario = self.args.scenario
                    if scenario == "mixed":
                        if not slots:
                            slots = list(MIX)
                            rng.shuffle(slots)
                        scenario = slots.pop()
                    self.writer.write(
                        {
                            "type": "flow_admit",
                            "phase": phase,
                            "vu": index,
                            "scenario": scenario,
                            "start_ns": state["first"],
                            "end_ns": state["first"],
                            "flow": state["flow"],
                        }
                    )
                    ok, error = False, ""
                    try:
                        self.perform(user, scenario, redis, state)
                        ok = True
                    except PhaseStop:
                        error = "driver_cancelled"
                    except ProtocolError as exc:
                        error = exc.kind
                    except (OSError, ValueError, TimeoutError):
                        error = "control_or_driver_error"
                    end = time.perf_counter_ns()
                    with self.lock:
                        counts = self.phase_counts.setdefault(
                            phase, {"attempts": 0, "failures": 0}
                        )
                        counts["attempts"] += 1
                        counts["failures"] += not ok
                    if state["first"] is not None:
                        self.writer.write(
                            {
                                "type": "flow",
                                "phase": phase,
                                "vu": index,
                                "scenario": scenario,
                                "start_ns": state["first"],
                                "end_ns": end,
                                "ok": ok,
                                "error": error,
                                "flow": state["flow"],
                                "control_ns": state["control_ns"],
                                "first_http_start_ns": state["first_http"],
                            }
                        )
            except Exception as exc:  # noqa: BLE001 - preserve worker/cleanup failures in evidence
                worker_errors.append(type(exc).__name__)
            finally:
                redis.close()

        threads = [
            threading.Thread(
                target=work, args=(i,), daemon=True, name="s07-vu-" + str(i)
            )
            for i in range(self.args.vus)
        ]
        for thread in threads:
            thread.start()
        self.phase_start_ns = time.perf_counter_ns()
        self.phase_stop_ns = self.phase_start_ns + int(duration * 1e9)
        self.drain_ns = self.phase_stop_ns + int(self.args.drain * 1e9)
        ready.wait(timeout=10)
        # No build/agent work during measurement; this monitor is part of the driver.
        import shutil

        while any(t.is_alive() for t in threads):
            available = shutil.disk_usage(self.out).free
            self.minimum_free_bytes = min(self.minimum_free_bytes, available)
            if available < self.args.reserve_mb * 1024 * 1024:
                self.incomplete.append("persistent_space_floor")
                self.cancel.set()
                self.pacer.cancel.set()
            if time.perf_counter_ns() >= self.drain_ns:
                self.incomplete.append("drain_deadline")
                self.cancel.set()
                self.pacer.cancel.set()
                for user in self.users:
                    user.session.interrupt()
            if time.perf_counter_ns() > self.drain_ns + int(
                (self.args.timeout + 2) * 1e9
            ):
                self.incomplete.append("unfinished_workers")
                break
            time.sleep(0.05)
        for thread in threads:
            thread.join(timeout=0)
        if worker_errors:
            self.incomplete.append("worker_exception")
            raise RuntimeError("worker failed")
        return self.phase_start_ns, self.phase_stop_ns

    def execute(self):
        cpu_before = resource.getrusage(resource.RUSAGE_SELF)
        started = time.monotonic()
        try:
            self.watchdog.start()
            if self.fixture.get("db"):
                self.meta["backend_preflight"] = verify_candidate(
                    self.fixture, pacer=self.pacer
                )
            # Do not call this inside a formal time window.
            for user in self.users:
                user.prepare()
            if self.telemetry:
                self.telemetry.start()
            if (
                self.args.scenario in ("introspect_reuse", "introspect_cold")
                and self.fixture["cache_enabled"]
            ):
                self.meta["cache_metrics_before_warmup"] = self.metrics()
            self.phase_run("warmup", self.args.warmup)
            self.meta["warmup_flows"] = self.phase_counts.get("warmup", {})
            if self.phase_counts.get("warmup", {}).get("failures"):
                self.incomplete.append("warmup_failed_flows")
            if self.incomplete:
                raise RuntimeError("warmup incomplete")
            if (
                self.args.scenario in ("introspect_reuse", "introspect_cold")
                and self.fixture["cache_enabled"]
            ):
                self.meta["cache_metrics_before"] = self.metrics()
            used_before = resource.getrusage(resource.RUSAGE_SELF)
            child_before = resource.getrusage(resource.RUSAGE_CHILDREN)
            start, stop = self.phase_run("measure", self.args.duration)
            used_after = resource.getrusage(resource.RUSAGE_SELF)
            child_after = resource.getrusage(resource.RUSAGE_CHILDREN)
            window_wall = (time.perf_counter_ns() - start) / 1e9
            cpu = (
                used_after.ru_utime
                + used_after.ru_stime
                - used_before.ru_utime
                - used_before.ru_stime
            )
            cores = self.cpu_budget_cores
            self.meta["measurement_driver"] = {
                "cpu_seconds_with_drain": cpu,
                "wall_seconds_with_drain": window_wall,
                "cpu_core_fraction": cpu / window_wall,
                "cgroup_budget_cores": cores,
                "cpu_quota_available": cores is not None,
                "budget_fraction": cpu / window_wall / cores
                if cores is not None
                else None,
                "driver_limited": cpu / window_wall >= 0.8,
                "always_capacity_lower_bound": True,
                "observer_child_cpu_seconds": child_after.ru_utime
                + child_after.ru_stime
                - child_before.ru_utime
                - child_before.ru_stime,
            }
            self.meta.update(measurement_start_ns=start, measurement_stop_ns=stop)
            if (
                self.args.scenario in ("introspect_reuse", "introspect_cold")
                and self.fixture["cache_enabled"]
            ):
                after = self.metrics()
                before = self.meta["cache_metrics_before"]
                self.meta["cache_metric_delta"] = {
                    k: after.get(k, 0) - before.get(k, 0)
                    for k in ("hit", "miss", "error")
                }
            self.phase = "postflight"
            for state in self.vu_state:
                state["phase"] = "postflight"
            for user in self.users:
                user.session.stage_prefix = "postflight."
                user.introspect()
            if self.fixture.get("db"):
                now = time.monotonic()
                hashes = [
                    h
                    for user in self.users
                    for h, expires in user.cc_issued
                    if expires > now + 30
                ]
                self.meta["cc_persistence_audit"] = Database(
                    self.fixture
                ).validate_issued_credentials(hashes)
            self.writer.close()
            self.meta["result"] = summarize(self.out / "attempts.jsonl.gz", start, stop)
            result = self.meta["result"]
            if (
                result["http"]["event_count_mismatch"]
                or result["flow"]["event_count_mismatch"]
            ):
                self.incomplete.append("missing_cohort_completions")
            if (
                self.args.scenario == "mixed"
                and self.args.kind in ("steady", "soak")
                and set(result.get("scenarios", {})) != set(MIX)
            ):
                self.incomplete.append("mixed_scenario_coverage_missing")
            if (
                not result["http"]["cohort_attempts"]
                or not result["flow"]["cohort_attempts"]
            ):
                self.incomplete.append("no_measured_work")
            if (
                self.args.scenario in ("introspect_reuse", "introspect_cold")
                and self.fixture["cache_enabled"]
            ):
                delta = self.meta["cache_metric_delta"]
                total = delta["hit"] + delta["miss"]
                wanted = "hit" if self.args.scenario == "introspect_reuse" else "miss"
                ratio = delta[wanted] / total if total else 0
                self.meta["cache_observed_ratio"] = ratio
                if ratio < 0.95 or delta["error"]:
                    self.incomplete.append("cache_path_not_proven")
            if not self.incomplete:
                self.meta["status"] = (
                    "completed_with_failures"
                    if result["flow"]["cohort_failure"]
                    else "passed"
                )
        except Exception as exc:
            # Exception type only: third-party messages can contain credentials.
            self.meta["exception_type"] = type(exc).__name__
            self.meta["exception_category"] = (
                exc.kind if isinstance(exc, ProtocolError) else "driver_or_setup"
            )
            raise
        finally:
            if self.telemetry and self.telemetry.thread.ident is not None:
                try:
                    self.telemetry.stop()
                except Exception as exc:  # noqa: BLE001 - preserve worker/cleanup failures in evidence
                    self.incomplete.append("observer_shutdown_" + type(exc).__name__)
                self.meta["observer"] = {
                    "samples": self.telemetry.samples,
                    "errors": self.telemetry.errors,
                    "failure_type": self.telemetry.failure_type,
                    "interval_seconds": 5,
                }
                if (
                    self.telemetry.errors
                    or not self.telemetry.samples
                    or self.telemetry.thread.is_alive()
                    or self.telemetry.samples
                    < int((self.args.duration + self.args.warmup) / 5 * 0.8)
                ):
                    self.incomplete.append("observer_incomplete")
            self.watchdog_stop.set()
            if self.watchdog.ident is not None:
                self.watchdog.join(timeout=1)
            if self.incomplete:
                self.meta["status"] = "incomplete"
            try:
                self.writer.close()
            except Exception as exc:  # noqa: BLE001 - keep the result even when raw close fails
                self.incomplete.append("raw_close_" + type(exc).__name__)
            for user in self.users:
                try:
                    user.session.close()
                except Exception as exc:  # noqa: BLE001 - one close must not lose the whole result
                    self.incomplete.append("session_close_" + type(exc).__name__)
            if self.incomplete:
                self.meta["status"] = "incomplete"
            used = resource.getrusage(resource.RUSAGE_SELF)
            wall = time.monotonic() - started
            self.meta["driver"] = {
                "process_cpu_seconds": used.ru_utime
                + used.ru_stime
                - cpu_before.ru_utime
                - cpu_before.ru_stime,
                "whole_run_wall_seconds": wall,
                "limited_warning": "shared host and Python driver; no dedicated-driver capacity claim",
            }
            self.meta["minimum_persistent_free_bytes"] = self.minimum_free_bytes
            self.meta["incomplete_reasons"] = sorted(set(self.incomplete))
            (self.out / "result.json").write_text(
                json.dumps(self.meta, indent=2) + "\n"
            )
        return self.meta


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--budget-plan", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scenario", choices=SCENARIOS, required=True)
    parser.add_argument(
        "--kind",
        choices=("smoke", "pilot", "steady", "soak"),
        default="steady",
    )
    parser.add_argument("--vus", type=int, default=4)
    parser.add_argument("--warmup", type=float, default=30)
    parser.add_argument("--duration", type=float, default=120)
    parser.add_argument("--timeout", type=float, default=5)
    parser.add_argument("--drain", type=float, default=60)
    parser.add_argument("--max-attempts", type=int, default=2000000)
    parser.add_argument("--reserve-mb", type=int, default=192)
    parser.add_argument("--production-pacing", action="store_true")
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()
    if (
        args.duration <= 0
        or args.warmup < 0
        or args.timeout <= 0
        or args.drain <= 0
        or args.max_attempts <= 0
    ):
        parser.error("durations/budgets must be positive")
    result = Run(args).execute()
    print(json.dumps({"status": result["status"], "output": str(args.output)}))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
