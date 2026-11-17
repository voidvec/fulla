"""Exercise probe isolation without a server, database or external providers."""
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch
import urllib.error
import urllib.parse

import check_runtime


class RuntimeProbeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="feature-probe-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.server = self.root / "bin" / "fulla-server"
        self.server.parent.mkdir()
        self.config = self.root / "runtime.json"
        self.config.write_text(json.dumps({"marker": "selected-config", "app": {}}))
        self.resources = self.root / "resources"
        for name in ("migrations", "seed"):
            directory = self.resources / name
            directory.mkdir(parents=True)
            (directory / "owned.sql").write_text("-- test resource\n")
        self.process = Mock()
        self.process.poll.return_value = None
        self.process.wait.return_value = 0
        self.snapshot = {}

    def invoke(self, social=True, webauthn=True, fail_route=False):
        def start(*args, **kwargs):
            work = kwargs["cwd"]
            self.snapshot = {
                "config": json.loads((work / "config.json").read_text()),
                "resources": [(work / n / "owned.sql").read_text()
                              for n in ("migrations", "seed")],
                "env": kwargs.get("env"),
            }
            return self.process

        def response(request, **kwargs):
            path = urllib.parse.urlsplit(request.full_url).path
            status = 200
            body = {}
            if path == "/health/ready":
                body = {"database": "connected", "redis": "connected"}
            elif path == "/api/auth/providers":
                body = {"providers": []}
            elif path.startswith("/api/auth/providers/") or path in (
                    "/api/github/login", "/api/google/login", "/api/wechat/login"):
                status = 400 if social else 404
            elif "webauthn" in path:
                status = 401 if webauthn else 404
            if fail_route and path == "/.well-known/jwks.json":
                status = 500
            if status >= 400:
                raise urllib.error.HTTPError(request.full_url, status, "fixture", {}, io.BytesIO(b"{}"))
            result = Mock()
            result.status = status
            result.read.return_value = json.dumps(body).encode()
            result.__enter__ = Mock(return_value=result)
            result.__exit__ = Mock(return_value=False)
            return result

        argv = ["check_runtime.py", "--server-exe", str(self.server),
                "--config", str(self.config), "--resources-dir", str(self.resources),
                "--social", "ON" if social else "OFF",
                "--webauthn", "ON" if webauthn else "OFF",
                "--log-file", str(self.root / "server.log"), "--require-backends"]
        listener = Mock()
        listener.__enter__ = Mock(return_value=listener)
        listener.__exit__ = Mock(return_value=False)
        listener.getsockname.return_value = ("127.0.0.1", 23456)
        with patch("sys.argv", argv), patch.object(check_runtime.subprocess, "Popen", side_effect=start) as popen, \
                patch.object(check_runtime.socket, "socket", return_value=listener), \
                patch.object(check_runtime.urllib.request, "urlopen", side_effect=response), \
                redirect_stdout(io.StringIO()):
            check_runtime.main()
        return popen

    def test_selected_config_and_resources_work_without_adjacent_config(self):
        self.invoke()
        self.assertEqual(self.snapshot["config"]["marker"], "selected-config")
        self.assertEqual(self.snapshot["resources"], ["-- test resource\n"] * 2)
        self.process.terminate.assert_called_once()
        self.process.wait.assert_called_once_with(timeout=5)

    def test_explicit_config_wins_over_adjacent_config(self):
        (self.server.parent / "config.json").write_text('{"marker":"wrong"}')
        self.invoke()
        self.assertEqual(self.snapshot["config"]["marker"], "selected-config")

    def test_child_port_matches_json_and_preserves_backend_environment(self):
        with patch.dict(os.environ, {"FULLA_LISTEN_PORT": "1", "FULLA_DB_HOST": "owned-db"}):
            before = os.environ.copy()
            self.invoke()
            selected = str(self.snapshot["config"]["listeners"][0]["port"])
            self.assertNotEqual(selected, "1")
            expected = before.copy()
            expected["FULLA_LISTEN_PORT"] = selected
            self.assertEqual(self.snapshot["env"], expected)
            self.assertEqual(dict(os.environ), before)

    def test_missing_config_fails_before_launch(self):
        self.config.unlink()
        with self.assertRaises(FileNotFoundError):
            self.invoke()
        self.process.terminate.assert_not_called()

    def test_missing_resource_fails_before_launch(self):
        (self.resources / "seed" / "owned.sql").unlink()
        (self.resources / "seed").rmdir()
        with self.assertRaises(FileNotFoundError):
            self.invoke()
        self.process.terminate.assert_not_called()

    def test_failed_protocol_probe_still_reaps_child(self):
        with self.assertRaises(AssertionError):
            self.invoke(fail_route=True)
        self.process.terminate.assert_called_once()
        self.process.wait.assert_called_once_with(timeout=5)

    def test_optional_route_checks_for_all_four_combinations(self):
        for social in (False, True):
            for webauthn in (False, True):
                with self.subTest(social=social, webauthn=webauthn):
                    self.invoke(social=social, webauthn=webauthn)


if __name__ == "__main__":
    unittest.main()
