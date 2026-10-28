from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="PutApiAdminClaimMappingsMappingIdBody")


@_attrs_define
class PutApiAdminClaimMappingsMappingIdBody:
    """
    Attributes:
        scope_name (str):
        claim_name (str):
        source_path (str):
        include_in (str | Unset):
        enabled (bool | Unset):
    """

    scope_name: str
    claim_name: str
    source_path: str
    include_in: str | Unset = UNSET
    enabled: bool | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        scope_name = self.scope_name

        claim_name = self.claim_name

        source_path = self.source_path

        include_in = self.include_in

        enabled = self.enabled

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "scope_name": scope_name,
                "claim_name": claim_name,
                "source_path": source_path,
            }
        )
        if include_in is not UNSET:
            field_dict["include_in"] = include_in
        if enabled is not UNSET:
            field_dict["enabled"] = enabled

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        scope_name = d.pop("scope_name")

        claim_name = d.pop("claim_name")

        source_path = d.pop("source_path")

        include_in = d.pop("include_in", UNSET)

        enabled = d.pop("enabled", UNSET)

        put_api_admin_claim_mappings_mapping_id_body = cls(
            scope_name=scope_name,
            claim_name=claim_name,
            source_path=source_path,
            include_in=include_in,
            enabled=enabled,
        )

        put_api_admin_claim_mappings_mapping_id_body.additional_properties = d
        return put_api_admin_claim_mappings_mapping_id_body

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
