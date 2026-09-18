from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="PostApiMeOrganizationsSlugInvitationsResponse201")


@_attrs_define
class PostApiMeOrganizationsSlugInvitationsResponse201:
    """
    Attributes:
        id (int | Unset):
        email (str | Unset):
        role (str | Unset):
        token (str | Unset): Present exactly once; also delivered by email when SMTP is configured.
        expires_at (str | Unset):
        message (str | Unset):
    """

    id: int | Unset = UNSET
    email: str | Unset = UNSET
    role: str | Unset = UNSET
    token: str | Unset = UNSET
    expires_at: str | Unset = UNSET
    message: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        id = self.id

        email = self.email

        role = self.role

        token = self.token

        expires_at = self.expires_at

        message = self.message

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if id is not UNSET:
            field_dict["id"] = id
        if email is not UNSET:
            field_dict["email"] = email
        if role is not UNSET:
            field_dict["role"] = role
        if token is not UNSET:
            field_dict["token"] = token
        if expires_at is not UNSET:
            field_dict["expires_at"] = expires_at
        if message is not UNSET:
            field_dict["message"] = message

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        id = d.pop("id", UNSET)

        email = d.pop("email", UNSET)

        role = d.pop("role", UNSET)

        token = d.pop("token", UNSET)

        expires_at = d.pop("expires_at", UNSET)

        message = d.pop("message", UNSET)

        post_api_me_organizations_slug_invitations_response_201 = cls(
            id=id,
            email=email,
            role=role,
            token=token,
            expires_at=expires_at,
            message=message,
        )

        post_api_me_organizations_slug_invitations_response_201.additional_properties = d
        return post_api_me_organizations_slug_invitations_response_201

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
