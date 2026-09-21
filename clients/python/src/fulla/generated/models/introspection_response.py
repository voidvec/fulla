from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="IntrospectionResponse")


@_attrs_define
class IntrospectionResponse:
    """RFC 7662 introspection body. Inactive tokens return {"active": false} only. Active responses always carry iss
    (backfilled from config); Cache-Control: no-store.

        Attributes:
            active (bool):
            client_id (str | Unset):
            token_type (str | Unset): Always "Bearer" when active.
            exp (int | Unset):
            iat (int | Unset):
            nbf (int | Unset):
            sub (str | Unset): Resource-owner subject; "client:<id>" for client_credentials tokens.
            aud (str | Unset):
            iss (str | Unset):
            scope (str | Unset): Space-separated scopes.
            org_id (int | Unset): v1.5.0: present when the token was issued in org context (authorize org_id parameter). The
                token's own binding; roles/name stay userinfo-only.
    """

    active: bool
    client_id: str | Unset = UNSET
    token_type: str | Unset = UNSET
    exp: int | Unset = UNSET
    iat: int | Unset = UNSET
    nbf: int | Unset = UNSET
    sub: str | Unset = UNSET
    aud: str | Unset = UNSET
    iss: str | Unset = UNSET
    scope: str | Unset = UNSET
    org_id: int | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        active = self.active

        client_id = self.client_id

        token_type = self.token_type

        exp = self.exp

        iat = self.iat

        nbf = self.nbf

        sub = self.sub

        aud = self.aud

        iss = self.iss

        scope = self.scope

        org_id = self.org_id

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "active": active,
            }
        )
        if client_id is not UNSET:
            field_dict["client_id"] = client_id
        if token_type is not UNSET:
            field_dict["token_type"] = token_type
        if exp is not UNSET:
            field_dict["exp"] = exp
        if iat is not UNSET:
            field_dict["iat"] = iat
        if nbf is not UNSET:
            field_dict["nbf"] = nbf
        if sub is not UNSET:
            field_dict["sub"] = sub
        if aud is not UNSET:
            field_dict["aud"] = aud
        if iss is not UNSET:
            field_dict["iss"] = iss
        if scope is not UNSET:
            field_dict["scope"] = scope
        if org_id is not UNSET:
            field_dict["org_id"] = org_id

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        active = d.pop("active")

        client_id = d.pop("client_id", UNSET)

        token_type = d.pop("token_type", UNSET)

        exp = d.pop("exp", UNSET)

        iat = d.pop("iat", UNSET)

        nbf = d.pop("nbf", UNSET)

        sub = d.pop("sub", UNSET)

        aud = d.pop("aud", UNSET)

        iss = d.pop("iss", UNSET)

        scope = d.pop("scope", UNSET)

        org_id = d.pop("org_id", UNSET)

        introspection_response = cls(
            active=active,
            client_id=client_id,
            token_type=token_type,
            exp=exp,
            iat=iat,
            nbf=nbf,
            sub=sub,
            aud=aud,
            iss=iss,
            scope=scope,
            org_id=org_id,
        )

        introspection_response.additional_properties = d
        return introspection_response

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
