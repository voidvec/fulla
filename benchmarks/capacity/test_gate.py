"""Formal-scope and concurrent-reset admission tests, without external services."""

import copy
import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from gate import SCOPES, fixture_lock, validate_scope


class GateTests(unittest.TestCase):
    def setUp(self):
        self.fixture = {"owner_nonce": "nonce", "build": {"binary_sha256": "binary"}}
        self.gate = {
            "status": "passed",
            "driver_sha256": "driver",
            "binary_sha256": "binary",
            "fixture_nonce_sha256": hashlib.sha256(b"nonce").hexdigest(),
            "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
            "environment_sha256": "env",
            "scopes": copy.deepcopy(SCOPES),
        }

    def check(self, **changes):
        args = {
            "kind": "steady",
            "profile": "default",
            "vus": 4,
            "warmup": 30,
            "duration": 120,
            "drain": 60,
            "environment_hash": "env",
        }
        args.update(changes)
        with patch("gate.source_digest", return_value="driver"):
            validate_scope(self.gate, self.fixture, **args)

    def test_oversized_windows_or_cross_kind_budgets_are_rejected(self):
        self.check()
        for change in (
            {"duration": 7200},
            {"warmup": 300},
            {"drain": 300},
            {"profile": "soak"},
            {"vus": 16},
            {"environment_hash": "changed-pg-config"},
        ):
            with self.assertRaises(ValueError):
                self.check(**change)
        self.gate["scopes"]["steady"]["duration"] = 7200
        with self.assertRaises(ValueError):
            self.check(duration=7200)

    def test_resource_monitor_error_invalidates_session(self):
        import threading

        from session import Experiment

        experiment = Experiment.__new__(Experiment)
        experiment.current_child = experiment.server = None
        experiment.space_violation = threading.Event()
        experiment.resource_monitor_error = None
        with patch.object(
            experiment, "watch_resources", side_effect=OSError("injected stat failure")
        ):
            experiment.monitor_resources()
        self.assertTrue(experiment.space_violation.is_set())
        self.assertEqual(experiment.resource_monitor_error, "OSError")

    def test_second_reset_session_cannot_obtain_same_fixture_lease(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fixture.json"
            path.write_text("{}")
            with fixture_lock(path), self.assertRaises(ValueError), fixture_lock(path):
                pass
            with fixture_lock(path):
                pass


if __name__ == "__main__":
    unittest.main()
