"""Small synthetic, independently identified measurements for tool tests only."""
import hashlib
import gzip
import json
import uuid



def write_measurement(directory, item, *, seconds=120, warmup=30, samples=24):
    directory.mkdir(parents=True, exist_ok=True)
    start = 2_000_000_000
    stop = start + int(seconds * 1e9)
    with gzip.open(directory / "attempts.jsonl.gz", "wt") as stream:
        for kind in ("http", "flow"):
            common = {"phase": "measure", "start_ns": start + 1000, "vu": 0}
            stream.write(json.dumps({**common, "type": kind + "_admit"}) + "\n")
            stream.write(json.dumps({**common, "type": kind, "end_ns": start + 2000,
                                     "status": 200, "error": "", "ok": True,
                                     "scenario": item["scenario"]}) + "\n")
    with gzip.open(directory / "telemetry.jsonl.gz", "wt") as stream:
        for i in range(samples):
            stream.write(json.dumps({"type": "resources", "monotonic_ns": start + i * 5_000_000_000}) + "\n")
    item["run_id"] = uuid.uuid4().hex
    result = {"status": "passed", "kind": item["kind"], "profile": item["profile"],
              "scenario": item["scenario"], "vus": item["vus"], "row_id": item["id"],
              "round": item["round"], "run_id": item["run_id"],
              "warmup_seconds": warmup, "measurement_seconds": seconds,
              "measurement_start_ns": start, "measurement_stop_ns": stop,
              "telemetry_status": "collected", "observer": {"samples": samples, "errors": 0},
              "driver_sha256": "driver", "build": {"binary_sha256": "binary"},
              "fixture_nonce_sha256": "nonce"}
    stats = {"cohort_admissions": 1, "event_count_mismatch": False,
             "missing_completions": 0, "cohort_attempts": 1, "cohort_success": 1,
             "cohort_failure": 0, "window_completed_success": 1,
             "window_completion_qps": 1 / seconds, "cohort_wall_seconds": seconds,
             "cohort_success_per_wall_second": 1 / seconds,
             "latency_ns_all_attempts": dict.fromkeys(("p50", "p95", "p99", "max"), 1000),
             "errors": {}, "drain_only_events": 0}
    result["result"] = {"http": {**stats, "statuses": {"200": 1}},
                        "flow": {**stats, "statuses": {}},
                        "scenarios": {item["scenario"]: {"attempts": 1, "success": 1,
                                                        "failure": 0, "control_ns": 0}},
                        "raw_events": 4,
                        "raw_bytes_compressed": (directory / "attempts.jsonl.gz").stat().st_size}
    result["artifacts"] = {}
    for name, records, resources in [("attempts.jsonl.gz", 4, 0), ("telemetry.jsonl.gz", samples, samples)]:
        data = (directory / name).read_bytes()
        result["artifacts"][name] = {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
                                     "records": records, "resource_samples": resources}
    (directory / "result.json").write_text(json.dumps(result))
    return result
