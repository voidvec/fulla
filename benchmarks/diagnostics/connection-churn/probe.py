#!/usr/bin/env python3
"""Bounded loopback-only HTTP churn probe; records every attempt without retries."""

import argparse
import concurrent.futures
import datetime
import http.client
import json
import math
from pathlib import Path
import platform
import time


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def percentile(values, quantile):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * quantile) - 1)]


def worker(worker_id, args, deadline, start):
    connection = None
    records = []
    next_start = start
    for sequence in range(args.max_requests_per_worker):
        now = time.monotonic()
        if now >= deadline:
            break
        if now < next_start:
            time.sleep(min(next_start - now, max(0, deadline - now)))
        if time.monotonic() >= deadline:
            break
        began = time.monotonic()
        record = {
            "worker": worker_id,
            "sequence": sequence,
            "utc": utc_now(),
            "elapsed_s": began - start,
            "status": None,
            "error": None,
            "stage": "connect",
            "new_connection": False,
        }
        request_deadline = min(deadline, began + args.timeout)

        def remaining():
            budget = request_deadline - time.monotonic()
            if budget <= 0:
                raise TimeoutError("request deadline reached")
            return budget

        try:
            if connection is None:
                connection = http.client.HTTPConnection("127.0.0.1", args.port)
            connection.timeout = remaining()
            if connection.sock is None:
                record["new_connection"] = True
                connection.connect()
            connection.sock.settimeout(remaining())
            record["stage"] = "send"
            connection.request("GET", "/ping", headers={
                "Connection": "close" if args.mode == "fresh" else "keep-alive",
            })
            connection.sock.settimeout(remaining())
            record["stage"] = "response_headers"
            response = connection.getresponse()
            record["status"] = response.status
            record["handled_counter"] = response.getheader("X-Diagnostic-Handled")
            record["stage"] = "response_body"
            # The fixture returns four bytes. Bound reads and reject unexpected
            # data; read1 avoids an unbounded slow-trickle body read loop.
            body = bytearray()
            while True:
                if connection.sock is not None:
                    connection.sock.settimeout(remaining())
                else:
                    remaining()
                chunk = response.read1(1025 - len(body))
                if not chunk:
                    break
                body.extend(chunk)
                if len(body) > 1024:
                    raise ValueError("response body exceeds 1024 bytes")
            if response.status != 200 or body != b"pong":
                raise ValueError("unexpected status or body")
            record["stage"] = "complete"
            if args.mode == "fresh" or response.will_close:
                connection.close()
                connection = None
        except (OSError, http.client.HTTPException, ValueError) as exc:
            record["error"] = type(exc).__name__
            if connection is not None:
                connection.close()
            connection = None
        record["latency_ms"] = (time.monotonic() - began) * 1000
        records.append(record)
        next_start = max(next_start + 1 / args.rate_per_worker, time.monotonic())
    if connection is not None:
        connection.close()
    return records


def run(args):
    start = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        futures = [executor.submit(worker, index, args, start + args.duration, start)
                   for index in range(args.concurrency)]
        records = [record for future in futures for record in future.result()]
    records.sort(key=lambda record: (record["elapsed_s"], record["worker"]))
    failures = [record for record in records if record["error"]]
    latency = [record["latency_ms"] for record in records]
    return {
        "schema_version": 1,
        "created_at": utc_now(),
        "environment": {"python": platform.python_version(), "platform": platform.platform()},
        "parameters": {key: str(value) if isinstance(value, Path) else value
                       for key, value in vars(args).items()},
        "summary": {
            "attempts": len(records), "failures": len(failures),
            "duration_s": time.monotonic() - start,
            "p50_ms": percentile(latency, .5), "p95_ms": percentile(latency, .95),
            "p99_ms": percentile(latency, .99),
            "first_failure": failures[0] if failures else None,
            "interpretation": "observations only; absence of failure does not prove #303 fixed",
        },
        "attempts": records,
    }


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--mode", choices=("fresh", "keepalive"), required=True)
    parser.add_argument("--duration", type=float, default=60)
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=5)
    parser.add_argument("--rate-per-worker", type=float, default=50)
    parser.add_argument("--max-requests-per-worker", type=int, default=10000)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for name, minimum, maximum in (
        ("port", 1024, 65535), ("duration", .1, 600), ("concurrency", 1, 64),
        ("timeout", .05, 30), ("rate_per_worker", .1, 1000),
        ("max_requests_per_worker", 1, 100000),
    ):
        value = getattr(args, name)
        if not math.isfinite(value) or not minimum <= value <= maximum:
            parser.error(f"{name} must be between {minimum} and {maximum}")
    if args.concurrency * args.max_requests_per_worker > 500000:
        parser.error("total attempt limit exceeds 500000")
    return args


def main():
    args = parse_args()
    # Open exclusively before sending traffic; never overwrite evidence.
    with args.output.open("x", encoding="utf-8") as output:
        report = run(args)
        json.dump(report, output, indent=2, ensure_ascii=False, allow_nan=False)
        output.write("\n")
    print(json.dumps(report["summary"], ensure_ascii=False))
    return 1 if report["summary"]["failures"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
