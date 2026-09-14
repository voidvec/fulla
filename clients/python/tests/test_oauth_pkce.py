"""Unit tests for the authorization-code helpers and PKCE (matrix P6-P7)."""

from __future__ import annotations

from urllib.parse import parse_qs, urlparse

import pytest
from conftest import decode_basic, oauth_token_response

from fulla import AuthorizationCodeFlow, FullaAuthError, PkcePair, create_pkce_challenge
from fulla.oauth import parse_authorization_response


class TestPkce:
    def test_rfc7636_appendix_b_vector(self):
        """P6 -- the canonical S256 test vector from RFC 7636 appendix B."""
        verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
        assert create_pkce_challenge(verifier) == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"

    def test_generated_pair_shape(self):
        pair = PkcePair.generate()
        assert 43 <= len(pair.verifier) <= 128
        assert pair.challenge == create_pkce_challenge(pair.verifier)
        assert PkcePair.generate().verifier != pair.verifier  # non-deterministic


class TestBuildAuthorizeUrl:
    def _flow(self) -> AuthorizationCodeFlow:
        return AuthorizationCodeFlow(
            "http://server.test/",
            "fulla-portal",
            redirect_uri="http://client.test/cb",
            scopes=["openid", "profile"],
        )

    def test_params_complete(self):
        """P7 -- every required authorize parameter present."""
        pkce = PkcePair.generate()
        url = self._flow().build_authorize_url(state="st4te", pkce=pkce)
        parsed = urlparse(url)
        assert parsed.scheme == "http"
        assert parsed.netloc == "server.test"
        assert parsed.path == "/oauth2/authorize"
        q = parse_qs(parsed.query)
        assert q["response_type"] == ["code"]
        assert q["client_id"] == ["fulla-portal"]
        assert q["redirect_uri"] == ["http://client.test/cb"]
        assert q["scope"] == ["openid profile"]
        assert q["state"] == ["st4te"]
        assert q["code_challenge"] == [pkce.challenge]
        assert q["code_challenge_method"] == ["S256"]

    def test_optional_bits_omitted(self):
        url = self._flow().build_authorize_url()
        q = parse_qs(urlparse(url).query)
        assert "state" not in q
        assert "code_challenge" not in q


class TestExchangeAndRefresh:
    def _route_token_success(self, fake):
        fake.on("POST", "/oauth2/token", json_body=oauth_token_response("at-exchanged", "rt-rotated"))

    def test_exchange_code_confidential_uses_basic(self, fake, transport):
        """F-017 mirror: confidential exchange carries HTTP Basic, not body creds."""
        self._route_token_success(fake)
        flow = AuthorizationCodeFlow(
            "http://server.test", "backend-svc", "test-secret",
            redirect_uri="http://client.test/cb", transport=transport,
        )
        tokens = flow.exchange_code("auth-code-1", code_verifier="ver-ifr")
        assert tokens.access_token == "at-exchanged"
        assert tokens.refresh_token == "rt-rotated"
        assert decode_basic(fake.basic_on_token_requests[0]) == "backend-svc:test-secret"
        body = fake.token_form()
        assert body["grant_type"] == "authorization_code"
        assert body["code"] == "auth-code-1"
        assert body["code_verifier"] == "ver-ifr"
        assert "client_secret" not in body  # must not leak into the form

    def test_exchange_code_public_client_identifies_in_form(self, fake, transport):
        self._route_token_success(fake)
        flow = AuthorizationCodeFlow(
            "http://server.test", "fulla-portal",
            redirect_uri="http://client.test/cb", transport=transport,
        )
        tokens = flow.exchange_code("auth-code-2")
        assert tokens.access_token == "at-exchanged"
        assert all(not h.startswith("Basic ") for h in fake.basic_on_token_requests)
        assert fake.token_form()["client_id"] == "fulla-portal"

    def test_refresh_rotates(self, fake, transport):
        self._route_token_success(fake)
        flow = AuthorizationCodeFlow(
            "http://server.test", "backend-svc", "test-secret", transport=transport
        )
        tokens = flow.refresh("rt-old")
        assert tokens.refresh_token == "rt-rotated"
        body = fake.token_form()
        assert body["grant_type"] == "refresh_token"
        assert body["refresh_token"] == "rt-old"
        assert decode_basic(fake.basic_on_token_requests[0]) == "backend-svc:test-secret"

    def test_exchange_error_raises_auth_error(self, fake, transport):
        fake.on("POST", "/oauth2/token", status=400,
                json_body={"error": "invalid_grant", "error_description": "code expired"})
        flow = AuthorizationCodeFlow(
            "http://server.test", "backend-svc", "test-secret", transport=transport
        )
        with pytest.raises(FullaAuthError) as excinfo:
            flow.exchange_code("stale")
        assert excinfo.value.error == "invalid_grant"


class TestParseAuthorizationResponse:
    def test_extracts_code_and_state(self):
        code, state, error = parse_authorization_response(
            "http://client.test/cb?code=abc&state=st4te"
        )
        assert (code, state, error) == ("abc", "st4te", None)

    def test_extracts_error(self):
        code, state, error = parse_authorization_response(
            "http://client.test/cb?error=access_denied&state=st4te"
        )
        assert code is None
        assert error == "access_denied"
        assert state == "st4te"
