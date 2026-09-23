from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="GetApiMeOrganizationsSlugConsentsResponse200ConsentsItemScopesItem")


@_attrs_define
class GetApiMeOrganizationsSlugConsentsResponse200ConsentsItemScopesItem:
    """
    Attributes:
        scope (str | Unset):
        granted_by (int | Unset):
        granted_at (str | Unset):
    """

    scope: str | Unset = UNSET
    granted_by: int | Unset = UNSET
    granted_at: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        scope = self.scope

        granted_by = self.granted_by

        granted_at = self.granted_at

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if scope is not UNSET:
            field_dict["scope"] = scope
        if granted_by is not UNSET:
            field_dict["granted_by"] = granted_by
        if granted_at is not UNSET:
            field_dict["granted_at"] = granted_at

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        scope = d.pop("scope", UNSET)

        granted_by = d.pop("granted_by", UNSET)

        granted_at = d.pop("granted_at", UNSET)

        get_api_me_organizations_slug_consents_response_200_consents_item_scopes_item = cls(
            scope=scope,
            granted_by=granted_by,
            granted_at=granted_at,
        )

        get_api_me_organizations_slug_consents_response_200_consents_item_scopes_item.additional_properties = d
        return get_api_me_organizations_slug_consents_response_200_consents_item_scopes_item

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
