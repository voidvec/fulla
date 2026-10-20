from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="GetOauth2ConsentContextResponse200OrgType0")


@_attrs_define
class GetOauth2ConsentContextResponse200OrgType0:
    """The org context this flow is authorizing under (design 2.1); null when the flow has no org binding. P1-1 adds the
    org brand fields (absent when unset).

        Attributes:
            org_id (int | Unset):
            org_name (str | Unset):
            logo_uri (str | Unset):
            primary_color (str | Unset):
    """

    org_id: int | Unset = UNSET
    org_name: str | Unset = UNSET
    logo_uri: str | Unset = UNSET
    primary_color: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        org_id = self.org_id

        org_name = self.org_name

        logo_uri = self.logo_uri

        primary_color = self.primary_color

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if org_id is not UNSET:
            field_dict["org_id"] = org_id
        if org_name is not UNSET:
            field_dict["org_name"] = org_name
        if logo_uri is not UNSET:
            field_dict["logo_uri"] = logo_uri
        if primary_color is not UNSET:
            field_dict["primary_color"] = primary_color

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        org_id = d.pop("org_id", UNSET)

        org_name = d.pop("org_name", UNSET)

        logo_uri = d.pop("logo_uri", UNSET)

        primary_color = d.pop("primary_color", UNSET)

        get_oauth_2_consent_context_response_200_org_type_0 = cls(
            org_id=org_id,
            org_name=org_name,
            logo_uri=logo_uri,
            primary_color=primary_color,
        )

        get_oauth_2_consent_context_response_200_org_type_0.additional_properties = d
        return get_oauth_2_consent_context_response_200_org_type_0

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
