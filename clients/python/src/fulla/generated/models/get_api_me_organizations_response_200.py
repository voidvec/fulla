from __future__ import annotations

from collections.abc import Mapping
from typing import TYPE_CHECKING, Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

if TYPE_CHECKING:
    from ..models.get_api_me_organizations_response_200_organizations_item import (
        GetApiMeOrganizationsResponse200OrganizationsItem,
    )


T = TypeVar("T", bound="GetApiMeOrganizationsResponse200")


@_attrs_define
class GetApiMeOrganizationsResponse200:
    """
    Attributes:
        organizations (list[GetApiMeOrganizationsResponse200OrganizationsItem] | Unset):
        total (int | Unset):
    """

    organizations: list[GetApiMeOrganizationsResponse200OrganizationsItem] | Unset = UNSET
    total: int | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        organizations: list[dict[str, Any]] | Unset = UNSET
        if not isinstance(self.organizations, Unset):
            organizations = []
            for organizations_item_data in self.organizations:
                organizations_item = organizations_item_data.to_dict()
                organizations.append(organizations_item)

        total = self.total

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if organizations is not UNSET:
            field_dict["organizations"] = organizations
        if total is not UNSET:
            field_dict["total"] = total

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        from ..models.get_api_me_organizations_response_200_organizations_item import (
            GetApiMeOrganizationsResponse200OrganizationsItem,
        )

        d = dict(src_dict)
        _organizations = d.pop("organizations", UNSET)
        organizations: list[GetApiMeOrganizationsResponse200OrganizationsItem] | Unset = UNSET
        if _organizations is not UNSET:
            organizations = []
            for organizations_item_data in _organizations:
                organizations_item = GetApiMeOrganizationsResponse200OrganizationsItem.from_dict(
                    organizations_item_data
                )

                organizations.append(organizations_item)

        total = d.pop("total", UNSET)

        get_api_me_organizations_response_200 = cls(
            organizations=organizations,
            total=total,
        )

        get_api_me_organizations_response_200.additional_properties = d
        return get_api_me_organizations_response_200

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
