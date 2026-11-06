"""Black-box transport checks for the diagnostic client, using only stdlib."""

import contextlib
import http.server
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest


PROBE = Path(__file__).with_name("probe.py")


@contextlib.contextmanager
def fixture(body=b"pong", delay=0):
    counts = {"connections": 0, "requests": 0}
    lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def setup(self):
            super().setup()
            with lock:
                counts["connections"] += 1

        def do_GET(self):
            with lock:
                counts["requests"] += 1
                count = counts["requests"]
            time.sleep(delay)
            try:
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("X-Diagnostic-Handled", str(count))
                self.end_headers()
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass  # Expected when testing a timed-out client.

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_port, counts
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


class ProbeTransportTest(unittest.TestCase):
    def invoke(self, port, output, mode="fresh", count=3, extra=()):
        return subprocess.run(
            [sys.executable, str(PROBE), "--port", str(port), "--mode", mode,
             "--duration", "2", "--concurrency", "1", "--rate-per-worker", "100",
             "--max-requests-per-worker", str(count), "--output", str(output), *extra],
            capture_output=True, text=True, timeout=8,
        )

    def test_fresh_opens_one_connection_per_attempt(self):
        with fixture() as (port, counts), tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "fresh.json"
            result = self.invoke(port, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(output.read_text())
            self.assertEqual(counts, {"connections": 3, "requests": 3})
            self.assertEqual(report["summary"]["attempts"], 3)
            self.assertTrue(all(row["new_connection"] for row in report["attempts"]))

    def test_keepalive_reuses_transport(self):
        with fixture() as (port, counts), tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "keepalive.json"
            result = self.invoke(port, output, mode="keepalive")
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(output.read_text())
            self.assertEqual(counts, {"connections": 1, "requests": 3})
            self.assertEqual(sum(row["new_connection"] for row in report["attempts"]), 1)

    def test_bad_body_is_retained_without_retry(self):
        with fixture(body=b"wrong") as (port, counts), tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "bad.json"
            result = self.invoke(port, output, count=1)
            self.assertEqual(result.returncode, 1, result.stderr)
            report = json.loads(output.read_text())
            self.assertEqual(counts["requests"], 1)
            self.assertEqual(report["summary"]["failures"], 1)
            self.assertEqual(report["summary"]["first_failure"]["stage"], "response_body")

    def test_header_timeout_is_retained_without_retry(self):
        with fixture(delay=.2) as (port, counts), tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "timeout.json"
            result = self.invoke(port, output, count=1, extra=("--timeout", ".05"))
            self.assertEqual(result.returncode, 1, result.stderr)
            report = json.loads(output.read_text())
            self.assertEqual(counts["requests"], 1)
            self.assertEqual(report["summary"]["first_failure"]["stage"], "response_headers")
            self.assertEqual(report["summary"]["failures"], 1)

    def test_existing_evidence_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "existing.json"
            output.write_text("existing evidence")
            result = self.invoke(18080, output, count=1)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_text(), "existing evidence")

    def test_invalid_rate_fails_before_opening_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "invalid.json"
            result = self.invoke(18080, output, extra=("--rate-per-worker", "nan"))
            self.assertEqual(result.returncode, 2)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
