"""Verify retained measurement bytes, replayed statistics and row identity."""

import gzip
import hashlib
import json
import math
import zlib
from pathlib import Path

from support import summarize

RAW = "attempts.jsonl.gz"
TELEMETRY = "telemetry.jsonl.gz"


def inspect_artifact(path):
    try:
        return _inspect_artifact(path)
    except (OSError, EOFError, UnicodeError, ValueError, TypeError, zlib.error):
        raise ValueError("invalid or unreadable gzip JSONL artifact") from None


def _inspect_artifact(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    records = resources = 0
    with gzip.open(path, "rt", encoding="utf-8") as stream:
        for line in stream:
            row = json.loads(line)
            if not isinstance(row, dict) or not isinstance(row.get("type"), str):
                raise ValueError("artifact contains a malformed event")
            records += 1
            resources += row["type"] == "resources"
    if not records:
        raise ValueError("empty measurement artifact")
    return {"sha256": digest.hexdigest(), "bytes": path.stat().st_size,
            "records": records, "resource_samples": resources}


def seal_artifacts(directory, *, telemetry):
    names = (RAW, TELEMETRY) if telemetry else (RAW,)
    for name in names:
        path = directory / name
        if path.is_symlink() or path.resolve().parent != directory.resolve():
            raise ValueError("artifact escapes its measurement directory")
    return {name: inspect_artifact(directory / name) for name in names}


def result_path(root, relative):
    if not isinstance(relative, str) or Path(relative).is_absolute():
        raise ValueError("result path must be relative to its session")
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()) or path.name != "result.json":
        raise ValueError("result path escapes its session")
    return path


def validate_artifacts(result, item, directory, provenance):
    """Raise on missing evidence; callers retain failed rows but never admit them."""
    for key, expected in (("row_id", item.get("id")), ("round", item.get("round")),
                          ("kind", item.get("kind")), ("profile", item.get("profile")),
                          ("scenario", item.get("scenario")), ("vus", item.get("vus")),
                          ("run_id", item.get("run_id"))):
        if result.get(key) != expected or expected is None:
            raise ValueError("measurement identity mismatch: " + key)
    if (not isinstance(result["run_id"], str) or not result["run_id"]
            or not isinstance(result["row_id"], str) or not result["row_id"]
            or type(result["round"]) is not int or result["round"] < 1
            or type(result["vus"]) is not int or result["vus"] not in (1, 4)):
        raise ValueError("invalid measurement identity")
    if result.get("telemetry_status") != "collected":
        raise ValueError("measurement requires collected telemetry")
    build = result.get("build")
    if not isinstance(build, dict):
        raise ValueError("missing build provenance")
    observed = {"driver_sha256": result.get("driver_sha256"),
                "binary_sha256": build.get("binary_sha256"),
                "fixture_nonce_sha256": result.get("fixture_nonce_sha256")}
    if any(not isinstance(value, str) or not value or observed.get(key) != value
           for key, value in provenance.items()) or set(provenance) != set(observed):
        raise ValueError("row source/build/fixture provenance mismatch")
    recorded = result.get("artifacts")
    if not isinstance(recorded, dict):
        raise ValueError("missing artifact seals")
    actual = seal_artifacts(directory, telemetry=True)
    if any(recorded.get(name) != actual[name] for name in (RAW, TELEMETRY)):
        raise ValueError("artifact digest or event count mismatch")
    observer = result.get("observer", {})
    samples = actual[TELEMETRY]["resource_samples"]
    if (type(observer.get("samples")) is not int or samples < 1
            or samples != observer["samples"] or observer.get("errors") != 0):
        raise ValueError("telemetry sample count mismatch")
    start, stop = result.get("measurement_start_ns"), result.get("measurement_stop_ns")
    if type(start) is not int or type(stop) is not int or stop <= start:
        raise ValueError("invalid measurement interval")
    seconds = result.get("measurement_seconds")
    warmup = result.get("warmup_seconds")
    if (type(seconds) not in (int, float) or not math.isfinite(seconds) or seconds <= 0
            or type(warmup) not in (int, float) or not math.isfinite(warmup) or warmup < 0
            or stop - start != int(seconds * 1e9)):
        raise ValueError("measurement interval does not match duration")
    if samples < max(1, int((seconds + warmup) / 5 * 0.8)):
        raise ValueError("insufficient observer coverage")
    try:
        replayed = summarize(directory / RAW, start, stop)
    except (OSError, EOFError, UnicodeError, ValueError, TypeError, KeyError, zlib.error):
        raise ValueError("cannot replay raw measurement") from None
    if replayed != result.get("result"):
        raise ValueError("raw measurement statistics mismatch")
