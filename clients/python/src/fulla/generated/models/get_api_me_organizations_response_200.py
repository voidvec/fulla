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
    from ..models.get_api_me_organizations_response_200_pending_succession_nominations_item import (
        GetApiMeOrganizationsResponse200PendingSuccessionNominationsItem,
    )


T = TypeVar("T", bound="GetApiMeOrganizationsResponse200")


@_attrs_define
class GetApiMeOrganizationsResponse200:
    """
    Attributes:
        organizations (list[GetApiMeOrganizationsResponse200OrganizationsItem] | Unset):
        total (int | Unset):
        pending_succession_nominations (list[GetApiMeOrganizationsResponse200PendingSuccessionNominationsItem] | Unset):
            Orgs where the CALLER is the pending nominee (the accept banner's discovery surface; the caller may be a non-
            member). v1.5.0 M3.
    """

    organizations: list[GetApiMeOrganizationsResponse200OrganizationsItem] | Unset = UNSET
    total: int | Unset = UNSET
    pending_succession_nominations: list[GetApiMeOrganizationsResponse200PendingSuccessionNominationsItem] | Unset = (
        UNSET
    )
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        organizations: list[dict[str, Any]] | Unset = UNSET
        if not isinstance(self.organizations, Unset):
            organizations = []
            for organizations_item_data in self.organizations:
                organizations_item = organizations_item_data.to_dict()
                organizations.append(organizations_item)

        total = self.total

        pending_succession_nominations: list[dict[str, Any]] | Unset = UNSET
        if not isinstance(self.pending_succession_nominations, Unset):
            pending_succession_nominations = []
            for pending_succession_nominations_item_data in self.pending_succession_nominations:
                pending_succession_nominations_item = pending_succession_nominations_item_data.to_dict()
                pending_succession_nominations.append(pending_succession_nominations_item)

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if organizations is not UNSET:
            field_dict["organizations"] = organizations
        if total is not UNSET:
            field_dict["total"] = total
        if pending_succession_nominations is not UNSET:
            field_dict["pending_succession_nominations"] = pending_succession_nominations

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        from ..models.get_api_me_organizations_response_200_organizations_item import (
            GetApiMeOrganizationsResponse200OrganizationsItem,
        )
        from ..models.get_api_me_organizations_response_200_pending_succession_nominations_item import (
            GetApiMeOrganizationsResponse200PendingSuccessionNominationsItem,
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

        _pending_succession_nominations = d.pop("pending_succession_nominations", UNSET)
        pending_succession_nominations: (
            list[GetApiMeOrganizationsResponse200PendingSuccessionNominationsItem] | Unset
        ) = UNSET
        if _pending_succession_nominations is not UNSET:
            pending_succession_nominations = []
            for pending_succession_nominations_item_data in _pending_succession_nominations:
                pending_succession_nominations_item = (
                    GetApiMeOrganizationsResponse200PendingSuccessionNominationsItem.from_dict(
                        pending_succession_nominations_item_data
                    )
                )

                pending_succession_nominations.append(pending_succession_nominations_item)

        get_api_me_organizations_response_200 = cls(
            organizations=organizations,
            total=total,
            pending_succession_nominations=pending_succession_nominations,
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
