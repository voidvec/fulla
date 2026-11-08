"""Real-fixture smoke checks. Set FULLA_CONNECTION_PROBE_BINARY to the built binary."""

import gzip
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

BINARY = os.environ.get("FULLA_CONNECTION_PROBE_BINARY")
RUNNER = Path(__file__).with_name("run_matrix.py")


@unittest.skipUnless(BINARY and sys.platform == "linux", "requires Linux and the compiled fixture")
class MatrixSmokeTest(unittest.TestCase):
    def launch(self, root, duration=600, timeout=.1, extra=()):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        log = (root / "runner.log").open("w")
        process = subprocess.Popen([
            sys.executable, str(RUNNER), "--binary", BINARY, "--output", str(root / "results"),
            "--port", str(port), "--rounds", "1", "--duration", str(duration),
            "--timeout", str(timeout), "--warmup", "0", "--drain", ".1",
            "--sample-interval", "1", "--concurrency", "1", *extra], stdout=log, stderr=log)
        self.addCleanup(log.close)

        def cleanup():
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)

        self.addCleanup(cleanup)
        return process, port

    def baseline(self, root):
        cell = root / "results" / "round-1-1loops-reuse-false-fresh"
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                with gzip.open(cell / "snapshots.jsonl.gz", "rt") as stream:
                    return cell, json.loads(stream.readline())
            except (OSError, EOFError, ValueError):
                time.sleep(.02)
        self.fail("baseline not captured within 10 seconds")

    def assert_gone(self, pid, port):
        self.assertFalse(Path(f"/proc/{pid}").exists(), "fixture was orphaned")
        with socket.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("127.0.0.1", port))

    def test_sigterm_preserves_partial_evidence_and_stops_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            process, port = self.launch(root)
            cell, baseline = self.baseline(root)
            time.sleep(.1)
            process.terminate()
            self.assertEqual(process.wait(timeout=10), 2)
            summary = json.loads((cell / "summary.json").read_text())
            self.assertFalse(summary["valid"])
            self.assertIn("InterruptedError", summary["error"])
            self.assertTrue((root / "results" / "matrix-summary.json").exists())
            with gzip.open(cell / "attempts.jsonl.gz", "rt") as stream:
                self.assertGreater(len([json.loads(line) for line in stream]), 0)
            self.assert_gone(baseline["pid"], port)

    def test_selected_replacement_has_one_cell_and_explicit_round(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            process, _ = self.launch(root, duration=.2,
                                     extra=("--scenario", "4-true-keepalive", "--first-round", "3"))
            self.assertEqual(process.wait(timeout=10), 0, (root / "runner.log").read_text())
            cells = json.loads((root / "results" / "matrix-summary.json").read_text())
            self.assertEqual(len(cells), 1)
            self.assertEqual(cells[0]["cell"], "round-3-4loops-reuse-true-keepalive")
            self.assertTrue(cells[0]["valid"])

    def test_controlled_pause_captures_first_failure_and_endpoints(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            process, port = self.launch(root, duration=1.5)
            cell, baseline = self.baseline(root)
            pid = baseline["pid"]
            os.kill(pid, signal.SIGSTOP)
            try:
                time.sleep(.35)
            finally:
                os.kill(pid, signal.SIGCONT)
            # Let the short six-cell matrix finish; never call this a #303 reproduction.
            self.assertEqual(process.wait(timeout=30), 1, (root / "runner.log").read_text())
            summary = json.loads((cell / "summary.json").read_text())
            self.assertTrue(summary["valid"])
            self.assertGreater(summary["summary"]["failures"], 0)
            with gzip.open(cell / "snapshots.jsonl.gz", "rt") as stream:
                captures = [json.loads(line) for line in stream]
            failure = next(row for row in captures if row["kind"] == "first_failure")
            periodic = next(row for row in captures if row["kind"] == "periodic")
            self.assertEqual(periodic["server_owned_states"].get("LISTEN"), 1)
            self.assertIn("/net/tcp", periodic["server_sockets"]["source"])
            self.assertEqual(failure["failure"]["stage"], "response_headers")
            self.assertEqual(failure["failure"]["local_endpoint"][0], "127.0.0.1")
            with gzip.open(cell / "attempts.jsonl.gz", "rt") as stream:
                attempts = [json.loads(line) for line in stream]
            self.assertEqual(len(attempts), summary["summary"]["attempts"])
            self.assertEqual(len({(row["worker"], row["sequence"]) for row in attempts}), len(attempts))
            self.assert_gone(pid, port)


if __name__ == "__main__":
    unittest.main()
