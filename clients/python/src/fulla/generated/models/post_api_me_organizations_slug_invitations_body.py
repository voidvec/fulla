from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..models.post_api_me_organizations_slug_invitations_body_role import PostApiMeOrganizationsSlugInvitationsBodyRole
from ..types import UNSET, Unset

T = TypeVar("T", bound="PostApiMeOrganizationsSlugInvitationsBody")


@_attrs_define
class PostApiMeOrganizationsSlugInvitationsBody:
    """
    Attributes:
        email (str):
        role (PostApiMeOrganizationsSlugInvitationsBodyRole | Unset):  Default:
            PostApiMeOrganizationsSlugInvitationsBodyRole.MEMBER.
    """

    email: str
    role: PostApiMeOrganizationsSlugInvitationsBodyRole | Unset = PostApiMeOrganizationsSlugInvitationsBodyRole.MEMBER
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        email = self.email

        role: str | Unset = UNSET
        if not isinstance(self.role, Unset):
            role = self.role.value

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "email": email,
            }
        )
        if role is not UNSET:
            field_dict["role"] = role

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        email = d.pop("email")

        _role = d.pop("role", UNSET)
        role: PostApiMeOrganizationsSlugInvitationsBodyRole | Unset
        if isinstance(_role, Unset):
            role = UNSET
        else:
            role = PostApiMeOrganizationsSlugInvitationsBodyRole(_role)

        post_api_me_organizations_slug_invitations_body = cls(
            email=email,
            role=role,
        )

        post_api_me_organizations_slug_invitations_body.additional_properties = d
        return post_api_me_organizations_slug_invitations_body

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
