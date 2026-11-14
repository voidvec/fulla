#!/usr/bin/env python3
"""Export every retained row to CSV without pooling rounds or removing tails."""

import argparse
import csv
import json
import math
from pathlib import Path

from run import SCENARIOS


def mapping(value):
    return value if isinstance(value, dict) else {}


def valid_steady_result(result, item, directory):
    if (
        not isinstance(result, dict)
        or result.get("status") != "passed"
        or result.get("kind") != "steady"
        or item.get("kind") != "steady"
    ):
        return False
    if (result.get("warmup_seconds"), result.get("measurement_seconds")) != (
        30,
        120,
    ) or result.get("incomplete_reasons"):
        return False
    if any(result.get(k) != item.get(k) for k in ("profile", "scenario", "vus")):
        return False
    start, stop = result.get("measurement_start_ns"), result.get("measurement_stop_ns")
    if (
        not isinstance(start, int)
        or not isinstance(stop, int)
        or stop - start != 120_000_000_000
    ):
        return False
    observer = mapping(result.get("observer"))
    if (
        observer.get("errors") != 0
        or type(observer.get("samples")) is not int
        or observer["samples"] < 24
    ):
        return False
    raw = directory / "attempts.jsonl.gz"
    telemetry = directory / "telemetry.jsonl.gz"
    if not all(p.is_file() and p.stat().st_size > 0 for p in (raw, telemetry)):
        return False
    measured = mapping(result.get("result"))
    if raw.stat().st_size != measured.get("raw_bytes_compressed"):
        return False
    for kind in ("http", "flow"):
        stats = mapping(measured.get(kind))
        count = stats.get("cohort_attempts")
        if (
            type(count) is not int
            or count <= 0
            or stats.get("cohort_admissions") != count
            or stats.get("cohort_success") != count
            or stats.get("cohort_failure") != 0
        ):
            return False
        for key in ("window_completion_qps", "cohort_wall_seconds"):
            number = stats.get(key)
            if (
                not isinstance(number, (int, float))
                or not math.isfinite(number)
                or number < 0
            ):
                return False
        if stats["cohort_wall_seconds"] < 120:
            return False
        quantiles = mapping(stats.get("latency_ns_all_attempts"))
        values = [quantiles.get(k) for k in ("p50", "p95", "p99", "max")]
        if not all(
            isinstance(v, (int, float)) and math.isfinite(v) and v >= 0 for v in values
        ) or values != sorted(values):
            return False
        if (
            stats.get("event_count_mismatch") is not False
            or stats.get("missing_completions") != 0
        ):
            return False
        if not all(
            k in stats
            for k in (
                "window_completion_qps",
                "cohort_wall_seconds",
                "latency_ns_all_attempts",
            )
        ):
            return False
    return True


def export(session_directory, output):
    session = json.loads((session_directory / "session.json").read_text())
    rows = []
    complete_artifacts = True
    for item in session["rows"]:
        path = session_directory / item.get("result_path", "missing-result")
        try:
            result = json.loads(path.read_text())
            if not isinstance(result, dict):
                raise TypeError("invalid result object")
            artifact_error = ""
        except (OSError, ValueError, TypeError):
            result = {}
            artifact_error = "missing_or_unreadable_result"
        steady_valid = valid_steady_result(result, item, path.parent)
        complete_artifacts = complete_artifacts and steady_valid
        row = {
            k: item.get(k)
            for k in ("id", "kind", "profile", "scenario", "vus", "round", "status")
        }
        row.update(
            declared_status=item.get("status"),
            steady_artifacts_valid=steady_valid,
            measurement_seconds=result.get("measurement_seconds"),
            warmup_seconds=result.get("warmup_seconds"),
            incomplete_reasons=";".join(result.get("incomplete_reasons", [])),
            exception_category=result.get("exception_category"),
        )
        if artifact_error:
            row.update(status="incomplete", incomplete_reasons=artifact_error)
        elif result.get("status") != item.get("status"):
            row.update(
                status="incomplete",
                incomplete_reasons="declared_and_actual_status_mismatch",
            )
        elif item.get("kind") == "steady" and not steady_valid:
            row.update(
                status="incomplete",
                incomplete_reasons="invalid_or_missing_steady_artifact",
            )
        for kind in ("http", "flow"):
            stats = mapping(mapping(result.get("result")).get(kind))
            for key in (
                "cohort_admissions",
                "cohort_attempts",
                "cohort_success",
                "cohort_failure",
                "window_completion_qps",
                "cohort_wall_seconds",
            ):
                row[kind + "_" + key] = stats.get(key)
            for percentile in ("p50", "p95", "p99", "max"):
                value = mapping(stats.get("latency_ns_all_attempts")).get(percentile)
                row[kind + "_all_attempt_" + percentile + "_ms"] = (
                    value / 1e6
                    if isinstance(value, (int, float)) and math.isfinite(value)
                    else None
                )
        rows.append(row)
    with output.open("x", newline="") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=list(rows[0]) if rows else ["status"]
        )
        writer.writeheader()
        writer.writerows(rows)
    expected = {
        (p, s, v, r)
        for p in ("default", "tuned")
        for s in SCENARIOS
        for v in (1, 4)
        for r in (1, 2, 3)
    }
    actual = {(r["profile"], r["scenario"], r["vus"], r["round"]) for r in rows}
    complete = (
        session.get("mode") == "matrix"
        and session.get("status") == "passed"
        and len(rows) == 108
        and actual == expected
        and complete_artifacts
        and all(r["status"] == "passed" for r in rows)
    )
    return {
        "rows_exported": len(rows),
        "complete_steady_matrix": complete,
        "whole_S07_complete": False,
        "reason": "soak/fault evidence and independent acceptance are separate; no automatic capacity claim",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(export(args.session, args.output)))


if __name__ == "__main__":
    main()
