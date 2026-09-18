from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..models.get_api_me_organizations_slug_members_response_200_members_item_role import (
    GetApiMeOrganizationsSlugMembersResponse200MembersItemRole,
)
from ..types import UNSET, Unset

T = TypeVar("T", bound="GetApiMeOrganizationsSlugMembersResponse200MembersItem")


@_attrs_define
class GetApiMeOrganizationsSlugMembersResponse200MembersItem:
    """
    Attributes:
        user_id (int | Unset):
        username (str | Unset):
        display_name (str | Unset):
        role (GetApiMeOrganizationsSlugMembersResponse200MembersItemRole | Unset):
    """

    user_id: int | Unset = UNSET
    username: str | Unset = UNSET
    display_name: str | Unset = UNSET
    role: GetApiMeOrganizationsSlugMembersResponse200MembersItemRole | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        user_id = self.user_id

        username = self.username

        display_name = self.display_name

        role: str | Unset = UNSET
        if not isinstance(self.role, Unset):
            role = self.role.value

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if user_id is not UNSET:
            field_dict["user_id"] = user_id
        if username is not UNSET:
            field_dict["username"] = username
        if display_name is not UNSET:
            field_dict["display_name"] = display_name
        if role is not UNSET:
            field_dict["role"] = role

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        user_id = d.pop("user_id", UNSET)

        username = d.pop("username", UNSET)

        display_name = d.pop("display_name", UNSET)

        _role = d.pop("role", UNSET)
        role: GetApiMeOrganizationsSlugMembersResponse200MembersItemRole | Unset
        if isinstance(_role, Unset):
            role = UNSET
        else:
            role = GetApiMeOrganizationsSlugMembersResponse200MembersItemRole(_role)

        get_api_me_organizations_slug_members_response_200_members_item = cls(
            user_id=user_id,
            username=username,
            display_name=display_name,
            role=role,
        )

        get_api_me_organizations_slug_members_response_200_members_item.additional_properties = d
        return get_api_me_organizations_slug_members_response_200_members_item

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
