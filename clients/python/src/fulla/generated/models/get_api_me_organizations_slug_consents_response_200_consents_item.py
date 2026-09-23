from __future__ import annotations

from collections.abc import Mapping
from typing import TYPE_CHECKING, Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

if TYPE_CHECKING:
    from ..models.get_api_me_organizations_slug_consents_response_200_consents_item_scopes_item import (
        GetApiMeOrganizationsSlugConsentsResponse200ConsentsItemScopesItem,
    )


T = TypeVar("T", bound="GetApiMeOrganizationsSlugConsentsResponse200ConsentsItem")


@_attrs_define
class GetApiMeOrganizationsSlugConsentsResponse200ConsentsItem:
    """
    Attributes:
        client_id (str | Unset):
        scopes (list[GetApiMeOrganizationsSlugConsentsResponse200ConsentsItemScopesItem] | Unset):
    """

    client_id: str | Unset = UNSET
    scopes: list[GetApiMeOrganizationsSlugConsentsResponse200ConsentsItemScopesItem] | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        client_id = self.client_id

        scopes: list[dict[str, Any]] | Unset = UNSET
        if not isinstance(self.scopes, Unset):
            scopes = []
            for scopes_item_data in self.scopes:
                scopes_item = scopes_item_data.to_dict()
                scopes.append(scopes_item)

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if client_id is not UNSET:
            field_dict["client_id"] = client_id
        if scopes is not UNSET:
            field_dict["scopes"] = scopes

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        from ..models.get_api_me_organizations_slug_consents_response_200_consents_item_scopes_item import (
            GetApiMeOrganizationsSlugConsentsResponse200ConsentsItemScopesItem,
        )

        d = dict(src_dict)
        client_id = d.pop("client_id", UNSET)

        _scopes = d.pop("scopes", UNSET)
        scopes: list[GetApiMeOrganizationsSlugConsentsResponse200ConsentsItemScopesItem] | Unset = UNSET
        if _scopes is not UNSET:
            scopes = []
            for scopes_item_data in _scopes:
                scopes_item = GetApiMeOrganizationsSlugConsentsResponse200ConsentsItemScopesItem.from_dict(
                    scopes_item_data
                )

                scopes.append(scopes_item)

        get_api_me_organizations_slug_consents_response_200_consents_item = cls(
            client_id=client_id,
            scopes=scopes,
        )

        get_api_me_organizations_slug_consents_response_200_consents_item.additional_properties = d
        return get_api_me_organizations_slug_consents_response_200_consents_item

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
