from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="PutApiAdminScopesScopeIdBody")


@_attrs_define
class PutApiAdminScopesScopeIdBody:
    """
    Attributes:
        description (str | Unset):
        description_zh (str | Unset): P1-1 (#279): zh-CN consent-screen copy; the consent context serves it for zh
            pages. Empty clears it. Description changes are audited with a before/after diff.
        is_default (bool | Unset):
        mapped_role (str | Unset):
        requires_admin_role (bool | Unset):
    """

    description: str | Unset = UNSET
    description_zh: str | Unset = UNSET
    is_default: bool | Unset = UNSET
    mapped_role: str | Unset = UNSET
    requires_admin_role: bool | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        description = self.description

        description_zh = self.description_zh

        is_default = self.is_default

        mapped_role = self.mapped_role

        requires_admin_role = self.requires_admin_role

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if description is not UNSET:
            field_dict["description"] = description
        if description_zh is not UNSET:
            field_dict["description_zh"] = description_zh
        if is_default is not UNSET:
            field_dict["is_default"] = is_default
        if mapped_role is not UNSET:
            field_dict["mapped_role"] = mapped_role
        if requires_admin_role is not UNSET:
            field_dict["requires_admin_role"] = requires_admin_role

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        description = d.pop("description", UNSET)

        description_zh = d.pop("description_zh", UNSET)

        is_default = d.pop("is_default", UNSET)

        mapped_role = d.pop("mapped_role", UNSET)

        requires_admin_role = d.pop("requires_admin_role", UNSET)

        put_api_admin_scopes_scope_id_body = cls(
            description=description,
            description_zh=description_zh,
            is_default=is_default,
            mapped_role=mapped_role,
            requires_admin_role=requires_admin_role,
        )

        put_api_admin_scopes_scope_id_body.additional_properties = d
        return put_api_admin_scopes_scope_id_body

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
