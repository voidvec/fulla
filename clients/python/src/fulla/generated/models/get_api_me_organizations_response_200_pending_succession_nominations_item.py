from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="GetApiMeOrganizationsResponse200PendingSuccessionNominationsItem")


@_attrs_define
class GetApiMeOrganizationsResponse200PendingSuccessionNominationsItem:
    """
    Attributes:
        organization_id (int | Unset):
        slug (str | Unset):
        name (str | Unset):
        created_at (str | Unset):
    """

    organization_id: int | Unset = UNSET
    slug: str | Unset = UNSET
    name: str | Unset = UNSET
    created_at: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        organization_id = self.organization_id

        slug = self.slug

        name = self.name

        created_at = self.created_at

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if organization_id is not UNSET:
            field_dict["organization_id"] = organization_id
        if slug is not UNSET:
            field_dict["slug"] = slug
        if name is not UNSET:
            field_dict["name"] = name
        if created_at is not UNSET:
            field_dict["created_at"] = created_at

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        organization_id = d.pop("organization_id", UNSET)

        slug = d.pop("slug", UNSET)

        name = d.pop("name", UNSET)

        created_at = d.pop("created_at", UNSET)

        get_api_me_organizations_response_200_pending_succession_nominations_item = cls(
            organization_id=organization_id,
            slug=slug,
            name=name,
            created_at=created_at,
        )

        get_api_me_organizations_response_200_pending_succession_nominations_item.additional_properties = d
        return get_api_me_organizations_response_200_pending_succession_nominations_item

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
