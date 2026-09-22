from __future__ import annotations

from collections.abc import Mapping
from typing import TYPE_CHECKING, Any, TypeVar, cast

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

if TYPE_CHECKING:
    from ..models.get_oauth_2_consent_context_response_200_org_type_0 import GetOauth2ConsentContextResponse200OrgType0


T = TypeVar("T", bound="GetOauth2ConsentContextResponse200")


@_attrs_define
class GetOauth2ConsentContextResponse200:
    """
    Attributes:
        owner_name (str | Unset): Attribution label: org name for org apps, the creator's display name for personal
            apps; empty for admin-seeded clients and on storage degradation.
        org (GetOauth2ConsentContextResponse200OrgType0 | None | Unset): The org context this flow is authorizing under
            (design 2.1); null when the flow has no org binding.
    """

    owner_name: str | Unset = UNSET
    org: GetOauth2ConsentContextResponse200OrgType0 | None | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        from ..models.get_oauth_2_consent_context_response_200_org_type_0 import (
            GetOauth2ConsentContextResponse200OrgType0,
        )

        owner_name = self.owner_name

        org: dict[str, Any] | None | Unset
        if isinstance(self.org, Unset):
            org = UNSET
        elif isinstance(self.org, GetOauth2ConsentContextResponse200OrgType0):
            org = self.org.to_dict()
        else:
            org = self.org

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if owner_name is not UNSET:
            field_dict["owner_name"] = owner_name
        if org is not UNSET:
            field_dict["org"] = org

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        from ..models.get_oauth_2_consent_context_response_200_org_type_0 import (
            GetOauth2ConsentContextResponse200OrgType0,
        )

        d = dict(src_dict)
        owner_name = d.pop("owner_name", UNSET)

        def _parse_org(data: object) -> GetOauth2ConsentContextResponse200OrgType0 | None | Unset:
            if data is None:
                return data
            if isinstance(data, Unset):
                return data
            try:
                if not isinstance(data, dict):
                    raise TypeError()
                org_type_0 = GetOauth2ConsentContextResponse200OrgType0.from_dict(data)

                return org_type_0
            except (TypeError, ValueError, AttributeError, KeyError):
                pass
            return cast(GetOauth2ConsentContextResponse200OrgType0 | None | Unset, data)

        org = _parse_org(d.pop("org", UNSET))

        get_oauth_2_consent_context_response_200 = cls(
            owner_name=owner_name,
            org=org,
        )

        get_oauth_2_consent_context_response_200.additional_properties = d
        return get_oauth_2_consent_context_response_200

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
