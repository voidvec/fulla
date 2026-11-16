from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="UpstreamProvider")


@_attrs_define
class UpstreamProvider:
    """P2-1: one OIDC-in upstream registry row (public view; client_secret is never echoed).

    Attributes:
        provider_key (str): Lowercase [a-z0-9-]; keys URLs and oauth2_subject_mappings.provider; immutable after create.
        display_name (str):
        issuer (str): OIDC issuer; endpoints via discovery unless pinned in endpoint_overrides.
        client_id (str):
        enabled (bool):
        client_secret_set (bool | Unset):
        scopes (str | Unset):  Example: openid profile email.
        claims_mapping (str | Unset): JSON object mapping identity slots to upstream claim names; defaults
            {"sub":"sub","email":"email","username":"preferred_username","name":"name"}.
        endpoint_overrides (str | Unset): Optional discovery bypass {"authorize":url,"token":url,"userinfo":url}.
    """

    provider_key: str
    display_name: str
    issuer: str
    client_id: str
    enabled: bool
    client_secret_set: bool | Unset = UNSET
    scopes: str | Unset = UNSET
    claims_mapping: str | Unset = UNSET
    endpoint_overrides: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        provider_key = self.provider_key

        display_name = self.display_name

        issuer = self.issuer

        client_id = self.client_id

        enabled = self.enabled

        client_secret_set = self.client_secret_set

        scopes = self.scopes

        claims_mapping = self.claims_mapping

        endpoint_overrides = self.endpoint_overrides

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "provider_key": provider_key,
                "display_name": display_name,
                "issuer": issuer,
                "client_id": client_id,
                "enabled": enabled,
            }
        )
        if client_secret_set is not UNSET:
            field_dict["client_secret_set"] = client_secret_set
        if scopes is not UNSET:
            field_dict["scopes"] = scopes
        if claims_mapping is not UNSET:
            field_dict["claims_mapping"] = claims_mapping
        if endpoint_overrides is not UNSET:
            field_dict["endpoint_overrides"] = endpoint_overrides

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        provider_key = d.pop("provider_key")

        display_name = d.pop("display_name")

        issuer = d.pop("issuer")

        client_id = d.pop("client_id")

        enabled = d.pop("enabled")

        client_secret_set = d.pop("client_secret_set", UNSET)

        scopes = d.pop("scopes", UNSET)

        claims_mapping = d.pop("claims_mapping", UNSET)

        endpoint_overrides = d.pop("endpoint_overrides", UNSET)

        upstream_provider = cls(
            provider_key=provider_key,
            display_name=display_name,
            issuer=issuer,
            client_id=client_id,
            enabled=enabled,
            client_secret_set=client_secret_set,
            scopes=scopes,
            claims_mapping=claims_mapping,
            endpoint_overrides=endpoint_overrides,
        )

        upstream_provider.additional_properties = d
        return upstream_provider

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
