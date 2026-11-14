"""Stateful benchmark protocol; one instance and credential chain per VU."""

from __future__ import annotations

import base64
import hashlib
import http.client
import json
import math
import secrets
import socket
import ssl
import threading
import time
from http.cookies import SimpleCookie
from urllib.parse import parse_qs, urlencode, urljoin, urlsplit


class _Deadlines:
    """One fixed observer per process also covers setup and boundary Sessions."""

    def __init__(self):
        self.condition = threading.Condition()
        self.sessions = set()
        self.thread = None

    def add(self, session):
        with self.condition:
            self.sessions.add(session)
            if self.thread is None:
                self.thread = threading.Thread(
                    target=self.watch, daemon=True, name="s07-http-deadlines"
                )
                self.thread.start()
            self.condition.notify()

    def remove(self, session):
        with self.condition:
            self.sessions.discard(session)

    def watch(self):
        while True:
            with self.condition:
                self.condition.wait_for(lambda: bool(self.sessions))
                sessions = tuple(self.sessions)
            for session in sessions:
                session.interrupt(expired_only=True)
            time.sleep(0.02)


_deadlines = _Deadlines()


class ProtocolError(Exception):
    def __init__(self, kind: str, stage: str, status: int = 0):
        self.kind, self.stage, self.status = kind, stage, status
        super().__init__(f"{stage}: {kind} (HTTP {status})")


def pkce() -> tuple[str, str]:
    verifier = secrets.token_urlsafe(32)
    challenge = (
        base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest())
        .rstrip(b"=")
        .decode()
    )
    return verifier, challenge


class Session:
    def __init__(self, target: str, timeout: float = 5, record=None, pace=None):
        url = urlsplit(target)
        if (
            url.scheme not in ("http", "https")
            or url.username
            or url.password
            or url.query
            or url.fragment
            or url.path not in ("", "/")
        ):
            raise ValueError("target must be an HTTP(S) origin")
        if url.hostname not in ("127.0.0.1", "localhost", "::1"):
            raise ValueError("capacity driver accepts loopback fixtures only")
        self.url = url
        self.timeout = timeout
        self.record = record
        self.pace = pace
        self.cookies: dict[str, str] = {}
        self.connection = None
        self.before_send = None
        self.record_start = None
        self.stage_prefix = ""
        self.state_lock = threading.Lock()
        self.active_connection = None
        self.active_socket = None
        self.deadline_ns = 0
        self.interrupted = False

    def close(self):
        if self.connection:
            self.connection.close()
            self.connection = None

    def interrupt(self, expired_only=False):
        with self.state_lock:
            if not self.active_connection:
                return
            if expired_only and time.perf_counter_ns() < self.deadline_ns:
                return
            self.interrupted = True
            sock = self.active_socket or self.active_connection.sock
        if sock:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def request(
        self,
        stage: str,
        method: str,
        path: str,
        *,
        form=None,
        data=None,
        bearer=None,
        basic=None,
        expected=(200,),
        check=None,
    ):
        if self.pace:
            self.pace(path)
        if self.before_send:
            self.before_send()
        stage = self.stage_prefix + stage
        started = time.perf_counter_ns()
        status, error = 0, ""
        if self.record_start:
            self.record_start(stage, started)
        try:
            if self.connection is None:
                cls = (
                    http.client.HTTPSConnection
                    if self.url.scheme == "https"
                    else http.client.HTTPConnection
                )
                self.connection = cls(
                    self.url.hostname, self.url.port, timeout=self.timeout
                )
            headers = {"Accept": "application/json"}
            body = None
            if form is not None:
                body = urlencode(form).encode()
                headers["Content-Type"] = "application/x-www-form-urlencoded"
            elif data is not None:
                body = json.dumps(data).encode()
                headers["Content-Type"] = "application/json"
            if self.cookies:
                headers["Cookie"] = "; ".join(
                    k + "=" + v for k, v in self.cookies.items()
                )
            if bearer:
                headers["Authorization"] = "Bearer " + bearer
            if basic:
                headers["Authorization"] = (
                    "Basic "
                    + base64.b64encode((basic[0] + ":" + basic[1]).encode()).decode()
                )
            connection = self.connection
            with self.state_lock:
                self.active_connection = connection
                self.active_socket = None
                self.deadline_ns = started + int(self.timeout * 1e9)
                self.interrupted = False
            _deadlines.add(self)
            connection.request(method, path, body=body, headers=headers)
            with self.state_lock:
                self.active_socket = connection.sock
                interrupted = self.interrupted
            if interrupted:
                raise ProtocolError("timeout", stage)
            response = connection.getresponse()
            status = response.status
            pairs = response.getheaders()
            raw = response.read(1024 * 1024 + 1)
            if self.interrupted:
                self.close()
                raise ProtocolError("timeout", stage, status)
            if len(raw) > 1024 * 1024:
                raise ProtocolError("response_limit", stage, status)
            for key, value in pairs:
                if key.lower() == "set-cookie":
                    parsed = SimpleCookie()
                    parsed.load(value)
                    for name, cookie in parsed.items():
                        if cookie["max-age"] == "0":
                            self.cookies.pop(name, None)
                        else:
                            self.cookies[name] = cookie.value
            if status not in expected:
                raise ProtocolError(
                    "rate_limited" if status == 429 else "http_status", stage, status
                )
            result = {
                "headers": {k.lower(): v for k, v in pairs},
                "body": raw,
                "status": status,
            }
            if check:
                check(result)
            if self.interrupted or time.perf_counter_ns() >= self.deadline_ns:
                raise ProtocolError("timeout", stage, status)
            return result
        except ProtocolError as exc:
            self.close()
            error = exc.kind
            raise
        except TimeoutError:
            error = "timeout"
            self.close()
            raise ProtocolError(error, stage, status) from None
        except (OSError, http.client.HTTPException, ssl.SSLError):
            error = "timeout" if self.interrupted else "transport"
            self.close()
            raise ProtocolError(error, stage, status) from None
        finally:
            _deadlines.remove(self)
            with self.state_lock:
                self.active_connection = self.active_socket = None
            if self.record:
                self.record(stage, started, time.perf_counter_ns(), status, error)


def json_body(response: dict, stage: str) -> dict:
    try:
        value = json.loads(response["body"])
        if not isinstance(value, dict):
            raise ValueError()  # noqa: TRY004 - mapped to a single redacted protocol error
        return value
    except (ValueError, UnicodeError):
        raise ProtocolError("json_shape", stage, response["status"]) from None


class User:
    def __init__(self, session: Session, account: dict, fixture: dict):
        self.session, self.account, self.fixture = session, account, fixture
        self.access = ""
        self.refresh = ""
        self.expires = 0.0
        self.subject = account.get("expected_sub", "")
        self.org_access = ""
        self.org_subject = ""
        self.cc_access = ""
        self.cc_issued = []
        self.refresh_uncertain = False

    def token(self, form: dict, *, client=None, rotating=False) -> dict:
        client = client or self.fixture["public_client"]
        form = {**form, "client_id": client["id"]}
        auth = (client["id"], client["secret"]) if client.get("secret") else None
        if rotating:
            # A transport error does not prove the old RT was unconsumed.
            self.refresh_uncertain = True
            self.refresh = ""

        def validate(response):
            value = json_body(response, "token")
            if (
                not isinstance(value.get("access_token"), str)
                or not value["access_token"]
                or type(value.get("expires_in")) not in (int, float)
                or not math.isfinite(value["expires_in"])
                or value["expires_in"] <= 0
                or str(value.get("token_type", "")).lower() != "bearer"
                or (
                    "scope" in value
                    and (
                        not isinstance(value["scope"], str)
                        or set(value["scope"].split())
                        != set(client.get("scope", "").split())
                    )
                )
            ):
                raise ProtocolError("token_shape", "token", response["status"])
            response["json"] = value

        response = self.session.request(
            "token", "POST", "/oauth2/token", form=form, basic=auth, check=validate
        )
        return response["json"]

    def accept_pair(self, pair: dict):
        refresh = pair.get("refresh_token")
        if not isinstance(refresh, str) or not refresh:
            raise ProtocolError("refresh_missing", "token", 200)
        self.access = pair["access_token"]
        self.refresh = refresh
        self.refresh_uncertain = False
        self.expires = time.monotonic() + pair["expires_in"]

    def login(self, client=None, org=None):
        client = client or self.fixture["public_client"]
        verifier, challenge = pkce()
        state = secrets.token_urlsafe(20)
        nonce = secrets.token_urlsafe(20)
        form = {
            "username": self.account["username"],
            "password": self.account["password"],
            "client_id": client["id"],
            "redirect_uri": client["redirect_uri"],
            "scope": client.get("scope", "openid profile"),
            "state": state,
            "code_challenge": challenge,
            "code_challenge_method": "S256",
            "nonce": nonce,
            "json": "true",
        }
        if org:
            form["org_id"] = org

        def validate(response):
            value = json_body(response, "login")
            if (
                not isinstance(value.get("code"), str)
                or not value["code"]
                or not self.session.cookies
            ):
                raise ProtocolError("login_shape", "login", response["status"])

        self.session.request(
            "login", "POST", "/oauth2/login", form=form, check=validate
        )
        return verifier, challenge, state, nonce

    def authorize_consent(self, client=None, org=None):
        client = client or self.fixture["public_client"]
        verifier, challenge, state, nonce = self.login(client, org)
        params = {
            "response_type": "code",
            "client_id": client["id"],
            "redirect_uri": client["redirect_uri"],
            "scope": client.get("scope", "openid profile"),
            "state": state,
            "prompt": "consent",
            "code_challenge": challenge,
            "code_challenge_method": "S256",
            "nonce": nonce,
        }
        if org:
            params["org_id"] = org
        response = self.session.request(
            "authorize",
            "GET",
            "/oauth2/authorize?" + urlencode(params),
            expected=(302,),
        )
        consent_location = urlsplit(
            urljoin(self.session.url.geturl(), response["headers"].get("location", ""))
        )
        allowed = urlsplit(
            urljoin(
                self.session.url.geturl(), self.fixture.get("consent_url", "/consent")
            )
        )
        if (
            consent_location.scheme,
            consent_location.netloc,
            consent_location.path,
        ) != (allowed.scheme, allowed.netloc, allowed.path):
            raise ProtocolError("untrusted_consent_redirect", "authorize", 302)
        query = parse_qs(consent_location.query)
        required = ("consent_csrf", "user_id", "client_id", "redirect_uri", "state")
        if any(len(query.get(k, [])) != 1 for k in required):
            raise ProtocolError("consent_redirect_shape", "authorize", 302)
        if (
            query["client_id"][0] != client["id"]
            or query["redirect_uri"][0] != client["redirect_uri"]
            or query["state"][0] != state
        ):
            raise ProtocolError("consent_context", "authorize", 302)
        if self.subject and query["user_id"][0] != self.subject:
            raise ProtocolError("consent_subject_mismatch", "authorize", 302)
        form = {
            **params,
            "user_id": query["user_id"][0],
            "consent_csrf": query["consent_csrf"][0],
            "action": "approve",
        }
        form.pop("response_type")
        form.pop("prompt")
        response = self.session.request(
            "consent", "POST", "/oauth2/consent", form=form, expected=(302,)
        )
        location = urlsplit(response["headers"].get("location", ""))
        wanted = urlsplit(client["redirect_uri"])
        query = parse_qs(location.query)
        if (
            (location.scheme, location.netloc, location.path)
            != (wanted.scheme, wanted.netloc, wanted.path)
            or query.get("state") != [state]
            or len(query.get("code", [])) != 1
        ):
            raise ProtocolError("code_redirect_context", "consent", 302)
        pair = self.token(
            {
                "grant_type": "authorization_code",
                "code": query["code"][0],
                "redirect_uri": client["redirect_uri"],
                "code_verifier": verifier,
            },
            client=client,
        )
        # Correlation check only, not a substitute for an RP's JWT signature verification.
        try:
            if not isinstance(pair.get("id_token"), str):
                raise ValueError()  # noqa: TRY004 - mapped to a single redacted protocol error
            part = pair["id_token"].split(".")[1]
            claims = json.loads(base64.urlsafe_b64decode(part + "=" * (-len(part) % 4)))
            if not isinstance(claims, dict):
                raise ValueError()  # noqa: TRY004 - mapped to a single redacted protocol error
            if claims.get("nonce") != nonce or (
                self.subject and claims.get("sub") != self.subject
            ):
                raise ValueError()
        except (KeyError, IndexError, ValueError, TypeError):
            raise ProtocolError("id_token_nonce_or_subject", "token", 200) from None
        return pair

    def userinfo(self, token=None, expected_subject=None, expected_org=None):
        def validate(response):
            value = json_body(response, "userinfo")
            if not isinstance(value.get("sub"), str) or not value["sub"]:
                raise ProtocolError("userinfo_shape", "userinfo", 200)
            if expected_subject and value["sub"] != expected_subject:
                raise ProtocolError("subject_mismatch", "userinfo", 200)
            if expected_org is not None and str(
                value.get("org_ctx", {}).get("org_id")
            ) != str(expected_org):
                raise ProtocolError("org_mismatch", "userinfo", 200)
            response["json"] = value

        response = self.session.request(
            "userinfo",
            "GET",
            "/oauth2/userinfo",
            bearer=token or self.access,
            check=validate,
        )
        return response["json"]

    def introspect(self, token=None, client=None, expected_org=None):
        client = client or self.fixture["confidential_client"]

        def validate(response):
            value = json_body(response, "introspect")
            if value.get("active") is not True:
                raise ProtocolError("inactive_token", "introspect", 200)
            if value.get("client_id") != client["id"]:
                raise ProtocolError("client_binding_mismatch", "introspect", 200)
            if expected_org is not None and str(value.get("org_id")) != str(
                expected_org
            ):
                raise ProtocolError("org_binding_mismatch", "introspect", 200)
            response["json"] = value

        return self.session.request(
            "introspect",
            "POST",
            "/oauth2/introspect",
            form={"token": token or self.cc_access},
            basic=(client["id"], client["secret"]),
            check=validate,
        )["json"]

    def credentials(self):
        client = self.fixture["confidential_client"]
        pair = self.token(
            {"grant_type": "client_credentials", "scope": client["scope"]},
            client=client,
        )
        self.cc_access = pair["access_token"]
        self.cc_issued.append(
            (
                hashlib.sha256(self.cc_access.encode()).hexdigest().upper(),
                time.monotonic() + pair["expires_in"],
            )
        )
        return pair

    def rotate(self):
        if not self.refresh or self.refresh_uncertain:
            raise ProtocolError("refresh_chain_uncertain", "refresh")
        old = self.refresh
        pair = self.token(
            {"grant_type": "refresh_token", "refresh_token": old}, rotating=True
        )
        self.accept_pair(pair)
        if pair["refresh_token"] == old:
            self.refresh = ""
            self.refresh_uncertain = True
            raise ProtocolError("refresh_not_rotated", "refresh", 200)

    def prepare(self):
        self.accept_pair(self.authorize_consent())
        value = self.userinfo(expected_subject=self.subject)
        self.subject = value["sub"]
        self.credentials()
        self.introspect()
        if self.account.get("org"):
            pair = self.authorize_consent(
                self.account["org_client"], self.account["org"]
            )
            self.org_access = pair["access_token"]
            self.org_subject = self.userinfo(
                self.org_access, self.subject, self.account["org_id"]
            ).get("sub", "")
            self.introspect(
                self.org_access, self.account["org_client"], self.account["org_id"]
            )
