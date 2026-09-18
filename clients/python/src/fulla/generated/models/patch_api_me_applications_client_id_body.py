from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar, cast

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="PatchApiMeApplicationsClientIdBody")


@_attrs_define
class PatchApiMeApplicationsClientIdBody:
    """
    Attributes:
        name (str | Unset):
        redirect_uris (list[str] | Unset):
        allowed_grant_types (list[str] | Unset):
        scopes (list[str] | Unset):
    """

    name: str | Unset = UNSET
    redirect_uris: list[str] | Unset = UNSET
    allowed_grant_types: list[str] | Unset = UNSET
    scopes: list[str] | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        name = self.name

        redirect_uris: list[str] | Unset = UNSET
        if not isinstance(self.redirect_uris, Unset):
            redirect_uris = self.redirect_uris

        allowed_grant_types: list[str] | Unset = UNSET
        if not isinstance(self.allowed_grant_types, Unset):
            allowed_grant_types = self.allowed_grant_types

        scopes: list[str] | Unset = UNSET
        if not isinstance(self.scopes, Unset):
            scopes = self.scopes

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if name is not UNSET:
            field_dict["name"] = name
        if redirect_uris is not UNSET:
            field_dict["redirect_uris"] = redirect_uris
        if allowed_grant_types is not UNSET:
            field_dict["allowed_grant_types"] = allowed_grant_types
        if scopes is not UNSET:
            field_dict["scopes"] = scopes

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        name = d.pop("name", UNSET)

        redirect_uris = cast(list[str], d.pop("redirect_uris", UNSET))

        allowed_grant_types = cast(list[str], d.pop("allowed_grant_types", UNSET))

        scopes = cast(list[str], d.pop("scopes", UNSET))

        patch_api_me_applications_client_id_body = cls(
            name=name,
            redirect_uris=redirect_uris,
            allowed_grant_types=allowed_grant_types,
            scopes=scopes,
        )

        patch_api_me_applications_client_id_body.additional_properties = d
        return patch_api_me_applications_client_id_body

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
