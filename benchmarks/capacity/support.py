"""Bounded recording, local RESP control and conservative production pacing."""

from __future__ import annotations

import gzip
import hashlib
import json
import socket
import threading
import time
from collections import deque
from pathlib import Path
from urllib.parse import urlsplit


class Redis:
    def __init__(self, port: int, password: str = "", timeout=3):
        self.port, self.password, self.timeout = port, password, timeout
        self.sock = None
        self.stream = None

    def close(self):
        if self.stream:
            self.stream.close()
        if self.sock:
            self.sock.close()
        self.stream = self.sock = None

    def command(self, *args):
        if not self.sock:
            self.sock = socket.create_connection(("127.0.0.1", self.port), self.timeout)
            self.stream = self.sock.makefile("rb")
            if self.password:
                self.command("AUTH", self.password)
        encoded = [str(x).encode() for x in args]
        body = b"*" + str(len(encoded)).encode() + b"\r\n"
        body += b"".join(
            b"$" + str(len(x)).encode() + b"\r\n" + x + b"\r\n" for x in encoded
        )
        try:
            self.sock.sendall(body)
            return self._read()
        except (OSError, ValueError):
            self.close()
            raise

    def _read(self):
        line = self.stream.readline()
        if not line:
            raise OSError("Redis disconnected")
        kind, value = line[:1], line[1:-2]
        if kind == b"+":
            return value.decode()
        if kind == b"-":
            raise OSError("Redis command rejected")
        if kind == b":":
            return int(value)
        if kind == b"$":
            size = int(value)
            if size < 0:
                return None
            data = self.stream.read(size + 2)
            if len(data) != size + 2:
                raise OSError("Redis truncated response")
            return data[:-2].decode()
        if kind == b"*":
            return [self._read() for _ in range(int(value))]
        raise ValueError("unsupported Redis response")

    def evict_token(self, raw: str, owner: str):
        if self.command("GET", "fulla:s07:owner") != owner:
            raise ValueError("Redis fixture ownership mismatch")
        digest = hashlib.sha256(raw.encode()).hexdigest().upper()
        return self.command(
            "DEL",
            "fulla:cache:token:access:" + digest,
            "fulla:cache:token:introspect:" + digest,
        )


class Pacer:
    """Rolling quotas include every request, including observer HTTP traffic."""

    def __init__(self, enabled=False, clock=time.monotonic, sleep=time.sleep):
        self.enabled, self.clock, self.sleep = enabled, clock, sleep
        self.lock = threading.Lock()
        self.queues = {"all": deque(), "login": deque(), "token": deque()}
        self.limits = {"all": 8, "login": 1, "token": 2}
        self.cancel = threading.Event()

    def __call__(self, path):
        return self.wait(path)

    def wait(self, path, cancel_event=None):
        if not self.enabled:
            return
        clean = urlsplit(path).path
        keys = ["all"]
        if any(
            clean.startswith(p)
            for p in (
                "/oauth2/login",
                "/oauth2/password/change",
                "/oauth2/mfa/verify",
                "/oauth2/authorize-context",
            )
        ):
            keys.append("login")
        if clean.startswith("/oauth2/token"):
            keys.append("token")
        while not self.cancel.is_set() and not (cancel_event and cancel_event.is_set()):
            with self.lock:
                now = self.clock()
                wait = 0.0
                for key in keys:
                    q = self.queues[key]
                    while q and q[0] <= now - 60:
                        q.popleft()
                    if len(q) >= self.limits[key]:
                        wait = max(wait, q[0] + 60 - now + 0.01)
                if wait <= 0:
                    for key in keys:
                        self.queues[key].append(now)
                    return
            self.sleep(min(wait, 0.1))
        raise TimeoutError("pacing cancelled")


class Recorder:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        # A raw gzip member per run; no overwrite and no credential fields.
        self.stream = path.open("xb")
        self.file = gzip.GzipFile(
            fileobj=self.stream, mode="wb", compresslevel=3, mtime=0
        )
        self.lock = threading.Lock()
        self.rows = 0

    def write(self, row: dict):
        allowed = {
            "type",
            "phase",
            "vu",
            "stage",
            "start_ns",
            "end_ns",
            "status",
            "error",
            "scenario",
            "ok",
            "flow",
            "control_ns",
            "first_http_start_ns",
        }
        if set(row) - allowed:
            raise ValueError("unsupported raw event field")
        data = (json.dumps(row, separators=(",", ":"), sort_keys=True) + "\n").encode()
        with self.lock:
            self.file.write(data)
            self.rows += 1

    def close(self):
        with self.lock:
            self.file.close()
            self.stream.close()


def quantile(values, percentile):
    if not values:
        return None
    values = sorted(values)
    # Nearest-rank; preserve all tails, failures and timeouts.
    import math

    return values[max(0, math.ceil(len(values) * percentile) - 1)]


def summarize(raw: Path, start_ns: int, stop_ns: int) -> dict:
    import collections

    stats = {
        kind: {
            "count": 0,
            "success": 0,
            "completed": 0,
            "end": stop_ns,
            "latencies": [],
            "errors": collections.Counter(),
            "statuses": collections.Counter(),
            "drain": 0,
        }
        for kind in ("http", "flow")
    }
    rows = 0
    scenarios = {}
    admissions = {"http": 0, "flow": 0}
    with gzip.open(raw, "rt") as stream:
        for line in stream:
            r = json.loads(line)
            rows += 1
            if r["phase"] != "measure":
                continue
            if r["type"] in ("http_admit", "flow_admit"):
                if start_ns <= r["start_ns"] < stop_ns:
                    admissions[r["type"].removesuffix("_admit")] += 1
                continue
            if r["type"] not in stats:
                continue
            kind, state = r["type"], stats[r["type"]]
            if r["start_ns"] >= stop_ns:
                state["drain"] += 1
                continue
            if r["start_ns"] < start_ns:
                continue
            ok = bool(r["ok"]) if kind == "flow" else not r["error"]
            if kind == "flow":
                scenario = scenarios.setdefault(
                    r["scenario"],
                    {"attempts": 0, "success": 0, "failure": 0, "control_ns": 0},
                )
                scenario["attempts"] += 1
                scenario["success"] += ok
                scenario["failure"] += not ok
                scenario["control_ns"] += r.get("control_ns", 0)
            state["count"] += 1
            state["success"] += ok
            state["completed"] += ok and r["end_ns"] < stop_ns
            state["end"] = max(state["end"], r["end_ns"])
            state["latencies"].append(r["end_ns"] - r["start_ns"])
            if r["error"]:
                state["errors"][r["error"]] += 1
            if kind == "http":
                state["statuses"][str(r["status"])] += 1
    result = {}
    for kind, state in stats.items():
        wall = (state["end"] - start_ns) / 1e9
        times = state["latencies"]
        result[kind] = {
            "cohort_admissions": admissions[kind],
            "event_count_mismatch": admissions[kind] != state["count"],
            "missing_completions": max(0, admissions[kind] - state["count"]),
            "cohort_attempts": state["count"],
            "cohort_success": state["success"],
            "cohort_failure": state["count"] - state["success"],
            "window_completed_success": state["completed"],
            "window_completion_qps": state["completed"] / ((stop_ns - start_ns) / 1e9),
            "cohort_wall_seconds": wall,
            "cohort_success_per_wall_second": state["success"] / wall,
            "latency_ns_all_attempts": {
                "p50": quantile(times, 0.50),
                "p95": quantile(times, 0.95),
                "p99": quantile(times, 0.99),
                "max": max(times, default=None),
            },
            "errors": dict(state["errors"]),
            "statuses": dict(state["statuses"]),
            "drain_only_events": state["drain"],
        }
    result["scenarios"] = scenarios
    result["raw_events"] = rows
    result["raw_bytes_compressed"] = raw.stat().st_size
    return result


def load_config(path: Path):
    """Read Drogon's commented JSON without treating http:// inside strings as a comment."""
    text = path.read_text()
    out, index, quoted, escaped = [], 0, False, False
    while index < len(text):
        char = text[index]
        if quoted:
            out.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
            index += 1
        elif char == '"':
            quoted = True
            out.append(char)
            index += 1
        elif text.startswith("//", index):
            end = text.find("\n", index)
            if end < 0:
                end = len(text)
            out.append(" " * (end - index))
            index = end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end < 0:
                raise ValueError("unterminated config comment")
            out.extend("\n" if c == "\n" else " " for c in text[index : end + 2])
            index = end + 2
        else:
            out.append(char)
            index += 1
    return json.loads("".join(out))


def source_digest():
    root = Path(__file__).resolve().parent
    digest = hashlib.sha256()
    for path in sorted(root.iterdir()):
        if path.suffix in (".py", ".json"):
            digest.update(path.name.encode() + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


def atomic_private_json(path: Path, value: dict):
    """Persist runtime credentials without exposing a permissive temporary file."""
    import os
    import secrets

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp-" + secrets.token_hex(6))
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()
