from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar, cast

from attrs import define as _attrs_define
from attrs import field as _attrs_field

T = TypeVar("T", bound="PostApiMeApplicationsClientIdTransferBody")


@_attrs_define
class PostApiMeApplicationsClientIdTransferBody:
    """
    Attributes:
        org_slug (None | str): Target organization slug, or null to transfer back to personal ownership.
    """

    org_slug: None | str
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        org_slug: None | str
        org_slug = self.org_slug

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "org_slug": org_slug,
            }
        )

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)

        def _parse_org_slug(data: object) -> None | str:
            if data is None:
                return data
            return cast(None | str, data)

        org_slug = _parse_org_slug(d.pop("org_slug"))

        post_api_me_applications_client_id_transfer_body = cls(
            org_slug=org_slug,
        )

        post_api_me_applications_client_id_transfer_body.additional_properties = d
        return post_api_me_applications_client_id_transfer_body

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
