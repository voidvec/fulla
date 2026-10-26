from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..models.post_api_admin_claim_mappings_body_include_in import PostApiAdminClaimMappingsBodyIncludeIn
from ..types import UNSET, Unset

T = TypeVar("T", bound="PostApiAdminClaimMappingsBody")


@_attrs_define
class PostApiAdminClaimMappingsBody:
    """
    Attributes:
        scope_name (str):
        claim_name (str):
        source_path (str):
        include_in (PostApiAdminClaimMappingsBodyIncludeIn | Unset):  Default:
            PostApiAdminClaimMappingsBodyIncludeIn.BOTH.
    """

    scope_name: str
    claim_name: str
    source_path: str
    include_in: PostApiAdminClaimMappingsBodyIncludeIn | Unset = PostApiAdminClaimMappingsBodyIncludeIn.BOTH
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        scope_name = self.scope_name

        claim_name = self.claim_name

        source_path = self.source_path

        include_in: str | Unset = UNSET
        if not isinstance(self.include_in, Unset):
            include_in = self.include_in.value

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

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        scope_name = d.pop("scope_name")

        claim_name = d.pop("claim_name")

        source_path = d.pop("source_path")

        _include_in = d.pop("include_in", UNSET)
        include_in: PostApiAdminClaimMappingsBodyIncludeIn | Unset
        if isinstance(_include_in, Unset):
            include_in = UNSET
        else:
            include_in = PostApiAdminClaimMappingsBodyIncludeIn(_include_in)

        post_api_admin_claim_mappings_body = cls(
            scope_name=scope_name,
            claim_name=claim_name,
            source_path=source_path,
            include_in=include_in,
        )

        post_api_admin_claim_mappings_body.additional_properties = d
        return post_api_admin_claim_mappings_body

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
