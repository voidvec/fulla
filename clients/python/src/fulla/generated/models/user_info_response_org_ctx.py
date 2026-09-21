from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar, cast

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="UserInfoResponseOrgCtx")


@_attrs_define
class UserInfoResponseOrgCtx:
    """v1.5.0: the ACTIVE organization context (id + name + the user's current roles in it); active-org-only, never the
    full membership list.

        Attributes:
            org_id (int | Unset):
            org_name (str | Unset):
            roles (list[str] | Unset):
    """

    org_id: int | Unset = UNSET
    org_name: str | Unset = UNSET
    roles: list[str] | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        org_id = self.org_id

        org_name = self.org_name

        roles: list[str] | Unset = UNSET
        if not isinstance(self.roles, Unset):
            roles = self.roles

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if org_id is not UNSET:
            field_dict["org_id"] = org_id
        if org_name is not UNSET:
            field_dict["org_name"] = org_name
        if roles is not UNSET:
            field_dict["roles"] = roles

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        org_id = d.pop("org_id", UNSET)

        org_name = d.pop("org_name", UNSET)

        roles = cast(list[str], d.pop("roles", UNSET))

        user_info_response_org_ctx = cls(
            org_id=org_id,
            org_name=org_name,
            roles=roles,
        )

        user_info_response_org_ctx.additional_properties = d
        return user_info_response_org_ctx

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
