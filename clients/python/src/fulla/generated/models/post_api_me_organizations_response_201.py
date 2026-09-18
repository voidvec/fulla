from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="PostApiMeOrganizationsResponse201")


@_attrs_define
class PostApiMeOrganizationsResponse201:
    """
    Attributes:
        slug (str | Unset):
        role (str | Unset):
        message (str | Unset):
    """

    slug: str | Unset = UNSET
    role: str | Unset = UNSET
    message: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        slug = self.slug

        role = self.role

        message = self.message

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if slug is not UNSET:
            field_dict["slug"] = slug
        if role is not UNSET:
            field_dict["role"] = role
        if message is not UNSET:
            field_dict["message"] = message

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        slug = d.pop("slug", UNSET)

        role = d.pop("role", UNSET)

        message = d.pop("message", UNSET)

        post_api_me_organizations_response_201 = cls(
            slug=slug,
            role=role,
            message=message,
        )

        post_api_me_organizations_response_201.additional_properties = d
        return post_api_me_organizations_response_201

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
