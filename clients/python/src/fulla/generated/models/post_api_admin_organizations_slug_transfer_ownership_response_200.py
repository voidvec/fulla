from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="PostApiAdminOrganizationsSlugTransferOwnershipResponse200")


@_attrs_define
class PostApiAdminOrganizationsSlugTransferOwnershipResponse200:
    """
    Attributes:
        slug (str | Unset):
        organization_id (int | Unset):
        owner_user_id (int | Unset):
    """

    slug: str | Unset = UNSET
    organization_id: int | Unset = UNSET
    owner_user_id: int | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        slug = self.slug

        organization_id = self.organization_id

        owner_user_id = self.owner_user_id

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if slug is not UNSET:
            field_dict["slug"] = slug
        if organization_id is not UNSET:
            field_dict["organization_id"] = organization_id
        if owner_user_id is not UNSET:
            field_dict["owner_user_id"] = owner_user_id

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        slug = d.pop("slug", UNSET)

        organization_id = d.pop("organization_id", UNSET)

        owner_user_id = d.pop("owner_user_id", UNSET)

        post_api_admin_organizations_slug_transfer_ownership_response_200 = cls(
            slug=slug,
            organization_id=organization_id,
            owner_user_id=owner_user_id,
        )

        post_api_admin_organizations_slug_transfer_ownership_response_200.additional_properties = d
        return post_api_admin_organizations_slug_transfer_ownership_response_200

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
