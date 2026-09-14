from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="LoginRequest")


@_attrs_define
class LoginRequest:
    """/oauth2/login credentials (JSON body or form-encoded; query also accepted).

    Attributes:
        username (str): Account username or email.
        password (str):
        client_id (str | Unset): Matches the requesting app (required for code issuance).
        redirect_uri (str | Unset): Must match the registered client.
        scope (str | Unset): Space-separated (optional).
        state (str | Unset): Opaque value maintained between request and callback (recommended).
        code_challenge (str | Unset): PKCE code challenge (RFC 7636). REQUIRED for PUBLIC clients (fulla-portal, fulla-
            admin-console) when auth.require_pkce_for_public is enabled (default true, F-011 / RFC 9700 §2.1.1). The
            matching code_verifier must be sent on the /oauth2/token exchange.
        code_challenge_method (str | Unset): S256 (recommended) or plain.
        nonce (str | Unset): OIDC nonce (anti-replay), echoed in the id_token.
        json (str | Unset): Set to "true" to receive a JSON body instead of a 302 redirect.
    """

    username: str
    password: str
    client_id: str | Unset = UNSET
    redirect_uri: str | Unset = UNSET
    scope: str | Unset = UNSET
    state: str | Unset = UNSET
    code_challenge: str | Unset = UNSET
    code_challenge_method: str | Unset = UNSET
    nonce: str | Unset = UNSET
    json: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        username = self.username

        password = self.password

        client_id = self.client_id

        redirect_uri = self.redirect_uri

        scope = self.scope

        state = self.state

        code_challenge = self.code_challenge

        code_challenge_method = self.code_challenge_method

        nonce = self.nonce

        json = self.json

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "username": username,
                "password": password,
            }
        )
        if client_id is not UNSET:
            field_dict["client_id"] = client_id
        if redirect_uri is not UNSET:
            field_dict["redirect_uri"] = redirect_uri
        if scope is not UNSET:
            field_dict["scope"] = scope
        if state is not UNSET:
            field_dict["state"] = state
        if code_challenge is not UNSET:
            field_dict["code_challenge"] = code_challenge
        if code_challenge_method is not UNSET:
            field_dict["code_challenge_method"] = code_challenge_method
        if nonce is not UNSET:
            field_dict["nonce"] = nonce
        if json is not UNSET:
            field_dict["json"] = json

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        username = d.pop("username")

        password = d.pop("password")

        client_id = d.pop("client_id", UNSET)

        redirect_uri = d.pop("redirect_uri", UNSET)

        scope = d.pop("scope", UNSET)

        state = d.pop("state", UNSET)

        code_challenge = d.pop("code_challenge", UNSET)

        code_challenge_method = d.pop("code_challenge_method", UNSET)

        nonce = d.pop("nonce", UNSET)

        json = d.pop("json", UNSET)

        login_request = cls(
            username=username,
            password=password,
            client_id=client_id,
            redirect_uri=redirect_uri,
            scope=scope,
            state=state,
            code_challenge=code_challenge,
            code_challenge_method=code_challenge_method,
            nonce=nonce,
            json=json,
        )

        login_request.additional_properties = d
        return login_request

    @property
    def additional_keys(self) -> list[str]:
        return list(self.additional_properties.keys())

    def __getitem__(self, key: str) -> Any:
        return self.additional_properties[key]

    def __setitem__(self, key: str, value: Any) -> None:
        self.additional_properties[key] = value

    def __delitem__(self, key: str) -> None:
        del self.additional_properties[key]

    def __contains__(self, key: str) -> bool:
        return key in self.additional_properties
