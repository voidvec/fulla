"""Behavior tests for real HTTP state, one-use RTs and measurement boundaries."""

import base64
import gzip
import hashlib
import json
import secrets
import socket
import tempfile
import threading
import time
import unittest
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlsplit

from protocol import ProtocolError, Session, User
from support import Pacer, Recorder, quantile, summarize


class ProtocolServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.state_lock = threading.Lock()
        self.sessions, self.codes, self.nonces, self.tokens, self.refreshes = (
            {},
            {},
            {},
            {},
            {},
        )
        self.calls = []
        self.inactive = False
        self.bad_origin = False
        self.drop_refresh_response = False
        self.rate_limited = False
        self.bad_subject = False
        self.bad_client = False
        self.bad_ttl = None
        self.bad_token_type = False
        self.bad_nonce = False
        self.omit_scope = False
        self.trickle = False

    def mint(self, subject, nonce=None):
        token, refresh = (
            "AT-secret-" + secrets.token_hex(8),
            "RT-secret-" + secrets.token_hex(8),
        )
        self.tokens[token] = subject
        self.refreshes[refresh] = subject
        payload = (
            base64.urlsafe_b64encode(
                json.dumps(
                    {"sub": subject, "nonce": "wrong" if self.bad_nonce else nonce}
                ).encode()
            )
            .rstrip(b"=")
            .decode()
        )
        pair = {
            "access_token": token,
            "refresh_token": refresh,
            "expires_in": 3600 if self.bad_ttl is None else self.bad_ttl,
            "token_type": "Wrong" if self.bad_token_type else "Bearer",
            "scope": "read" if subject == "service" else "openid profile",
            "id_token": "e30." + payload + ".stub",
        }
        if self.omit_scope:
            del pair["scope"]
        return pair


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def reply(self, status=200, body=None, location=None, cookie=None):
        raw = json.dumps(body or {}).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        if location:
            self.send_header("Location", location)
        if cookie:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        self.process()

    def do_POST(self):
        self.process()

    def process(self):
        length = int(self.headers.get("Content-Length", "0"))
        form = {k: v[0] for k, v in parse_qs(self.rfile.read(length).decode()).items()}
        parsed = urlsplit(self.path)
        query = {k: v[0] for k, v in parse_qs(parsed.query).items()}
        cookie = SimpleCookie(self.headers.get("Cookie", ""))
        sid = cookie["SID"].value if "SID" in cookie else ""
        auth = self.headers.get("Authorization", "")
        s = self.server
        with s.state_lock:
            s.calls.append((parsed.path, dict(form), sid))
            if s.rate_limited:
                return self.reply(429)
            if parsed.path == "/.well-known/openid-configuration":
                if s.trickle:
                    self.send_response(200)
                    self.send_header("Content-Length", "100000")
                    self.end_headers()
                    try:
                        for _ in range(1000):
                            self.wfile.write(b" ")
                            self.wfile.flush()
                            time.sleep(0.01)
                    except (OSError, ConnectionError):
                        return
                    return
                return self.reply(
                    body={"issuer": "fixture", "jwks_uri": "fixture/jwks"}
                )
            if parsed.path == "/oauth2/login":
                if form.get("password") != "password-secret" or not form.get(
                    "code_challenge"
                ):
                    return self.reply(400)
                sid = secrets.token_hex(8)
                s.sessions[sid] = form["username"]
                return self.reply(
                    body={"code": "initial-code"},
                    cookie="SID=" + sid + "; HttpOnly; Path=/",
                )
            if parsed.path == "/oauth2/authorize":
                if sid not in s.sessions:
                    return self.reply(401)
                nonce = secrets.token_hex(8)
                s.nonces[nonce] = (sid, query)
                values = {k: query[k] for k in ("client_id", "redirect_uri", "state")}
                values.update(user_id=s.sessions[sid], consent_csrf=nonce)
                location = (
                    ("https://evil.invalid/consent" if s.bad_origin else "/consent")
                    + "?"
                    + urlencode(values)
                )
                return self.reply(302, location=location)
            if parsed.path == "/oauth2/consent":
                entry = s.nonces.pop(form.get("consent_csrf", ""), None)
                if (
                    entry is None
                    or entry[0] != sid
                    or form.get("user_id") != s.sessions.get(sid)
                ):
                    return self.reply(400)
                query = entry[1]
                if (
                    form.get("state") != query["state"]
                    or form.get("action") != "approve"
                ):
                    return self.reply(400)
                code = secrets.token_hex(8)
                s.codes[code] = (
                    s.sessions[sid],
                    query["code_challenge"],
                    query["redirect_uri"],
                    query.get("nonce"),
                )
                return self.reply(
                    302,
                    location=query["redirect_uri"]
                    + "?"
                    + urlencode({"state": query["state"], "code": code}),
                )
            if parsed.path == "/oauth2/token":
                grant = form.get("grant_type")
                if grant == "authorization_code":
                    code = s.codes.pop(form.get("code", ""), None)
                    challenge = (
                        base64.urlsafe_b64encode(
                            hashlib.sha256(
                                form.get("code_verifier", "").encode()
                            ).digest()
                        )
                        .rstrip(b"=")
                        .decode()
                    )
                    if (
                        not code
                        or code[1] != challenge
                        or code[2] != form.get("redirect_uri")
                    ):
                        return self.reply(400)
                    return self.reply(body=s.mint(code[0], code[3]))
                if grant == "client_credentials":
                    if (
                        auth
                        != "Basic "
                        + base64.b64encode(b"service:client-secret").decode()
                    ):
                        return self.reply(401)
                    return self.reply(body=s.mint("service"))
                if grant == "refresh_token":
                    subject = s.refreshes.pop(form.get("refresh_token", ""), None)
                    if not subject:
                        return self.reply(400)
                    pair = s.mint(subject)
                    if s.drop_refresh_response:
                        self.close_connection = True
                        self.connection.shutdown(socket.SHUT_RDWR)
                        return
                    return self.reply(body=pair)
                return self.reply(400)
            if parsed.path == "/oauth2/userinfo":
                subject = s.tokens.get(auth.removeprefix("Bearer "))
                return (
                    self.reply(
                        200, body={"sub": "wrong-user" if s.bad_subject else subject}
                    )
                    if subject
                    else self.reply(401)
                )
            if parsed.path == "/oauth2/introspect":
                active = form.get("token") in s.tokens and not s.inactive
                return self.reply(
                    body={
                        "active": active,
                        "client_id": "wrong-client" if s.bad_client else "service",
                    }
                )
            return self.reply(404)


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.server = ProtocolServer()
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.target = "http://127.0.0.1:" + str(self.server.server_port)
        self.fixture = {
            "public_client": {
                "id": "portal",
                "redirect_uri": "http://127.0.0.1:5173/callback",
                "scope": "openid profile",
            },
            "confidential_client": {
                "id": "service",
                "secret": "client-secret",
                "scope": "read",
            },
        }
        self.sessions = []

    def user(self, name="user-1", record=None):
        session = Session(self.target, record=record)
        self.sessions.append(session)
        return User(
            session,
            {"username": name, "password": "password-secret", "expected_sub": name},
            self.fixture,
        )

    def tearDown(self):
        for s in self.sessions:
            s.close()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def test_password_consent_pkce_and_single_use_refresh(self):
        user = self.user()
        user.prepare()
        old = user.refresh
        user.rotate()
        self.assertNotEqual(old, user.refresh)
        self.assertNotIn(old, self.server.refreshes)
        paths = [x[0] for x in self.server.calls]
        self.assertEqual(
            paths[:5],
            [
                "/oauth2/login",
                "/oauth2/authorize",
                "/oauth2/consent",
                "/oauth2/token",
                "/oauth2/userinfo",
            ],
        )
        self.assertEqual(user.subject, "user-1")

    def test_two_vus_do_not_share_cookies_or_rt(self):
        a, b = self.user("a"), self.user("b")
        errors = []

        def work(u):
            try:
                u.prepare()
                u.rotate()
            except Exception as exc:  # noqa: BLE001 - preserve worker/cleanup failures in evidence
                errors.append(type(exc).__name__)

        threads = [threading.Thread(target=work, args=(u,)) for u in (a, b)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=5)
        self.assertEqual(errors, [])
        self.assertEqual((a.subject, b.subject), ("a", "b"))
        self.assertNotEqual(a.refresh, b.refresh)
        self.assertNotEqual(a.session.cookies, b.session.cookies)

    def test_inactive_http_200_is_protocol_failure(self):
        user = self.user()
        user.prepare()
        self.server.inactive = True
        with self.assertRaises(ProtocolError) as caught:
            user.introspect()
        self.assertEqual(caught.exception.kind, "inactive_token")

    def test_429_is_not_success(self):
        user = self.user()
        self.server.rate_limited = True
        with self.assertRaises(ProtocolError) as caught:
            user.credentials()
        self.assertEqual(caught.exception.kind, "rate_limited")

    def test_untrusted_consent_redirect_never_reaches_exchange(self):
        user = self.user()
        self.server.bad_origin = True
        with self.assertRaises(ProtocolError) as caught:
            user.authorize_consent()
        self.assertEqual(caught.exception.kind, "untrusted_consent_redirect")
        self.assertNotIn("/oauth2/consent", [x[0] for x in self.server.calls])
        self.assertNotIn("/oauth2/token", [x[0] for x in self.server.calls])

    def test_lost_rotation_response_does_not_replay_old_rt(self):
        user = self.user()
        user.prepare()
        old = user.refresh
        self.server.drop_refresh_response = True
        with self.assertRaises(ProtocolError):
            user.rotate()
        self.assertTrue(user.refresh_uncertain)
        self.assertEqual(user.refresh, "")
        before = len(self.server.calls)
        with self.assertRaises(ProtocolError):
            user.rotate()
        self.assertEqual(before, len(self.server.calls))
        self.assertEqual(
            sum(f.get("refresh_token") == old for _, f, _ in self.server.calls), 1
        )

    def test_wrong_subject_is_rejected_before_baseline_learning(self):
        user = self.user()
        self.server.bad_subject = True
        with self.assertRaises(ProtocolError) as caught:
            user.prepare()
        self.assertEqual(caught.exception.kind, "subject_mismatch")

    def test_wrong_introspection_client_is_rejected(self):
        user = self.user()
        user.prepare()
        self.server.bad_client = True
        with self.assertRaises(ProtocolError) as caught:
            user.introspect()
        self.assertEqual(caught.exception.kind, "client_binding_mismatch")

    def test_token_ttl_and_type_are_validated(self):
        user = self.user()
        for value in (0, -1, True):
            self.server.bad_ttl = value
            with self.assertRaises(ProtocolError) as caught:
                user.credentials()
            self.assertEqual(caught.exception.kind, "token_shape")
        self.server.bad_ttl = None
        self.server.bad_token_type = True
        with self.assertRaises(ProtocolError):
            user.credentials()

    def test_raw_close_failure_still_saves_incomplete_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = self.runner(tmp)
            run.args.drain = 2
            original = run.writer.close

            def broken_close():
                original()
                raise OSError("injected close failure")

            run.writer.close = broken_close
            with self.assertRaises(OSError):
                run.execute()
            result = json.loads((run.out / "result.json").read_text())
            self.assertEqual(result["status"], "incomplete")
            self.assertIn("raw_close_OSError", result["incomplete_reasons"])

    def test_oidc_nonce_is_correlated(self):
        self.server.bad_nonce = True
        with self.assertRaises(ProtocolError) as caught:
            self.user().authorize_consent()
        self.assertEqual(caught.exception.kind, "id_token_nonce_or_subject")

    def test_standalone_boundary_request_has_absolute_deadline(self):
        self.server.trickle = True
        client = Session(self.target, timeout=0.04)
        started = time.monotonic()
        try:
            with self.assertRaises(ProtocolError) as caught:
                client.request(
                    "boundary_metrics", "GET", "/.well-known/openid-configuration"
                )
            self.assertEqual(caught.exception.kind, "timeout")
            self.assertLess(time.monotonic() - started, 1)
        finally:
            client.close()

    def test_omitted_unchanged_scope_is_valid(self):
        self.server.omit_scope = True
        user = self.user()
        user.prepare()
        self.assertTrue(user.access)

    def runner(self, tmp, scenario="discovery", cache=False, kind="smoke"):
        from types import SimpleNamespace

        from run import Run

        fixture = {
            **self.fixture,
            "owner": "fulla-capacity-s07",
            "owner_nonce": "unit-owner",
            "target": self.target,
            "port": self.server.server_port,
            "redis_port": 1,
            "redis_password": "",
            "profile": "default",
            "cache_enabled": cache,
            "accounts": [
                {
                    "username": "user-1",
                    "password": "password-secret",
                    "expected_sub": "user-1",
                }
            ],
        }
        p = Path(tmp) / "fixture.json"
        p.write_text(json.dumps(fixture))
        args = SimpleNamespace(
            target=None,
            fixture=p,
            output=Path(tmp) / "run",
            scenario=scenario,
            kind=kind,
            vus=1,
            warmup=0,
            duration=0.1,
            timeout=1,
            drain=0.2,
            max_attempts=10000,
            reserve_mb=1,
            production_pacing=False,
            seed=7,
            budget_plan=None,
        )
        return Run(args)

    def test_missing_cpu_quota_is_explicit_in_smoke_but_blocks_pilot(self):
        from unittest.mock import patch

        from run import cpu_quota_cores

        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / "missing-cpu.max"
            with patch(
                "run.cpu_quota_cores",
                side_effect=lambda *, required: cpu_quota_cores(
                    required=required, quota_path=missing
                ),
            ):
                run = self.runner(tmp)
                result = run.execute()
                self.assertEqual(result["status"], "passed")
                cpu = result["measurement_driver"]
                self.assertFalse(cpu["cpu_quota_available"])
                self.assertIsNone(cpu["cgroup_budget_cores"])
                self.assertIsNone(cpu["budget_fraction"])
                before = len(self.server.calls)
                with self.assertRaises(FileNotFoundError):
                    self.runner(tmp, kind="pilot")
                self.assertEqual(len(self.server.calls), before)

    def test_trickle_body_absolute_deadline_and_finite_drain(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = self.runner(tmp)
            self.server.trickle = True
            original_prepare = run.users[0].prepare

            def prepare_then_bound_measurement():
                original_prepare()
                run.users[0].session.timeout = 0.04

            run.users[0].prepare = prepare_then_bound_measurement
            start = time.monotonic()
            result = run.execute()
            self.assertLess(time.monotonic() - start, 2)
            self.assertEqual(result["status"], "completed_with_failures")
            self.assertGreater(result["result"]["http"]["errors"].get("timeout", 0), 0)
            self.assertEqual(result["result"]["flow"]["missing_completions"], 0)

    def test_control_failure_before_http_is_a_failed_flow(self):
        from unittest.mock import patch

        with tempfile.TemporaryDirectory() as tmp:
            run = self.runner(tmp, "introspect_cold", True)
            with (
                patch.object(run, "metrics", return_value={"hit": 0, "miss": 1}),
                patch("run.Redis.evict_token", side_effect=OSError("failed")),
            ):
                result = run.execute()
            flow = result["result"]["flow"]
            self.assertGreater(flow["cohort_failure"], 0)
            self.assertEqual(flow["cohort_admissions"], flow["cohort_attempts"])
            self.assertEqual(
                result["result"]["scenarios"]["introspect_cold"]["success"], 0
            )
            self.assertNotEqual(result["status"], "passed")

    def test_observer_initialization_failure_still_writes_result(self):
        from observe import Sampler

        with tempfile.TemporaryDirectory() as tmp:
            run = self.runner(tmp)
            run.telemetry = Sampler({}, Path(tmp) / "missing" / "observer.gz")
            with self.assertRaises(RuntimeError):
                run.execute()
            result = json.loads((run.out / "result.json").read_text())
            self.assertEqual(result["status"], "incomplete")
            self.assertIn("observer_incomplete", result["incomplete_reasons"])

    def test_observer_shutdown_failure_does_not_drop_result(self):
        from unittest.mock import Mock

        with tempfile.TemporaryDirectory() as tmp:
            run = self.runner(tmp)
            observer = Mock()
            observer.thread.ident = 1
            observer.thread.is_alive.return_value = False
            observer.session = None
            observer.samples = 2
            observer.errors = 0
            observer.failure_type = None
            observer.stop.side_effect = RuntimeError("failed")
            run.telemetry = observer
            result = run.execute()
            self.assertEqual(result["status"], "incomplete")
            self.assertTrue((run.out / "result.json").exists())

    def test_target_mismatch_rejected_before_credentials(self):
        from run import Run

        with tempfile.TemporaryDirectory() as tmp:
            run = self.runner(tmp)
            run.writer.close()
            run.args.output = Path(tmp) / "other"
            run.args.target = "http://127.0.0.1:1"
            with self.assertRaisesRegex(ValueError, "target mismatch"):
                Run(run.args)

    def test_raw_record_contains_no_credentials(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "raw.gz"
            r = Recorder(p)

            def record(stage, start, end, status, error):
                r.write(
                    {
                        "type": "http",
                        "phase": "measure",
                        "vu": 0,
                        "stage": stage,
                        "start_ns": start,
                        "end_ns": end,
                        "status": status,
                        "error": error,
                    }
                )

            user = self.user(record=record)
            user.prepare()
            r.close()
            with gzip.open(p, "rt") as raw_file:
                raw = raw_file.read()
            for secret in (
                user.access,
                user.refresh,
                "password-secret",
                "client-secret",
                "SID=",
            ):
                self.assertNotIn(secret, raw)
            self.assertGreater(len(raw.splitlines()), 5)


class MeasurementTests(unittest.TestCase):
    def test_cpu_quota_budget_keeps_the_real_limit_and_rejects_bad_data(self):
        from run import cpu_quota_cores

        with tempfile.TemporaryDirectory() as tmp:
            quota = Path(tmp) / "cpu.max"
            quota.write_text("200000 100000\n")
            self.assertEqual(cpu_quota_cores(required=True, quota_path=quota), 2)
            quota.write_text("invalid")
            with self.assertRaises(ValueError):
                cpu_quota_cores(required=False, quota_path=quota)

    def test_start_cohort_tail_and_window_denominators(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "raw.gz"
            r = Recorder(p)
            for phase, start, end, error in [
                ("warmup", 0, 2_000_000_000, ""),
                ("measure", 1_000_000_000, 2_000_000_000, ""),
                ("measure", 2_000_000_000, 4_000_000_000, ""),
                ("measure", 2_500_000_000, 5_000_000_000, "timeout"),
                ("measure", 3_100_000_000, 3_200_000_000, ""),
            ]:
                r.write(
                    {
                        "type": "http",
                        "phase": phase,
                        "vu": 0,
                        "stage": "test",
                        "start_ns": start,
                        "end_ns": end,
                        "status": 200 if not error else 0,
                        "error": error,
                    }
                )
            r.close()
            s = summarize(p, 1_000_000_000, 3_000_000_000)["http"]
            self.assertEqual(s["cohort_attempts"], 3)
            self.assertEqual(s["window_completion_qps"], 0.5)
            self.assertEqual(s["cohort_success_per_wall_second"], 0.5)
            self.assertEqual(s["cohort_failure"], 1)
            self.assertEqual(s["latency_ns_all_attempts"]["p99"], 2_500_000_000)
            self.assertEqual(s["drain_only_events"], 1)

    def test_evidence_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "raw.gz"
            r = Recorder(p)
            r.close()
            with self.assertRaises(FileExistsError):
                Recorder(p)

    def test_record_schema_rejects_secret_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = Recorder(Path(tmp) / "raw.gz")
            with self.assertRaises(ValueError):
                r.write({"token": "do-not-record"})
            r.close()

    def test_all_http_including_observer_counts_for_pacing(self):
        now = [0.0]
        p = Pacer(
            True, clock=lambda: now[0], sleep=lambda n: now.__setitem__(0, now[0] + n)
        )
        for _ in range(8):
            p("/metrics")
        p("/health/ready")
        self.assertGreaterEqual(now[0], 60)
        p("/oauth2/login")
        before = now[0]
        p("/oauth2/login")
        self.assertGreaterEqual(now[0] - before, 60)

    def test_jsonc_preserves_urls_and_reads_actual_production_policy(self):
        from support import load_config

        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "config.json"
            p.write_text(
                '{//comment\n"url":"http://localhost/path",/*block*/"x":"quoted\\"value"}'
            )
            self.assertEqual(load_config(p)["url"], "http://localhost/path")
        root = Path(__file__).resolve().parents[2]
        prod = load_config(root / "apps/server/config/config.prod.json")
        hodor = next(
            p["config"] for p in prod["plugins"] if p["name"] == "drogon::plugin::Hodor"
        )
        self.assertEqual(hodor["ip_capacity"], 30)
        self.assertTrue(hodor["sub_limits"])

    def test_p99_keeps_worst_sample(self):
        self.assertEqual(quantile([1] * 99 + [10000], 0.99), 1)
        self.assertEqual(quantile([1] * 98 + [5000, 10000], 0.99), 5000)


if __name__ == "__main__":
    unittest.main()
