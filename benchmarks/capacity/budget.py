#!/usr/bin/env python3
"""Forecast the complete S07 evidence footprint from a same-environment pilot.

This conservative admission gate is a forecast, never a hard WAL limit. Runtime
free-space checks remain necessary even after admission.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path

from gate import SCOPES, environment_signature
from run import SCENARIOS
from support import source_digest

MIB = 1024 * 1024


def forecast(
    pilot,
    *,
    current_free,
    current_digest,
    nonce_hash,
    binary_hash,
    boot_id,
    environment_hash,
):
    root = Path(pilot)
    session = json.loads((root / "session.json").read_text())
    if session.get("status") != "passed" or session.get("mode") != "pilot":
        raise ValueError("a completed, successful pilot session is required")
    expected = {(p, s) for p in ("default", "tuned") for s in SCENARIOS}
    actual = [(r["profile"], r["scenario"]) for r in session["rows"]]
    if set(actual) != expected or len(actual) != len(expected):
        raise ValueError("pilot must contain exactly the 18 profile/scenario rows")
    if (
        session["driver_sha256"],
        session["binary_sha256"],
        session["fixture_nonce_sha256"],
        session["environment"]["boot_id"],
    ) != (current_digest, binary_hash, nonce_hash, boot_id):
        raise ValueError("pilot source/build/fixture/boot provenance mismatch")
    if (
        environment_hash is None
        or session.get("environment_sha256") != environment_hash
    ):
        raise ValueError("pilot hardware/PostgreSQL configuration changed")
    raw_matrix = peak_database = peak_wal_rate = max_attempts = 0
    details = []
    for row in session["rows"]:
        result_path = root / row["result_path"]
        result = json.loads(result_path.read_text())
        seconds = result["measurement_seconds"] + result["warmup_seconds"]
        if (
            row["status"] != "passed"
            or result["status"] != "passed"
            or result["measurement_seconds"] < 5
            or row["vus"] != 4
        ):
            raise ValueError(
                "all pilot rows must pass at 4 VU for at least five seconds"
            )
        # Count every result, raw gzip and telemetry byte, including setup and drain.
        size = sum(
            p.stat().st_size for p in result_path.parent.rglob("*") if p.is_file()
        )
        # Six formal rows per pilot arm: VU 1/4 times three rounds. Do not assume
        # VU=1 is smaller. 2x extrapolation includes setup, observer and variance.
        formal_row = int(size * max(1, 150 / seconds) * 2) + MIB
        raw_matrix += formal_row * 6
        before, after = row["postgres_before"], row["postgres_after_shutdown"]
        db_growth = max(0, after["database_bytes"] - before["database_bytes"])
        peak_database = max(peak_database, int(db_growth * 150 / seconds * 2))
        wal_rate = max(0, int(after["wal_bytes"]) - int(before["wal_bytes"])) / seconds
        peak_wal_rate = max(peak_wal_rate, wal_rate)
        predicted_attempts = (
            int(
                result["result"]["http"]["cohort_attempts"]
                / result["measurement_seconds"]
                * 150
                * 2
            )
            + 10000
        )
        max_attempts = max(max_attempts, predicted_attempts)
        details.append(
            {
                "profile": row["profile"],
                "scenario": row["scenario"],
                "pilot_bytes": size,
                "forecast_one_formal_row_bytes": formal_row,
                "pilot_wal_bytes_per_second": wal_rate,
            }
        )
    last = session["rows"][-1]["postgres_after_shutdown"]
    # PostgreSQL can exceed max_wal_size. Include 3x the soft threshold AND two
    # minutes at twice the worst pilot rate; use the larger, never a fake cap.
    wal_headroom = int(max(3 * last["max_wal_size_bytes"], peak_wal_rate * 120 * 2))
    peak_database = max(32 * MIB, peak_database)
    # Paced 2h soak: at most 8 HTTP/min, conservative <=16KiB per HTTP/flow pair,
    # plus 5s resource observations and setup. Fault probes have their own budget.
    other_raw = 64 * MIB
    raw_total = raw_matrix + other_raw
    reserve = 192 * MIB
    runtime_headroom = wal_headroom + peak_database + 16 * MIB
    required = reserve + runtime_headroom + 2 * raw_total  # retained raw + archive copy
    reasons = []
    if current_free < required:
        reasons.append("insufficient_shared_persistent_space")
    if max_attempts > 2000000:
        reasons.append("attempt_budget_must_be_increased_before_new_pilot")
    return {
        "schema": 1,
        "status": "blocked" if reasons else "passed",
        "reasons": reasons,
        "pilot_session_sha256": hashlib.sha256(
            (root / "session.json").read_bytes()
        ).hexdigest(),
        "driver_sha256": current_digest,
        "binary_sha256": binary_hash,
        "fixture_nonce_sha256": nonce_hash,
        "boot_id": boot_id,
        "vus": [1, 4],
        "profiles": ["default", "tuned", "soak"],
        "allowed_kinds": ["steady", "soak", "fault"],
        "max_measure_seconds": 7200,
        "scopes": SCOPES,
        "environment_sha256": environment_hash,
        "required_free_bytes": required,
        "per_row_required_free_bytes": reserve + runtime_headroom,
        "current_free_bytes": current_free,
        "additional_free_bytes_needed": max(0, required - current_free),
        "forecast": {
            "formal_rows": 108,
            "matrix_raw_bytes": raw_matrix,
            "soak_fault_raw_bytes": other_raw,
            "retained_raw_and_archive_bytes": 2 * raw_total,
            "database_peak_growth_bytes": peak_database,
            "wal_headroom_bytes": wal_headroom,
            "reserve_bytes": reserve,
            "maximum_forecast_http_attempts": max_attempts,
        },
        "caveats": [
            "max_wal_size is a soft trigger, not a disk cap",
            "same overlay remains a shared failure domain",
            "pilot extrapolation with 2x margin is not a guarantee; runtime floor remains mandatory",
            "all formal tails, failed rows and full compressed raw must be retained",
        ],
        "pilot_rows": details,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--pilot", type=Path)
    mode.add_argument("--preflight", action="store_true")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    fixture = json.loads(args.fixture.read_text())
    if args.preflight:
        from fixture import Database

        stats = Database(fixture).stats()
        free = shutil.disk_usage(args.output.parent).free
        # This is an admission-policy floor, not an estimate of actual WAL peak.
        # Matrix raw remains unknown until a successful same-environment pilot.
        components = {
            "reserve_bytes": 192 * MIB,
            "wal_headroom_bytes": 3 * stats["max_wal_size_bytes"],
            "database_growth_headroom_bytes": 32 * MIB,
            "runtime_headroom_bytes": 16 * MIB,
            "soak_fault_raw_and_archive_headroom_bytes": 128 * MIB,
        }
        required = sum(components.values())
        report = {
            "schema": 1,
            "kind": "resource_preflight",
            "status": "blocked",
            "reasons": ["successful_18_row_pilot_required"]
            + (["insufficient_policy_headroom"] if free < required else []),
            "current_free_bytes": free,
            "policy_floor_excluding_matrix_raw_bytes": required,
            "additional_free_bytes_before_matrix_raw": max(0, required - free),
            "components": components,
            "matrix_raw_bytes": None,
            "caveat": "policy floor, not a measured peak or formal authorization",
            "environment_sha256": environment_signature(fixture),
            "driver_sha256": source_digest(),
        }
        with args.output.open("x") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
        print(json.dumps(report))
        return 2
    report = forecast(
        args.pilot,
        current_free=shutil.disk_usage(args.pilot).free,
        current_digest=source_digest(),
        nonce_hash=hashlib.sha256(fixture["owner_nonce"].encode()).hexdigest(),
        binary_hash=fixture["build"]["binary_sha256"],
        environment_hash=environment_signature(fixture),
        boot_id=Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
    )
    with args.output.open("x") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(
        json.dumps(
            {
                k: report[k]
                for k in (
                    "status",
                    "required_free_bytes",
                    "current_free_bytes",
                    "additional_free_bytes_needed",
                )
            }
        )
    )
    return 0 if report["status"] == "passed" else 2


if __name__ == "__main__":
    raise SystemExit(main())
