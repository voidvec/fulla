from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="GetApiMeOrganizationsResponse200OrganizationsItemSuccessorNominationType0")


@_attrs_define
class GetApiMeOrganizationsResponse200OrganizationsItemSuccessorNominationType0:
    """The org's pending succession nomination; visible to owner/admin callers only (null otherwise).

    Attributes:
        nominee_user_id (int | Unset):
        nominated_by (int | Unset):
        created_at (str | Unset):
    """

    nominee_user_id: int | Unset = UNSET
    nominated_by: int | Unset = UNSET
    created_at: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        nominee_user_id = self.nominee_user_id

        nominated_by = self.nominated_by

        created_at = self.created_at

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if nominee_user_id is not UNSET:
            field_dict["nominee_user_id"] = nominee_user_id
        if nominated_by is not UNSET:
            field_dict["nominated_by"] = nominated_by
        if created_at is not UNSET:
            field_dict["created_at"] = created_at

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        nominee_user_id = d.pop("nominee_user_id", UNSET)

        nominated_by = d.pop("nominated_by", UNSET)

        created_at = d.pop("created_at", UNSET)

        get_api_me_organizations_response_200_organizations_item_successor_nomination_type_0 = cls(
            nominee_user_id=nominee_user_id,
            nominated_by=nominated_by,
            created_at=created_at,
        )

        get_api_me_organizations_response_200_organizations_item_successor_nomination_type_0.additional_properties = d
        return get_api_me_organizations_response_200_organizations_item_successor_nomination_type_0

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
