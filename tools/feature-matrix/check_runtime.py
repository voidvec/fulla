#!/usr/bin/env python3
"""Probe public/optional routes in a real executable without external providers."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

from check_build import enabled


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-exe", type=Path, required=True)
    parser.add_argument("--social", type=enabled, required=True)
    parser.add_argument("--webauthn", type=enabled, required=True)
    parser.add_argument("--log-file", type=Path, required=True)
    parser.add_argument("--require-backends", action="store_true",
                        default=enabled(os.environ.get("FULLA_FEATURE_REQUIRE_BACKENDS", "OFF")))
    args = parser.parse_args()
    server = args.server_exe.resolve()
    config = json.loads((server.parent / "config.json").read_text())
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    config["listeners"] = [{"address": "127.0.0.1", "port": port}]
    config.setdefault("app", {}).update(relaunch_on_error=False, number_of_threads=2)
    base = f"http://127.0.0.1:{port}"

    def request(path: str, method: str = "GET") -> tuple[int, bytes]:
        req = urllib.request.Request(base + path, data=b"{}" if method == "POST" else None,
                                     method=method, headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=5) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as error:
            return error.code, error.read()

    args.log_file.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fulla-feature-surface-") as directory:
        work = Path(directory)
        (work / "config.json").write_text(json.dumps(config))
        for name in ("migrations", "seed"):
            if (server.parent / name).is_dir():
                shutil.copytree(server.parent / name, work / name)
        with args.log_file.open("wb") as log:
            process = subprocess.Popen([str(server)], cwd=work, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 25
                while True:
                    assert process.poll() is None, f"server exited; see {args.log_file}"
                    try:
                        if request("/health/live")[0] == 200:
                            break
                    except (OSError, urllib.error.URLError):
                        pass
                    assert time.monotonic() < deadline, f"server readiness timed out; see {args.log_file}"
                    time.sleep(0.1)
                status, body = request("/health/ready")
                assert status == 200, ("readiness", status)
                readiness = json.loads(body)
                if args.require_backends:
                    assert readiness["database"] == "connected", readiness
                    assert readiness["redis"] == "connected", readiness
                for path in ("/.well-known/openid-configuration", "/.well-known/jwks.json"):
                    status, body = request(path)
                    assert status == 200, (path, status)
                    assert isinstance(json.loads(body), dict), path
                status, body = request("/api/auth/providers")
                assert status == 200
                if not args.social:
                    assert json.loads(body)["providers"] == []
                    assert request("/api/auth/providers/github/authorize", "POST")[0] == 404
                for provider in ("github", "google", "wechat"):
                    status, _ = request(f"/api/{provider}/login", "POST")
                    assert (status != 404) if args.social else (status == 404), (provider, status)
                for path, method in (("/api/me/webauthn/credentials", "GET"),
                                     ("/api/me/webauthn/register/begin", "POST"),
                                     ("/oauth2/webauthn/authenticate/begin", "POST")):
                    status, _ = request(path, method)
                    assert (status != 404) if args.webauthn else (status == 404), (path, status)
                print(json.dumps({"status": "passed", "social": args.social, "webauthn": args.webauthn,
                                  "readiness": readiness, "required_backends": args.require_backends}))
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
