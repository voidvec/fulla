from __future__ import annotations

from collections.abc import Mapping
from typing import TYPE_CHECKING, Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

if TYPE_CHECKING:
    from ..models.get_api_me_organizations_slug_consents_response_200_consents_item import (
        GetApiMeOrganizationsSlugConsentsResponse200ConsentsItem,
    )


T = TypeVar("T", bound="GetApiMeOrganizationsSlugConsentsResponse200")


@_attrs_define
class GetApiMeOrganizationsSlugConsentsResponse200:
    """
    Attributes:
        slug (str | Unset):
        consents (list[GetApiMeOrganizationsSlugConsentsResponse200ConsentsItem] | Unset):
        total (int | Unset):
    """

    slug: str | Unset = UNSET
    consents: list[GetApiMeOrganizationsSlugConsentsResponse200ConsentsItem] | Unset = UNSET
    total: int | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        slug = self.slug

        consents: list[dict[str, Any]] | Unset = UNSET
        if not isinstance(self.consents, Unset):
            consents = []
            for consents_item_data in self.consents:
                consents_item = consents_item_data.to_dict()
                consents.append(consents_item)

        total = self.total

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if slug is not UNSET:
            field_dict["slug"] = slug
        if consents is not UNSET:
            field_dict["consents"] = consents
        if total is not UNSET:
            field_dict["total"] = total

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        from ..models.get_api_me_organizations_slug_consents_response_200_consents_item import (
            GetApiMeOrganizationsSlugConsentsResponse200ConsentsItem,
        )

        d = dict(src_dict)
        slug = d.pop("slug", UNSET)

        _consents = d.pop("consents", UNSET)
        consents: list[GetApiMeOrganizationsSlugConsentsResponse200ConsentsItem] | Unset = UNSET
        if _consents is not UNSET:
            consents = []
            for consents_item_data in _consents:
                consents_item = GetApiMeOrganizationsSlugConsentsResponse200ConsentsItem.from_dict(consents_item_data)

                consents.append(consents_item)

        total = d.pop("total", UNSET)

        get_api_me_organizations_slug_consents_response_200 = cls(
            slug=slug,
            consents=consents,
            total=total,
        )

        get_api_me_organizations_slug_consents_response_200.additional_properties = d
        return get_api_me_organizations_slug_consents_response_200

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
