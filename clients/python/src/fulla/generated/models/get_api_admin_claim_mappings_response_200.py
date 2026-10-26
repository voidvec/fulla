from __future__ import annotations

from collections.abc import Mapping
from typing import TYPE_CHECKING, Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

if TYPE_CHECKING:
    from ..models.get_api_admin_claim_mappings_response_200_mappings_item import (
        GetApiAdminClaimMappingsResponse200MappingsItem,
    )


T = TypeVar("T", bound="GetApiAdminClaimMappingsResponse200")


@_attrs_define
class GetApiAdminClaimMappingsResponse200:
    """
    Attributes:
        status (str | Unset):
        total (int | Unset):
        mappings (list[GetApiAdminClaimMappingsResponse200MappingsItem] | Unset):
    """

    status: str | Unset = UNSET
    total: int | Unset = UNSET
    mappings: list[GetApiAdminClaimMappingsResponse200MappingsItem] | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        status = self.status

        total = self.total

        mappings: list[dict[str, Any]] | Unset = UNSET
        if not isinstance(self.mappings, Unset):
            mappings = []
            for mappings_item_data in self.mappings:
                mappings_item = mappings_item_data.to_dict()
                mappings.append(mappings_item)

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if status is not UNSET:
            field_dict["status"] = status
        if total is not UNSET:
            field_dict["total"] = total
        if mappings is not UNSET:
            field_dict["mappings"] = mappings

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        from ..models.get_api_admin_claim_mappings_response_200_mappings_item import (
            GetApiAdminClaimMappingsResponse200MappingsItem,
        )

        d = dict(src_dict)
        status = d.pop("status", UNSET)

        total = d.pop("total", UNSET)

        _mappings = d.pop("mappings", UNSET)
        mappings: list[GetApiAdminClaimMappingsResponse200MappingsItem] | Unset = UNSET
        if _mappings is not UNSET:
            mappings = []
            for mappings_item_data in _mappings:
                mappings_item = GetApiAdminClaimMappingsResponse200MappingsItem.from_dict(mappings_item_data)

                mappings.append(mappings_item)

        get_api_admin_claim_mappings_response_200 = cls(
            status=status,
            total=total,
            mappings=mappings,
        )

        get_api_admin_claim_mappings_response_200.additional_properties = d
        return get_api_admin_claim_mappings_response_200

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
