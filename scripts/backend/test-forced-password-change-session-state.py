#!/usr/bin/env python3
"""Characterize forced-password-change sessions against an isolated executable."""

import argparse
import base64
import copy
import hashlib
import http.cookiejar
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


REDIRECT = "http://127.0.0.1:9987/callback"
ADMIN_REDIRECT = "http://127.0.0.1:5174/admin/callback"
CHANGE = "/oauth2/password/change"


def require(condition, description):
    if not condition:
        raise AssertionError(description)


def unique():
    return uuid.uuid4().hex


def clone_cookies(source):
    jar = http.cookiejar.CookieJar()
    for cookie in source:
        jar.set_cookie(copy.deepcopy(cookie))
    return jar


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Browser:
    def __init__(self, base, deadline, cookies=None):
        self.base = base
        self.deadline = deadline
        self.jar = clone_cookies(cookies) if cookies is not None else http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}), NoRedirect(),
            urllib.request.HTTPCookieProcessor(self.jar))
        self.bearer = ""

    def session_id(self):
        values = [cookie.value for cookie in self.jar if cookie.name == "JSESSIONID"]
        require(len(values) == 1 and bool(values[0]), "one session cookie required")
        return values[0]

    def send(self, method, path, form=None, payload=None, raw=None, basic=None, limit=15):
        remaining = self.deadline - time.monotonic()
        require(remaining > 0, "HTTP phase deadline exceeded")
        headers = {}
        data = None
        if payload is not None:
            data = json.dumps(payload).encode()
            headers["Content-Type"] = "application/json"
        elif raw is not None:
            data = raw
            headers["Content-Type"] = "application/json"
        elif form is not None:
            data = urllib.parse.urlencode(form).encode()
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        if basic is not None:
            credentials = ":".join(urllib.parse.quote(value, safe="") for value in basic)
            headers["Authorization"] = "Basic " + base64.b64encode(credentials.encode()).decode()
        elif self.bearer:
            headers["Authorization"] = "Bearer " + self.bearer
        request = urllib.request.Request(self.base + path, data=data, method=method, headers=headers)
        try:
            response = self.opener.open(request, timeout=min(limit, remaining))
        except urllib.error.HTTPError as error:
            response = error
        with response:
            content = response.read()
            # A redirect may have HTML content. Never print response bodies.
            body = json.loads(content) if content and "json" in response.headers.get("Content-Type", "") else {}
            return response.status, response.headers, body


def json_response(response, status):
    require(response[0] == status, "unexpected HTTP status (expected " + str(status) + ")")
    require(isinstance(response[2], dict), "JSON object required")
    return response[2]


def error_response(response, status, code):
    body = json_response(response, status)
    require(body.get("error", {}).get("code") == code, "unexpected application error code")


class Fixture:
    def __init__(self, base, deadline):
        self.base = base
        self.deadline = deadline
        self.admin = Browser(base, deadline)
        self.users = []
        self.client = ""
        self.secret = ""

    def browser(self, cookies=None):
        return Browser(self.base, self.deadline, cookies)

    def parameters(self, client=None, redirect=REDIRECT):
        verifier = secrets.token_urlsafe(32)
        challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip("=")
        return verifier, {"client_id": client or self.client, "redirect_uri": redirect,
                          "scope": "openid profile", "state": "c1b-" + unique(),
                          "nonce": unique(), "code_challenge": challenge,
                          "code_challenge_method": "S256"}

    def login(self, browser, user, password=None):
        verifier, params = self.parameters()
        params.update(username=user["username"], password=password or user["password"])
        return browser.send("POST", "/oauth2/login?json=true", form=params), verifier

    def tokens(self, browser, user):
        response, verifier = self.login(browser, user)
        body = json_response(response, 200)
        require(bool(body.get("code")), "normal login must issue a code")
        return self.token({"grant_type": "authorization_code", "code": body["code"],
                           "redirect_uri": REDIRECT, "code_verifier": verifier})

    def token(self, fields):
        body = json_response(self.browser().send("POST", "/oauth2/token", form=fields,
                                                 basic=(self.client, self.secret)), 200)
        require(bool(body.get("access_token")) and bool(body.get("refresh_token")), "token pair required")
        return body

    def introspect(self, token):
        return json_response(self.browser().send("POST", "/oauth2/introspect", form={"token": token},
                                                 basic=(self.client, self.secret)), 200)

    def setup(self):
        verifier, params = self.parameters("fulla-admin-console", ADMIN_REDIRECT)
        params.update(username="admin", password="admin", scope="openid profile admin")
        body = json_response(self.admin.send("POST", "/oauth2/login?json=true", form=params), 200)
        require(bool(body.get("code")), "seed administrator login failed")
        body = json_response(self.admin.send("POST", "/oauth2/token", form={
            "grant_type": "authorization_code", "client_id": "fulla-admin-console",
            "code": body["code"], "code_verifier": verifier, "redirect_uri": ADMIN_REDIRECT}), 200)
        self.admin.bearer = body.get("access_token", "")
        require(bool(self.admin.bearer), "administrator token required")
        body = json_response(self.admin.send("POST", "/api/admin/clients", payload={
            "name": "c1b-" + unique(), "redirect_uris": REDIRECT, "client_type": "CONFIDENTIAL",
            "token_endpoint_auth_method": "client_secret_basic",
            "allowed_grant_types": "authorization_code,refresh_token"}), 201)
        self.client, self.secret = body.get("client_id", ""), body.get("client_secret", "")
        require(bool(self.client) and bool(self.secret), "confidential client credentials required")
        require(body.get("token_endpoint_auth_method") == "client_secret_basic", "Basic declaration required")
        path = "/api/admin/clients/" + self.client + "/scopes"
        json_response(self.admin.send("PUT", path, payload={"scopes": ["openid", "profile"]}), 200)
        deadline = min(self.deadline, time.monotonic() + 5)
        while time.monotonic() < deadline:
            scopes = json_response(self.admin.send("GET", path, limit=max(0.01, deadline - time.monotonic())), 200)
            if sorted(scopes.get("scopes", [])) == ["openid", "profile"]:
                return
            time.sleep(0.02)
        raise AssertionError("scope commit visibility deadline exceeded")

    def user(self):
        require(len(self.users) < 10, "fixture user budget exceeded")
        user = {"username": "c1b_" + unique(), "password": secrets.token_urlsafe(24) + "aA1!"}
        body = json_response(self.admin.send("POST", "/api/admin/users", payload={
            **user, "email": unique() + "@fixture.invalid", "email_verified": True,
            "must_change_password": False}), 201)
        identity = body.get("user", {}).get("id", "")
        require(isinstance(identity, (str, int)) and bool(identity), "created user identity required")
        user["id"] = str(identity)
        self.users.append(user)
        return user

    def flags(self, user, **flags):
        json_response(self.admin.send("PUT", "/api/admin/users/" + user["id"], payload=flags), 200)
        detail = json_response(self.admin.send("GET", "/api/admin/users/" + user["id"]), 200)
        for key, value in flags.items():
            require(detail.get(key) == value, "user flag update not visible")

    def forced(self, browser, user):
        body = json_response(self.login(browser, user)[0], 200)
        require(body.get("password_change_required") is True, "forced change marker required")
        require("code" not in body, "forced login must not issue code")

    def authorize_anonymous(self, browser):
        _, params = self.parameters()
        params.update(response_type="code", prompt="none")
        response = browser.send("GET", "/oauth2/authorize?" + urllib.parse.urlencode(params))
        require(response[0] == 302, "anonymous authorize redirect required")
        location = urllib.parse.urlsplit(response[1].get("Location", ""))
        target = urllib.parse.urlsplit(REDIRECT)
        require((location.scheme, location.netloc, location.path) == (target.scheme, target.netloc, target.path),
                "anonymous authorize must use registered redirect")
        query = urllib.parse.parse_qs(location.query)
        require(query.get("error") == ["login_required"] and query.get("state") == [params["state"]],
                "anonymous authorize error/state mismatch")
        require("code" not in query, "anonymous authorize must not issue code")

    def consent(self, browser, subject):
        _, params = self.parameters()
        params.update(user_id=subject, action="approve", consent_csrf="unconsumed-c1b-probe")
        return browser.send("POST", "/oauth2/consent", form=params)

    def retire(self):
        self.admin.deadline = time.monotonic() + 60
        disabled = 0
        for user in self.users:
            try:
                response = self.admin.send("PUT", "/api/admin/users/" + user["id"] + "/disable", limit=5)
                if response[0] == 200:
                    disabled += 1
            except Exception:
                pass
        print(json.dumps({"fixture_users_created": len(self.users), "fixture_users_disabled": disabled,
                          "fixture_clients_retained": int(bool(self.client))}))
        require(disabled == len(self.users), "fixture retirement incomplete")


def success(fixture, verification=False):
    user = fixture.user()
    browser = fixture.browser()
    tokens = fixture.tokens(browser, user)
    # Keep an unused successor RT. Check AT revocation before touching its family.
    tokens = fixture.token({"grant_type": "refresh_token", "refresh_token": tokens["refresh_token"]})
    intro = fixture.introspect(tokens["access_token"])
    require(intro.get("active") is True and bool(intro.get("sub")), "positive introspection required")
    fixture.flags(user, must_change_password=True, email_verified=not verification)
    fixture.forced(browser, user)
    old_cookie = clone_cookies(browser.jar)
    old_id = browser.session_id()
    password = secrets.token_urlsafe(24) + "aA1!"
    change = {"old_password": user["password"], "new_password": password}
    body = json_response(browser.send("POST", CHANGE, payload=change), 200)
    expected = ("Password changed successfully. A verification email has been sent to your address "
                "\u2014 verify it, then sign in." if verification else
                "Password changed successfully. Sign in again to continue.")
    require(body == {"message": expected}, "success response contract changed")
    require(browser.session_id() != old_id, "successful change must rotate cookie")
    detail = json_response(fixture.admin.send("GET", "/api/admin/users/" + user["id"]), 200)
    require(detail.get("must_change_password") is False, "successful change must clear flag")
    require(fixture.introspect(tokens["access_token"]).get("active") is False, "AT must be revoked first")
    rejected = json_response(fixture.browser().send("POST", "/oauth2/token", form={
        "grant_type": "refresh_token", "refresh_token": tokens["refresh_token"]},
        basic=(fixture.client, fixture.secret)), 400)
    require(rejected.get("error") == "invalid_grant", "unused RT must be revoked")
    require("access_token" not in rejected and "refresh_token" not in rejected, "revoked RT issued tokens")
    # Freeze both jars before any re-login. Old-ID grace alias is allowed.
    for cookies in (old_cookie, clone_cookies(browser.jar)):
        probe = fixture.browser(cookies)
        fixture.authorize_anonymous(probe)
        error_response(fixture.consent(probe, intro["sub"]), 401, "AUTH_SESSION_REQUIRED")
        error_response(probe.send("POST", CHANGE, payload=change), 401, "AUTH_SESSION_REQUIRED")
    if not verification:
        response, _ = fixture.login(fixture.browser(), user, password)
        require(bool(json_response(response, 200).get("code")), "new password login failed")
        error_response(fixture.login(fixture.browser(), user)[0], 401, "AUTH_INVALID_CREDENTIALS")


def rejected_changes(fixture):
    cases = (("wrong-old", 401, "AUTH_INVALID_CREDENTIALS"),
             ("missing", 400, "VALIDATION_MISSING_REQUIRED_FIELD"),
             ("short", 400, "VALIDATION_PASSWORD_TOO_SHORT"),
             ("long", 400, "VALIDATION_INVALID_INPUT"),
             ("form", 400, "VALIDATION_INVALID_INPUT"),
             ("malformed", 400, "VALIDATION_INVALID_INPUT"))
    for name, status, code in cases:
        print("[forced-password-session] checking rejected " + name)
        user = fixture.user()
        browser = fixture.browser()
        tokens = fixture.tokens(browser, user)
        intro = fixture.introspect(tokens["access_token"])
        require(intro.get("active") is True and bool(intro.get("sub")), "positive introspection required")
        fixture.flags(user, must_change_password=True)
        fixture.forced(browser, user)
        cookie = browser.session_id()
        body = {"old_password": user["password"], "new_password": secrets.token_urlsafe(24) + "aA1!"}
        if name == "wrong-old":
            body["old_password"] = "incorrect-c1b-password"
        elif name == "missing":
            del body["new_password"]
        elif name == "short":
            body["new_password"] = "x"
        elif name == "long":
            body["new_password"] = "a" * 129
        kwargs = {"form": body} if name == "form" else {"raw": b"{"} if name == "malformed" else {"payload": body}
        error_response(browser.send("POST", CHANGE, **kwargs), status, code)
        require(browser.session_id() == cookie, "rejected change rotated cookie")
        detail = json_response(fixture.admin.send("GET", "/api/admin/users/" + user["id"]), 200)
        require(detail.get("must_change_password") is True, "rejected change cleared flag")
        fixture.authorize_anonymous(browser)
        error_response(fixture.consent(browser, intro["sub"]), 403, "AUTH_PASSWORD_CHANGE_REQUIRED")
        require(fixture.introspect(tokens["access_token"]).get("active") is True, "rejected change revoked AT")
        fixture.token({"grant_type": "refresh_token", "refresh_token": tokens["refresh_token"]})
        print("[forced-password-session] rejected " + name + " passed")


def unauthorized(fixture):
    for authenticated in (False, True):
        user = fixture.user()
        browser = fixture.browser()
        if authenticated:
            fixture.tokens(browser, user)
        error_response(browser.send("POST", CHANGE, payload={"old_password": user["password"],
            "new_password": secrets.token_urlsafe(24) + "aA1!"}), 401, "AUTH_SESSION_REQUIRED")
        response, _ = fixture.login(fixture.browser(), user)
        require(bool(json_response(response, 200).get("code")), "unflagged rejection changed password")


def run(args):
    config = json.loads(args.config.read_text())
    plugins = [entry for entry in config.get("plugins", []) if entry.get("name") == "OAuth2Plugin"]
    require(len(plugins) == 1, "exactly one OAuth2Plugin configuration required")
    plugin = plugins[0].setdefault("config", {})
    if plugin.get("storage_type") == "memory":
        print("[forced-password-session] SKIP memory storage; PostgreSQL acceptance not exercised")
        return 77
    require(args.server_exe.is_file(), "server executable required")
    deadline = time.monotonic() + 180
    plugin.setdefault("cache", {})["enabled"] = False
    for kind, prefix in (("db", "FULLA_DB"), ("redis", "FULLA_REDIS")):
        endpoints = config.get(kind + "_clients", [])
        require(bool(endpoints), "PostgreSQL/Redis configuration required")
        endpoint = endpoints[0]
        host = os.environ.get(prefix + "_HOST") or endpoint.get("host", "127.0.0.1")
        port = int(os.environ.get(prefix + "_PORT") or endpoint["port"])
        with socket.create_connection((host, port), timeout=min(2, max(0.01, deadline - time.monotonic()))):
            pass
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    config["listeners"] = [{"address": "127.0.0.1", "port": port}]
    app = config.setdefault("app", {})
    app.update(run_as_daemon=False, relaunch_on_error=False, number_of_threads=2,
               enable_session=True, session_cookie_key="JSESSIONID")
    app.setdefault("log", {}).update(log_level="WARN", log_path="", use_spdlog=False)
    env = os.environ.copy()
    for name in ("FULLA_SMTP_HOST", "FULLA_SMTP_USER", "FULLA_SMTP_PASSWORD"):
        env.pop(name, None)
    env.update(FULLA_ENV="test", FULLA_LISTEN_PORT=str(port))
    with tempfile.TemporaryDirectory(prefix="fulla-forced-session-") as temporary:
        work = Path(temporary) / "a" / "b"
        work.mkdir(parents=True)
        require(all(not (directory / ".env").exists() for directory in (work, work.parent, work.parent.parent)),
                "isolated email environment required")
        (work / "config.json").write_text(json.dumps(config))
        for name in ("migrations", "seed"):
            shutil.copytree(args.resources_dir / name, work / name)
        print("[forced-password-session] cache=false; isolated Console email prerequisites; startup log=WARN")
        fixture = Fixture("http://127.0.0.1:" + str(port), deadline)
        with (work / "server.log").open("wb") as log:
            process = subprocess.Popen([str(args.server_exe)], cwd=work, env=env,
                                       stdout=log, stderr=subprocess.STDOUT)
            try:
                readiness = min(deadline, time.monotonic() + 40)
                while True:
                    require(process.poll() is None, "server exited before readiness")
                    require(time.monotonic() < readiness, "server readiness deadline exceeded")
                    try:
                        if fixture.browser().send("GET", "/health/live", limit=1)[0] == 200:
                            break
                    except (OSError, urllib.error.URLError):
                        pass
                    time.sleep(0.05)
                health = json_response(fixture.browser().send("GET", "/health/ready"), 200)
                require(health.get("database") == "connected" and health.get("redis") == "connected",
                        "real PostgreSQL and Redis readiness required")
                fixture.setup()
                print("[forced-password-session] checking H1/H2")
                success(fixture)
                print("[forced-password-session] H1/H2 password, revocation and both anonymous cookies passed")
                rejected_changes(fixture)
                unauthorized(fixture)
                print("[forced-password-session] H3/H4 rejected and unauthorized changes passed")
                print("[forced-password-session] checking H5")
                success(fixture, verification=True)
                print("[forced-password-session] H5 unverified email response and session contract passed")
            finally:
                already_failed = sys.exc_info()[0] is not None
                cleanup_errors = []
                try:
                    fixture.retire()
                except Exception:
                    cleanup_errors.append("fixture retirement incomplete")
                finally:
                    prior_exit = process.poll()
                    if prior_exit is None:
                        process.terminate()
                    forced_kill = False
                    try:
                        exit_code = process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        forced_kill = True
                        process.kill()
                        exit_code = process.wait(timeout=5)
                    if prior_exit is not None:
                        cleanup_errors.append("server exited unexpectedly: " + str(prior_exit))
                    if forced_kill:
                        cleanup_errors.append("server graceful shutdown deadline exceeded")
                    allowed = (0, -15) if os.name != "nt" else (0, 1)
                    if exit_code not in allowed:
                        cleanup_errors.append("unexpected server exit code: " + str(exit_code))
                if cleanup_errors:
                    log_path = work / "server.log"
                    if log_path.is_file():
                        lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
                        print("[forced-password-session] server.log tail (last 60 lines):")
                        for line in lines[-60:]:
                            print("  " + line)
                for problem in cleanup_errors:
                    print("[forced-password-session] cleanup FAILED: " + problem)
                if cleanup_errors and not already_failed:
                    raise AssertionError("fixture/server cleanup failed")
    print("[forced-password-session] all checks passed")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-exe", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--resources-dir", required=True, type=Path)
    options = parser.parse_args()
    for field in ("server_exe", "config", "resources_dir"):
        setattr(options, field, getattr(options, field).resolve())
    try:
        raise SystemExit(run(options))
    except AssertionError as error:
        print("[forced-password-session] FAILED: " + str(error))
        raise SystemExit(1)
    except Exception:
        # Library exceptions can contain URLs/bodies. Keep credentials out of CI.
        print("[forced-password-session] FAILED: transport, configuration or response decoding")
        raise SystemExit(1)
