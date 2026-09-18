from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="GetApiMeOrganizationsSlugInvitationsResponse200InvitationsItem")


@_attrs_define
class GetApiMeOrganizationsSlugInvitationsResponse200InvitationsItem:
    """
    Attributes:
        id (int | Unset):
        email (str | Unset):
        role (str | Unset):
        expires_at (str | Unset):
    """

    id: int | Unset = UNSET
    email: str | Unset = UNSET
    role: str | Unset = UNSET
    expires_at: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        id = self.id

        email = self.email

        role = self.role

        expires_at = self.expires_at

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if id is not UNSET:
            field_dict["id"] = id
        if email is not UNSET:
            field_dict["email"] = email
        if role is not UNSET:
            field_dict["role"] = role
        if expires_at is not UNSET:
            field_dict["expires_at"] = expires_at

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        id = d.pop("id", UNSET)

        email = d.pop("email", UNSET)

        role = d.pop("role", UNSET)

        expires_at = d.pop("expires_at", UNSET)

        get_api_me_organizations_slug_invitations_response_200_invitations_item = cls(
            id=id,
            email=email,
            role=role,
            expires_at=expires_at,
        )

        get_api_me_organizations_slug_invitations_response_200_invitations_item.additional_properties = d
        return get_api_me_organizations_slug_invitations_response_200_invitations_item

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
