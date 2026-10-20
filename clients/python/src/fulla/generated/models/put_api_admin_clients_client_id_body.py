from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="PutApiAdminClientsClientIdBody")


@_attrs_define
class PutApiAdminClientsClientIdBody:
    """
    Attributes:
        name (str | Unset):
        redirect_uris (str | Unset):
        allowed_grant_types (str | Unset):
        backchannel_logout_uri (str | Unset): Set to an https URI to enable OIDC back-channel logout notifications to
            this RP; an empty string clears it.
        logo_uri (str | Unset): P1-1 (#279): client brand image (https-only, loopback exempt). An empty string clears
            it.
    """

    name: str | Unset = UNSET
    redirect_uris: str | Unset = UNSET
    allowed_grant_types: str | Unset = UNSET
    backchannel_logout_uri: str | Unset = UNSET
    logo_uri: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        name = self.name

        redirect_uris = self.redirect_uris

        allowed_grant_types = self.allowed_grant_types

        backchannel_logout_uri = self.backchannel_logout_uri

        logo_uri = self.logo_uri

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if name is not UNSET:
            field_dict["name"] = name
        if redirect_uris is not UNSET:
            field_dict["redirect_uris"] = redirect_uris
        if allowed_grant_types is not UNSET:
            field_dict["allowed_grant_types"] = allowed_grant_types
        if backchannel_logout_uri is not UNSET:
            field_dict["backchannel_logout_uri"] = backchannel_logout_uri
        if logo_uri is not UNSET:
            field_dict["logo_uri"] = logo_uri

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        name = d.pop("name", UNSET)

        redirect_uris = d.pop("redirect_uris", UNSET)

        allowed_grant_types = d.pop("allowed_grant_types", UNSET)

        backchannel_logout_uri = d.pop("backchannel_logout_uri", UNSET)

        logo_uri = d.pop("logo_uri", UNSET)

        put_api_admin_clients_client_id_body = cls(
            name=name,
            redirect_uris=redirect_uris,
            allowed_grant_types=allowed_grant_types,
            backchannel_logout_uri=backchannel_logout_uri,
            logo_uri=logo_uri,
        )

        put_api_admin_clients_client_id_body.additional_properties = d
        return put_api_admin_clients_client_id_body

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
