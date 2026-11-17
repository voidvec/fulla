"""Shared formal-run scope, environment provenance and exclusive fixture lease."""

import fcntl
import hashlib
import json
import os
from contextlib import contextmanager
from pathlib import Path

from fixture import Database
from support import source_digest

SCOPES = {
    "steady": {
        "profiles": ["default", "tuned"],
        "vus": [1, 4],
        "warmup": 30,
        "duration": 120,
        "drain": 60,
    },
    "soak": {
        "profiles": ["soak"],
        "vus": [4],
        "warmup": 30,
        "duration": 7200,
        "drain": 300,
    },
    "fault": {
        "profiles": ["tuned"],
        "vus": [1],
        "baseline": 30,
        "outage": 30,
        "recovery": 300,
    },
}


@contextmanager
def fixture_lock(path):
    """All reset-capable sessions must use the same canonical private lease file."""
    lock_path = path.resolve().with_suffix(".session.lock")
    with lock_path.open("a") as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError(
                "another session owns this fixture; refuse reset/start"
            ) from None
        try:
            yield
        finally:
            fcntl.flock(stream, fcntl.LOCK_UN)


def environment_signature(fixture):
    stats = Database(fixture).stats()
    keys = (
        "pg_version",
        "fsync",
        "synchronous_commit",
        "full_page_writes",
        "checkpoint_timeout_seconds",
        "checkpoint_completion_target",
        "min_wal_size_bytes",
        "max_wal_size_bytes",
        "archive_mode",
        "replication_slots",
        "shared_buffers_bytes",
        "max_connections",
    )
    value = {
        "configuration_sources": {
            name: hashlib.sha256(
                (
                    Path(__file__).resolve().parents[2] / "apps/server/config" / name
                ).read_bytes()
            ).hexdigest()
            for name in ("config.json", "config.prod.json")
        },
        "uname": list(os.uname()),
        "cpu_max": Path("/sys/fs/cgroup/cpu.max").read_text().strip(),
        "memory_max": Path("/sys/fs/cgroup/memory.max").read_text().strip(),
        "postgres": {k: stats[k] for k in keys},
        "db_port": fixture["db"]["port"],
        "redis_port": fixture["redis_port"],
        "target": fixture["target"],
    }
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def validate_scope(
    gate,
    fixture,
    *,
    kind,
    profile,
    vus,
    warmup=None,
    duration=None,
    drain=None,
    environment_hash,
):
    """Reject oversized windows and cross-profile reuse before sending credentials."""
    if kind not in SCOPES or gate.get("scopes", {}).get(kind) != SCOPES[kind]:
        raise ValueError("budget scope does not cover the fixed S07 protocol")
    if (
        gate.get("status") != "passed"
        or gate.get("driver_sha256") != source_digest()
        or gate.get("fixture_nonce_sha256")
        != hashlib.sha256(fixture["owner_nonce"].encode()).hexdigest()
        or gate.get("binary_sha256") != fixture["build"]["binary_sha256"]
        or gate.get("boot_id")
        != Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        or gate.get("environment_sha256") != environment_hash
    ):
        raise ValueError("pilot provenance/environment gate failed")
    scope = SCOPES[kind]
    if profile not in scope["profiles"] or vus not in scope["vus"]:
        raise ValueError("budget does not cover this profile/concurrency")
    if kind != "fault" and (warmup, duration, drain) != (
        scope["warmup"],
        scope["duration"],
        scope["drain"],
    ):
        raise ValueError(
            "formal window/drain must exactly match the measured budget scope"
        )
    if kind == "fault" and (warmup, duration, drain) != (None, None, None):
        raise ValueError("fault timing is fixed by the fault controller scope")
