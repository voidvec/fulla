from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="PostApiMeOrganizationsBody")


@_attrs_define
class PostApiMeOrganizationsBody:
    """
    Attributes:
        slug (str):
        name (str):
        logo_uri (str | Unset):
        primary_color (str | Unset):
    """

    slug: str
    name: str
    logo_uri: str | Unset = UNSET
    primary_color: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        slug = self.slug

        name = self.name

        logo_uri = self.logo_uri

        primary_color = self.primary_color

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "slug": slug,
                "name": name,
            }
        )
        if logo_uri is not UNSET:
            field_dict["logo_uri"] = logo_uri
        if primary_color is not UNSET:
            field_dict["primary_color"] = primary_color

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        slug = d.pop("slug")

        name = d.pop("name")

        logo_uri = d.pop("logo_uri", UNSET)

        primary_color = d.pop("primary_color", UNSET)

        post_api_me_organizations_body = cls(
            slug=slug,
            name=name,
            logo_uri=logo_uri,
            primary_color=primary_color,
        )

        post_api_me_organizations_body.additional_properties = d
        return post_api_me_organizations_body

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
