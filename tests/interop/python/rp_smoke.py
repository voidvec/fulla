#!/usr/bin/env python3
"""P0-9 (#263): authlib RP smoke against fulla.

Stages: authorize-URL construction (authlib OAuth2Client) -> json-mode
first-factor login -> fetch_token (token endpoint interop, PKCE threaded) ->
userinfo scope gating (profile claims present) -> refresh_token (rotation).
Any non-2xx or unexpected shape fails the run.

SSRF boundary: this harness deliberately talks to the LOCAL fulla stack
only. validate_base() admits http(s) URLs whose host is loopback
(127.0.0.1 / localhost / ::1), plus exactly one opt-in extra host for CI
containers (FULLA_INTEROP_ALLOW_HOST). Everything else is refused before
any request is issued.
"""

import base64
import hashlib
import ipaddress
import json
import os
import secrets
import socket
import sys
import urllib.error
import urllib.parse
import urllib.request

import requests
from authlib.integrations.requests_client import OAuth2Session

BASE = os.environ.get("FULLA_BASE_URL", "http://localhost:5555")
CLIENT_ID = os.environ.get("FULLA_RP_CLIENT_ID", "fulla-portal")
REDIRECT_URI = os.environ.get("FULLA_RP_REDIRECT_URI", "http://localhost:5173/callback")
SCOPE = os.environ.get("FULLA_RP_SCOPE", "openid profile email")
USER = os.environ.get("FULLA_RP_USER", "admin")
PASSWORD = os.environ.get("FULLA_RP_PASSWORD", "admin")
ALLOW_HOST = os.environ.get("FULLA_INTEROP_ALLOW_HOST", "")


def fail(stage: str, detail) -> None:
    print(f"[authlib] FAIL {stage}: {detail}", file=sys.stderr)
    sys.exit(1)


def ok(stage: str) -> None:
    print(f"[authlib] ok   {stage}")


def validate_base(base: str) -> str:
    """Only loopback (or the single opted-in CI host) may be fetched."""
    parsed = urllib.parse.urlparse(base)
    if parsed.scheme not in ("http", "https"):
        fail("base-url guard", f"scheme {parsed.scheme!r} not allowed")
    host = parsed.hostname or ""
    allowed = {"127.0.0.1", "localhost", "::1"}
    if host not in allowed:
        if ALLOW_HOST and host == ALLOW_HOST:
            return base
        # Resolve and refuse anything that is not loopback (DNS names can
        # hide internal targets).
        try:
            infos = socket.getaddrinfo(host, None)
        except OSError:
            fail("base-url guard", f"host {host!r} is neither loopback nor opted in")
        for info in infos:
            addr = ipaddress.ip_address(info[4][0])
            if not addr.is_loopback:
                fail("base-url guard", f"host {host!r} resolves to non-loopback {addr}")
        return base
    return base


BASE = validate_base(BASE)


def http_form(url: str, fields: dict) -> tuple[int, dict]:
    data = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(
        url, data=data, headers={"content-type": "application/x-www-form-urlencoded"}
    )
    try:
        with urllib.request.urlopen(req) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8"))


def main() -> None:
    metadata = json.loads(
        urllib.request.urlopen(f"{BASE}/.well-known/openid-configuration").read().decode()
    )
    ok("discovery")

    verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode()
    challenge = base64.urlsafe_b64encode(
        hashlib.sha256(verifier.encode()).digest()
    ).rstrip(b"=").decode()

    client = OAuth2Session(
        CLIENT_ID,
        redirect_uri=REDIRECT_URI,
        scope=SCOPE,
        code_challenge=challenge,
        code_challenge_method="S256",
    )
    authz_url, state = client.create_authorization_url(metadata["authorization_endpoint"])
    ok("authorization url")

    q = dict(urllib.parse.parse_qsl(urllib.parse.urlparse(authz_url).query))
    # authlib keeps the PKCE pair OUT of the authorization URL (it holds it
    # for fetch_token), but fulla's login form must thread the challenge onto
    # the issued code — pass ours explicitly.
    status, login = http_form(
        f"{BASE}/oauth2/login?json=true",
        {
            "username": USER,
            "password": PASSWORD,
            "client_id": q.get("client_id", CLIENT_ID),
            "redirect_uri": q.get("redirect_uri", REDIRECT_URI),
            "scope": q.get("scope", SCOPE),
            "state": q["state"],
            "nonce": q.get("nonce", ""),
            "code_challenge": challenge,
            "code_challenge_method": "S256",
        },
    )
    if status != 200 or not login.get("code"):
        fail("login", f"status {status}: {json.dumps(login)[:300]}")
    ok("login (code issued)")

    token = client.fetch_token(
        metadata["token_endpoint"],
        code=login["code"],
        state=state,
        code_verifier=verifier,
    )
    if "access_token" not in token or "id_token" not in token:
        fail("fetch_token", f"incomplete token: {sorted(token)}")
    ok("fetch_token (id_token + access_token + refresh_token)")

    req = urllib.request.Request(
        f"{BASE}/oauth2/userinfo",
        headers={"authorization": f"Bearer {token['access_token']}"},
    )
    with urllib.request.urlopen(req) as resp:
        userinfo = json.loads(resp.read().decode())
    # P0-2 gating: profile/email scopes requested -> claims present.
    for claim in ("sub", "name", "preferred_username", "email", "email_verified"):
        if claim not in userinfo:
            fail("userinfo", f"missing claim {claim} (scope {SCOPE})")
    ok(f"userinfo (sub={userinfo['sub'][:8]}…, preferred_username={userinfo['preferred_username']})")

    refreshed = client.refresh_token(
        metadata["token_endpoint"], refresh_token=token["refresh_token"]
    )
    if "access_token" not in refreshed:
        fail("refresh", f"no access_token: {sorted(refreshed)}")
    ok("refresh_token (rotation)")

    print("[authlib] ALL STAGES GREEN")


if __name__ == "__main__":
    main()
