from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="GetApiAdminOrganizationsSlugMembersResponse200MembersItem")


@_attrs_define
class GetApiAdminOrganizationsSlugMembersResponse200MembersItem:
    """
    Attributes:
        user_id (int | Unset):
        username (str | Unset):
        role (str | Unset):
        joined_at (str | Unset):
    """

    user_id: int | Unset = UNSET
    username: str | Unset = UNSET
    role: str | Unset = UNSET
    joined_at: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        user_id = self.user_id

        username = self.username

        role = self.role

        joined_at = self.joined_at

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if user_id is not UNSET:
            field_dict["user_id"] = user_id
        if username is not UNSET:
            field_dict["username"] = username
        if role is not UNSET:
            field_dict["role"] = role
        if joined_at is not UNSET:
            field_dict["joined_at"] = joined_at

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        user_id = d.pop("user_id", UNSET)

        username = d.pop("username", UNSET)

        role = d.pop("role", UNSET)

        joined_at = d.pop("joined_at", UNSET)

        get_api_admin_organizations_slug_members_response_200_members_item = cls(
            user_id=user_id,
            username=username,
            role=role,
            joined_at=joined_at,
        )

        get_api_admin_organizations_slug_members_response_200_members_item.additional_properties = d
        return get_api_admin_organizations_slug_members_response_200_members_item

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
