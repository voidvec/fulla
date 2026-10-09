from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..models.auth_provider_provider import AuthProviderProvider

T = TypeVar("T", bound="AuthProvider")


@_attrs_define
class AuthProvider:
    """An external login provider currently enabled on this deployment (v1.4.0 provider tiers). authorize_url is the fully
    built provider authorize URL — the SPA redirects to it as-is; the redirect target resolves from the per-provider
    redirect_uri override, else frontend.url + /callback/{provider}.

        Attributes:
            authorize_url (str):
            provider (AuthProviderProvider):
    """

    authorize_url: str
    provider: AuthProviderProvider
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        authorize_url = self.authorize_url

        provider = self.provider.value

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "authorize_url": authorize_url,
                "provider": provider,
            }
        )

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        authorize_url = d.pop("authorize_url")

        provider = AuthProviderProvider(d.pop("provider"))

        auth_provider = cls(
            authorize_url=authorize_url,
            provider=provider,
        )

        auth_provider.additional_properties = d
        return auth_provider

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
