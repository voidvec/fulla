"""A successful row manifest cannot substitute for missing measurement artifacts."""

import gzip
import json
import tempfile
import unittest
from pathlib import Path

from report import export
from run import SCENARIOS


class ReportTests(unittest.TestCase):
    def test_missing_result_cannot_report_complete_matrix(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rows = []
            for profile in ("default", "tuned"):
                for scenario in SCENARIOS:
                    for vus in (1, 4):
                        for round_number in (1, 2, 3):
                            name = f"{profile}-{scenario}-{vus}-{round_number}"
                            directory = root / name
                            directory.mkdir()
                            for filename in ("attempts.jsonl.gz", "telemetry.jsonl.gz"):
                                with gzip.open(directory / filename, "wt") as stream:
                                    stream.write("{}\n")
                            stats = {
                                "cohort_attempts": 1,
                                "cohort_admissions": 1,
                                "cohort_success": 1,
                                "cohort_failure": 0,
                                "event_count_mismatch": False,
                                "missing_completions": 0,
                                "window_completion_qps": 1 / 120,
                                "cohort_wall_seconds": 120,
                                "latency_ns_all_attempts": {
                                    "p50": 1,
                                    "p95": 1,
                                    "p99": 1,
                                    "max": 1,
                                },
                            }
                            result = {
                                "status": "passed",
                                "kind": "steady",
                                "profile": profile,
                                "scenario": scenario,
                                "vus": vus,
                                "warmup_seconds": 30,
                                "measurement_seconds": 120,
                                "measurement_start_ns": 0,
                                "measurement_stop_ns": 120_000_000_000,
                                "observer": {"samples": 30, "errors": 0},
                                "result": {
                                    "http": stats,
                                    "flow": stats,
                                    "raw_bytes_compressed": (
                                        directory / "attempts.jsonl.gz"
                                    )
                                    .stat()
                                    .st_size,
                                },
                            }
                            (directory / "result.json").write_text(json.dumps(result))
                            rows.append(
                                {
                                    "id": name,
                                    "kind": "steady",
                                    "status": "passed",
                                    "profile": profile,
                                    "scenario": scenario,
                                    "vus": vus,
                                    "round": round_number,
                                    "result_path": name + "/result.json",
                                }
                            )
            (root / "session.json").write_text(
                json.dumps({"status": "passed", "mode": "matrix", "rows": rows})
            )
            self.assertTrue(
                export(root, root / "complete.csv")["complete_steady_matrix"]
            )
            saved = (root / rows[0]["result_path"]).read_text()
            (root / rows[0]["result_path"]).unlink()
            missing = export(root, root / "missing.csv")
            self.assertFalse(missing["complete_steady_matrix"])
            self.assertIn(
                "missing_or_unreadable_result", (root / "missing.csv").read_text()
            )
            self.assertEqual(missing["rows_exported"], 108)
            broken = json.loads(saved)
            del broken["result"]["http"]["cohort_wall_seconds"]
            (root / rows[0]["result_path"]).write_text(json.dumps(broken))
            malformed = export(root, root / "malformed.csv")
            self.assertFalse(malformed["complete_steady_matrix"])
            self.assertEqual(malformed["rows_exported"], 108)


if __name__ == "__main__":
    unittest.main()
