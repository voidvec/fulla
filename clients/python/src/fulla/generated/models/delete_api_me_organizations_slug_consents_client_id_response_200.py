from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="DeleteApiMeOrganizationsSlugConsentsClientIdResponse200")


@_attrs_define
class DeleteApiMeOrganizationsSlugConsentsClientIdResponse200:
    """
    Attributes:
        slug (str | Unset):
        client_id (str | Unset):
        revoked (int | Unset):
        message (str | Unset):
    """

    slug: str | Unset = UNSET
    client_id: str | Unset = UNSET
    revoked: int | Unset = UNSET
    message: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        slug = self.slug

        client_id = self.client_id

        revoked = self.revoked

        message = self.message

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if slug is not UNSET:
            field_dict["slug"] = slug
        if client_id is not UNSET:
            field_dict["client_id"] = client_id
        if revoked is not UNSET:
            field_dict["revoked"] = revoked
        if message is not UNSET:
            field_dict["message"] = message

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        slug = d.pop("slug", UNSET)

        client_id = d.pop("client_id", UNSET)

        revoked = d.pop("revoked", UNSET)

        message = d.pop("message", UNSET)

        delete_api_me_organizations_slug_consents_client_id_response_200 = cls(
            slug=slug,
            client_id=client_id,
            revoked=revoked,
            message=message,
        )

        delete_api_me_organizations_slug_consents_client_id_response_200.additional_properties = d
        return delete_api_me_organizations_slug_consents_client_id_response_200

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
