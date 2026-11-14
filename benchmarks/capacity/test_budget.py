"""Admission behavior: full evidence forecast and strict provenance."""

import json
import tempfile
import unittest
from pathlib import Path

from budget import forecast
from run import SCENARIOS


class BudgetTests(unittest.TestCase):
    def make_pilot(self, root):
        rows = []
        for profile in ("default", "tuned"):
            for scenario in SCENARIOS:
                directory = root / (profile + "-" + scenario)
                directory.mkdir()
                result = {
                    "status": "passed",
                    "measurement_seconds": 10,
                    "warmup_seconds": 2,
                    "result": {"http": {"cohort_attempts": 1000}},
                }
                (directory / "result.json").write_text(json.dumps(result))
                (directory / "attempts.jsonl.gz").write_bytes(b"x" * 1024)
                stats = {
                    "database_bytes": 16 * 1024 * 1024,
                    "wal_bytes": "1000000",
                    "max_wal_size_bytes": 128 * 1024 * 1024,
                }
                rows.append(
                    {
                        "profile": profile,
                        "scenario": scenario,
                        "status": "passed",
                        "vus": 4,
                        "result_path": str(
                            (directory / "result.json").relative_to(root)
                        ),
                        "postgres_before": stats,
                        "postgres_after_shutdown": stats,
                    }
                )
        session = {
            "status": "passed",
            "mode": "pilot",
            "driver_sha256": "driver",
            "binary_sha256": "binary",
            "fixture_nonce_sha256": "nonce",
            "environment_sha256": "environment",
            "environment": {"boot_id": "boot"},
            "rows": rows,
        }
        (root / "session.json").write_text(json.dumps(session))
        return session

    def call(self, root, free=8 * 1024**3, **overrides):
        args = {
            "current_free": free,
            "current_digest": "driver",
            "nonce_hash": "nonce",
            "binary_hash": "binary",
            "boot_id": "boot",
            "environment_hash": "environment",
        }
        args.update(overrides)
        return forecast(root, **args)

    def test_full_raw_archive_and_soft_wal_margin_are_reserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_pilot(root)
            report = self.call(root)
            self.assertEqual(report["status"], "passed")
            self.assertEqual(report["forecast"]["formal_rows"], 108)
            self.assertGreaterEqual(
                report["forecast"]["wal_headroom_bytes"], 384 * 1024**2
            )
            self.assertEqual(
                report["forecast"]["retained_raw_and_archive_bytes"],
                2
                * (
                    report["forecast"]["matrix_raw_bytes"]
                    + report["forecast"]["soak_fault_raw_bytes"]
                ),
            )
            blocked = self.call(root, free=report["required_free_bytes"] - 1)
            self.assertEqual(blocked["status"], "blocked")
            self.assertEqual(blocked["additional_free_bytes_needed"], 1)

    def test_stale_or_incomplete_pilot_cannot_authorize_measurement(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            session = self.make_pilot(root)
            for change in (
                {"boot_id": "newboot"},
                {"nonce_hash": "other"},
                {"current_digest": "newsource"},
            ):
                with self.assertRaises(ValueError):
                    self.call(root, **change)
            session["rows"].pop()
            (root / "session.json").write_text(json.dumps(session))
            with self.assertRaises(ValueError):
                self.call(root)


if __name__ == "__main__":
    unittest.main()
