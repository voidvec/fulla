from __future__ import annotations

from collections.abc import Mapping
from typing import TYPE_CHECKING, Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

if TYPE_CHECKING:
    from ..models.upstream_provider import UpstreamProvider


T = TypeVar("T", bound="GetApiAdminUpstreamProvidersResponse200")


@_attrs_define
class GetApiAdminUpstreamProvidersResponse200:
    """
    Attributes:
        status (str | Unset):
        total (int | Unset):
        providers (list[UpstreamProvider] | Unset):
    """

    status: str | Unset = UNSET
    total: int | Unset = UNSET
    providers: list[UpstreamProvider] | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        status = self.status

        total = self.total

        providers: list[dict[str, Any]] | Unset = UNSET
        if not isinstance(self.providers, Unset):
            providers = []
            for providers_item_data in self.providers:
                providers_item = providers_item_data.to_dict()
                providers.append(providers_item)

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if status is not UNSET:
            field_dict["status"] = status
        if total is not UNSET:
            field_dict["total"] = total
        if providers is not UNSET:
            field_dict["providers"] = providers

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        from ..models.upstream_provider import UpstreamProvider

        d = dict(src_dict)
        status = d.pop("status", UNSET)

        total = d.pop("total", UNSET)

        _providers = d.pop("providers", UNSET)
        providers: list[UpstreamProvider] | Unset = UNSET
        if _providers is not UNSET:
            providers = []
            for providers_item_data in _providers:
                providers_item = UpstreamProvider.from_dict(providers_item_data)

                providers.append(providers_item)

        get_api_admin_upstream_providers_response_200 = cls(
            status=status,
            total=total,
            providers=providers,
        )

        get_api_admin_upstream_providers_response_200.additional_properties = d
        return get_api_admin_upstream_providers_response_200

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
