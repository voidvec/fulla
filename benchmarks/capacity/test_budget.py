"""Admission behavior: full evidence forecast and strict provenance."""

import json
import tempfile
import unittest
from pathlib import Path

from budget import forecast
from run import SCENARIOS
from evidence_test_support import write_measurement


class BudgetTests(unittest.TestCase):
    def make_pilot(self, root):
        rows = []
        for profile in ("default", "tuned"):
            for scenario in SCENARIOS:
                directory = root / (profile + "-" + scenario)
                directory.mkdir()
                stats = {
                    "database_bytes": 16 * 1024 * 1024,
                    "wal_bytes": "1000000",
                    "max_wal_size_bytes": 128 * 1024 * 1024,
                }
                rows.append(
                    {
                        "id": directory.name,
                        "kind": "pilot",
                        "round": 1,
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
                write_measurement(directory, rows[-1], seconds=10, warmup=2, samples=3)
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

    def test_missing_and_corrupt_raw_artifacts_cannot_authorize(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            session = self.make_pilot(root)
            directory = (root / session['rows'][0]['result_path']).parent
            for filename in ('attempts.jsonl.gz', 'telemetry.jsonl.gz'):
                path = directory / filename
                saved = path.read_bytes()
                for data in (None, saved[:-3], bytes([saved[0] ^ 1]) + saved[1:],
                             saved[:10] + b'\x07' + saved[-8:]):
                    with self.subTest(file=filename, missing=data is None):
                        if data is None:
                            path.unlink()
                        else:
                            path.write_bytes(data)
                        with self.assertRaises((ValueError, OSError, EOFError)):
                            self.call(root)
                        path.write_bytes(saved)

    def test_summary_only_or_sample_count_mismatch_cannot_authorize(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            session = self.make_pilot(root)
            path = root / session['rows'][0]['result_path']
            original = path.read_text()
            for kind in ('seal', 'samples', 'stats'):
                result = json.loads(original)
                if kind == 'seal':
                    del result['artifacts']
                elif kind == 'samples':
                    result['observer']['samples'] += 1
                else:
                    result['result']['http']['cohort_attempts'] += 1
                path.write_text(json.dumps(result))
                with self.assertRaises(ValueError):
                    self.call(root)
            path.write_text(original)

    def test_non_pilot_or_insufficient_observer_cannot_authorize(self):
        for kind, seconds, samples in [('smoke', 10, 3), ('steady', 10, 3), ('pilot', 1200, 1)]:
            with self.subTest(kind=kind, seconds=seconds), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                session = self.make_pilot(root)
                row = session['rows'][0]
                row['kind'] = kind
                write_measurement((root / row['result_path']).parent, row,
                                  seconds=seconds, warmup=2, samples=samples)
                (root / 'session.json').write_text(json.dumps(session))
                with self.assertRaises(ValueError):
                    self.call(root)

    def test_row_provenance_must_match_admitted_pilot(self):
        for field in ('driver_sha256', 'binary_sha256', 'fixture_nonce_sha256'):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                session = self.make_pilot(root)
                path = root / session['rows'][0]['result_path']
                result = json.loads(path.read_text())
                if field == 'binary_sha256':
                    result['build'][field] = 'other-binary'
                else:
                    result[field] = 'other-provenance'
                path.write_text(json.dumps(result))
                with self.assertRaises(ValueError):
                    self.call(root)

    def test_symlinked_artifact_cannot_authorize(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            session = self.make_pilot(root)
            directory = (root / session['rows'][0]['result_path']).parent
            path = directory / 'attempts.jsonl.gz'
            external = root / 'external.jsonl.gz'
            path.rename(external)
            path.symlink_to(external)
            with self.assertRaises(ValueError):
                self.call(root)


if __name__ == "__main__":
    unittest.main()
