#!/usr/bin/env python3
"""Exercise social state against the server executable using a real cookie jar.

Uses only the Python standard library. Requires the same isolated PostgreSQL /
Redis environment as EndpointTests_OutOfProcess; never contacts a social provider.
"""
import argparse
import copy
import http.cookiejar
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


def require(condition, description):
    if not condition:
        raise AssertionError(description)


def request(opener, base, path, body=b"{}", origin="http://localhost:5173", headers=None):
    fields = {"Content-Type": "application/json"}
    if origin:
        fields["Origin"] = origin
    fields.update(headers or {})
    req = urllib.request.Request(base + path, data=body, headers=fields, method="POST")
    try:
        response = opener.open(req, timeout=10)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        raw = response.read()
        return response.status, response.headers, json.loads(raw) if raw else None


def browser():
    return urllib.request.build_opener(urllib.request.ProxyHandler({}),
        urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))


def run(server, source):
    config = json.loads(source.read_text())
    if not config.get("db_clients") or not config.get("redis_clients"):
        print("[social-state] PostgreSQL and Redis configuration required")
        return 77
    for kind, prefix in (("db", "FULLA_DB"), ("redis", "FULLA_REDIS")):
        endpoint = config[kind + "_clients"][0]
        host = os.environ.get(prefix + "_HOST", endpoint.get("host", "127.0.0.1"))
        port = int(os.environ.get(prefix + "_PORT", endpoint["port"]))
        try:
            with socket.create_connection((host, port), timeout=2):
                pass
        except OSError:
            print(f"[social-state] {kind} prerequisite unavailable")
            return 77
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    config["listeners"] = [{"address": "127.0.0.1", "port": port}]
    config.setdefault("app", {}).update(run_as_daemon=False, relaunch_on_error=False)
    custom = config.setdefault("custom_config", {})
    custom.setdefault("frontend", {})["url"] = "http://localhost:5173"
    custom.setdefault("cors", {})["allow_origins"] = ["http://localhost:5173"]
    external = custom.setdefault("external_auth", {})
    external["tiers"] = {"domestic": True, "international": True}
    for provider in ("github", "google", "wechat"):
        external[provider] = {"client_id": "s01-test-client", "client_secret": "s01-test-secret",
            "appid": "s01-test-app", "secret": "s01-test-secret"}
    base = f"http://127.0.0.1:{port}"
    env = os.environ.copy()
    env["FULLA_ENV"] = "test"
    env["FULLA_FRONTEND_URL"] = "http://localhost:5173"
    env["FULLA_CORS_ORIGINS"] = "http://localhost:5173"
    with tempfile.TemporaryDirectory(prefix="fulla-social-state-") as directory:
        directory = Path(directory)
        for unavailable in (False, True):
            effective = copy.deepcopy(config)
            if unavailable:
                effective["redis_clients"][0].update(host="127.0.0.1", port=1, passwd="")
            (directory / "config.json").write_text(json.dumps(effective))
            child_env = env.copy()
            if unavailable:
                child_env["FULLA_REDIS_HOST"] = "127.0.0.1"
                child_env["FULLA_REDIS_PORT"] = "1"
                child_env["FULLA_REDIS_PASSWORD"] = ""
            with (directory / "server.log").open("w") as log:
                process = subprocess.Popen([str(server)], cwd=directory, env=child_env,
                    stdout=log, stderr=subprocess.STDOUT)
                try:
                    client = browser()
                    for _ in range(100):
                        try:
                            with client.open(base + "/health/live", timeout=1) as response:
                                if response.status == 200:
                                    break
                        except (OSError, urllib.error.URLError):
                            pass
                        if process.poll() is not None:
                            break
                        time.sleep(0.1)
                    else:
                        print("[social-state] server prerequisites unavailable")
                        return 77
                    require(process.poll() is None, "server exited during startup")
                    if unavailable:
                        status, _, _ = request(client, base, "/api/auth/providers/github/authorize")
                        require(status == 204, "unavailable store: initialization must not issue state")
                        started = time.monotonic()
                        status, headers, body = request(client, base, "/api/auth/providers/github/authorize")
                        require(status == 500 and body["error"]["code"] == "INTERNAL_ERROR", "storage outage must fail closed")
                        require(time.monotonic() - started < 8, "storage outage must complete within bound")
                        continue
                    status, _, _ = request(browser(), base, "/api/auth/providers/github/authorize", origin="")
                    require(status == 403, "missing Origin must fail before initialization")
                    status, _, _ = request(client, base, "/api/auth/providers/unknown/authorize")
                    require(status == 404, "unknown provider must be unavailable")
                    for provider in ("github", "google", "wechat"):
                        client = browser()
                        path = f"/api/auth/providers/{provider}/authorize"
                        status, headers, body = request(client, base, path)
                        require(status == 204 and body is None, "first request must initialize only")
                        cookie = headers.get("Set-Cookie", "")
                        require("HttpOnly" in cookie and "SameSite=Lax" in cookie and "Path=/" in cookie,
                            "binding cookie attributes required")
                        status, headers, body = request(client, base, path)
                        require(status == 200 and body["expires_in"] == 600, "second request must issue state")
                        state = body["state"]
                        require(re.fullmatch("[0-9a-f]{64}", state), "state must be a 32-byte opaque handle")
                        require(state in body["authorize_url"] and body["authorize_url"].startswith("https://"), "authorize URL must carry state")
                        require(body["redirect_uri"] == f"http://localhost:5173/callback/{provider}", "fixed provider callback required")
                        login = f"/api/{provider}/login"
                        invalid = [b'{}', b'{"code":"code-only"}',
                            json.dumps({"code": "probe", "state": "0" * 64}).encode(),
                            ('{"code":"a","code":"b","state":"' + state + '"}').encode()]
                        for payload in invalid:
                            status, headers, _ = request(client, base, login, payload)
                            require(status == 400, "invalid credentials must fail before upstream exchange")
                            require(headers.get("Cache-Control") == "no-store", "exchange response must not be cached")
                        payload = json.dumps({"code": "probe", "state": state}).encode()
                        status, _, _ = request(browser(), base, login, payload)
                        require(status == 400, "different browser must not exchange")
                        status, _, _ = request(client, base, login, payload, headers={"Sec-Fetch-Site": "cross-site"})
                        require(status == 403, "explicit cross-site request must not exchange")
                        status, _, _ = request(client, base, login + "?code=query&state=" + state)
                        require(status == 400, "query credentials must not exchange")
                        status, _, _ = request(client, base, path, headers={"Cookie": "fulla-social-binding-dev=malformed"})
                        require(status == 400, "malformed browser binding must be rejected")
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    print("[social-state] executable protocol and storage-outage checks passed")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-exe", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    args = parser.parse_args()
    raise SystemExit(run(args.server_exe.resolve(), args.config.resolve()))
