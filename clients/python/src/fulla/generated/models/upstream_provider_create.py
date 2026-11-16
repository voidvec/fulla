from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="UpstreamProviderCreate")


@_attrs_define
class UpstreamProviderCreate:
    """Create/update body for the upstream registry (client_secret write-only; empty on update keeps the current value).

    Attributes:
        provider_key (str): ^[a-z0-9][a-z0-9-]{1,62}[a-z0-9]$; not github/google/wechat (create only; immutable).
        display_name (str):
        issuer (str):
        client_id (str):
        client_secret (str):
        scopes (str | Unset):
        claims_mapping (str | Unset):
        endpoint_overrides (str | Unset):
        enabled (bool | Unset):
    """

    provider_key: str
    display_name: str
    issuer: str
    client_id: str
    client_secret: str
    scopes: str | Unset = UNSET
    claims_mapping: str | Unset = UNSET
    endpoint_overrides: str | Unset = UNSET
    enabled: bool | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        provider_key = self.provider_key

        display_name = self.display_name

        issuer = self.issuer

        client_id = self.client_id

        client_secret = self.client_secret

        scopes = self.scopes

        claims_mapping = self.claims_mapping

        endpoint_overrides = self.endpoint_overrides

        enabled = self.enabled

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "provider_key": provider_key,
                "display_name": display_name,
                "issuer": issuer,
                "client_id": client_id,
                "client_secret": client_secret,
            }
        )
        if scopes is not UNSET:
            field_dict["scopes"] = scopes
        if claims_mapping is not UNSET:
            field_dict["claims_mapping"] = claims_mapping
        if endpoint_overrides is not UNSET:
            field_dict["endpoint_overrides"] = endpoint_overrides
        if enabled is not UNSET:
            field_dict["enabled"] = enabled

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        provider_key = d.pop("provider_key")

        display_name = d.pop("display_name")

        issuer = d.pop("issuer")

        client_id = d.pop("client_id")

        client_secret = d.pop("client_secret")

        scopes = d.pop("scopes", UNSET)

        claims_mapping = d.pop("claims_mapping", UNSET)

        endpoint_overrides = d.pop("endpoint_overrides", UNSET)

        enabled = d.pop("enabled", UNSET)

        upstream_provider_create = cls(
            provider_key=provider_key,
            display_name=display_name,
            issuer=issuer,
            client_id=client_id,
            client_secret=client_secret,
            scopes=scopes,
            claims_mapping=claims_mapping,
            endpoint_overrides=endpoint_overrides,
            enabled=enabled,
        )

        upstream_provider_create.additional_properties = d
        return upstream_provider_create

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
