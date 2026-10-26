from __future__ import annotations

from collections.abc import Mapping
from typing import TYPE_CHECKING, Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

if TYPE_CHECKING:
    from ..models.get_api_me_profile_custom_attributes_response_200_custom_attributes import (
        GetApiMeProfileCustomAttributesResponse200CustomAttributes,
    )


T = TypeVar("T", bound="GetApiMeProfileCustomAttributesResponse200")


@_attrs_define
class GetApiMeProfileCustomAttributesResponse200:
    """
    Attributes:
        status (str | Unset):
        custom_attributes (GetApiMeProfileCustomAttributesResponse200CustomAttributes | Unset):
    """

    status: str | Unset = UNSET
    custom_attributes: GetApiMeProfileCustomAttributesResponse200CustomAttributes | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        status = self.status

        custom_attributes: dict[str, Any] | Unset = UNSET
        if not isinstance(self.custom_attributes, Unset):
            custom_attributes = self.custom_attributes.to_dict()

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if status is not UNSET:
            field_dict["status"] = status
        if custom_attributes is not UNSET:
            field_dict["custom_attributes"] = custom_attributes

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        from ..models.get_api_me_profile_custom_attributes_response_200_custom_attributes import (
            GetApiMeProfileCustomAttributesResponse200CustomAttributes,
        )

        d = dict(src_dict)
        status = d.pop("status", UNSET)

        _custom_attributes = d.pop("custom_attributes", UNSET)
        custom_attributes: GetApiMeProfileCustomAttributesResponse200CustomAttributes | Unset
        if isinstance(_custom_attributes, Unset):
            custom_attributes = UNSET
        else:
            custom_attributes = GetApiMeProfileCustomAttributesResponse200CustomAttributes.from_dict(_custom_attributes)

        get_api_me_profile_custom_attributes_response_200 = cls(
            status=status,
            custom_attributes=custom_attributes,
        )

        get_api_me_profile_custom_attributes_response_200.additional_properties = d
        return get_api_me_profile_custom_attributes_response_200

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
