from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..models.auth_provider_type import AuthProviderType
from ..types import UNSET, Unset

T = TypeVar("T", bound="AuthProvider")


@_attrs_define
class AuthProvider:
    """An enabled external login provider. The deprecated authorize_url is always empty. Start a browser-bound transaction
    using the authorize POST. P2-1 registry-configured OIDC upstreams additionally carry display_name and type=oidc.

        Attributes:
            authorize_url (str):
            provider (str): Config-static social keys (github, google, wechat) or a registry provider key
                (upstream_providers.provider_key, lowercase [a-z0-9-]).
            display_name (str | Unset): Registry providers only; the login-page button label.
            type_ (AuthProviderType | Unset): Registry providers only; config-static social entries omit this field.
    """

    authorize_url: str
    provider: str
    display_name: str | Unset = UNSET
    type_: AuthProviderType | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        authorize_url = self.authorize_url

        provider = self.provider

        display_name = self.display_name

        type_: str | Unset = UNSET
        if not isinstance(self.type_, Unset):
            type_ = self.type_.value

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "authorize_url": authorize_url,
                "provider": provider,
            }
        )
        if display_name is not UNSET:
            field_dict["display_name"] = display_name
        if type_ is not UNSET:
            field_dict["type"] = type_

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        authorize_url = d.pop("authorize_url")

        provider = d.pop("provider")

        display_name = d.pop("display_name", UNSET)

        _type_ = d.pop("type", UNSET)
        type_: AuthProviderType | Unset
        if isinstance(_type_, Unset):
            type_ = UNSET
        else:
            type_ = AuthProviderType(_type_)

        auth_provider = cls(
            authorize_url=authorize_url,
            provider=provider,
            display_name=display_name,
            type_=type_,
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
