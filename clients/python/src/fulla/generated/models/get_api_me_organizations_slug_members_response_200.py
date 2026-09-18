from __future__ import annotations

from collections.abc import Mapping
from typing import TYPE_CHECKING, Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

if TYPE_CHECKING:
    from ..models.get_api_me_organizations_slug_members_response_200_members_item import (
        GetApiMeOrganizationsSlugMembersResponse200MembersItem,
    )


T = TypeVar("T", bound="GetApiMeOrganizationsSlugMembersResponse200")


@_attrs_define
class GetApiMeOrganizationsSlugMembersResponse200:
    """
    Attributes:
        members (list[GetApiMeOrganizationsSlugMembersResponse200MembersItem] | Unset):
        total (int | Unset):
    """

    members: list[GetApiMeOrganizationsSlugMembersResponse200MembersItem] | Unset = UNSET
    total: int | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        members: list[dict[str, Any]] | Unset = UNSET
        if not isinstance(self.members, Unset):
            members = []
            for members_item_data in self.members:
                members_item = members_item_data.to_dict()
                members.append(members_item)

        total = self.total

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if members is not UNSET:
            field_dict["members"] = members
        if total is not UNSET:
            field_dict["total"] = total

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        from ..models.get_api_me_organizations_slug_members_response_200_members_item import (
            GetApiMeOrganizationsSlugMembersResponse200MembersItem,
        )

        d = dict(src_dict)
        _members = d.pop("members", UNSET)
        members: list[GetApiMeOrganizationsSlugMembersResponse200MembersItem] | Unset = UNSET
        if _members is not UNSET:
            members = []
            for members_item_data in _members:
                members_item = GetApiMeOrganizationsSlugMembersResponse200MembersItem.from_dict(members_item_data)

                members.append(members_item)

        total = d.pop("total", UNSET)

        get_api_me_organizations_slug_members_response_200 = cls(
            members=members,
            total=total,
        )

        get_api_me_organizations_slug_members_response_200.additional_properties = d
        return get_api_me_organizations_slug_members_response_200

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
