from __future__ import annotations

from collections.abc import Mapping
from typing import TYPE_CHECKING, Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

if TYPE_CHECKING:
    from ..models.get_api_me_organizations_slug_consent_requests_response_200_requests_item import (
        GetApiMeOrganizationsSlugConsentRequestsResponse200RequestsItem,
    )


T = TypeVar("T", bound="GetApiMeOrganizationsSlugConsentRequestsResponse200")


@_attrs_define
class GetApiMeOrganizationsSlugConsentRequestsResponse200:
    """
    Attributes:
        slug (str | Unset):
        requests (list[GetApiMeOrganizationsSlugConsentRequestsResponse200RequestsItem] | Unset):
        total (int | Unset):
    """

    slug: str | Unset = UNSET
    requests: list[GetApiMeOrganizationsSlugConsentRequestsResponse200RequestsItem] | Unset = UNSET
    total: int | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        slug = self.slug

        requests: list[dict[str, Any]] | Unset = UNSET
        if not isinstance(self.requests, Unset):
            requests = []
            for requests_item_data in self.requests:
                requests_item = requests_item_data.to_dict()
                requests.append(requests_item)

        total = self.total

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if slug is not UNSET:
            field_dict["slug"] = slug
        if requests is not UNSET:
            field_dict["requests"] = requests
        if total is not UNSET:
            field_dict["total"] = total

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        from ..models.get_api_me_organizations_slug_consent_requests_response_200_requests_item import (
            GetApiMeOrganizationsSlugConsentRequestsResponse200RequestsItem,
        )

        d = dict(src_dict)
        slug = d.pop("slug", UNSET)

        _requests = d.pop("requests", UNSET)
        requests: list[GetApiMeOrganizationsSlugConsentRequestsResponse200RequestsItem] | Unset = UNSET
        if _requests is not UNSET:
            requests = []
            for requests_item_data in _requests:
                requests_item = GetApiMeOrganizationsSlugConsentRequestsResponse200RequestsItem.from_dict(
                    requests_item_data
                )

                requests.append(requests_item)

        total = d.pop("total", UNSET)

        get_api_me_organizations_slug_consent_requests_response_200 = cls(
            slug=slug,
            requests=requests,
            total=total,
        )

        get_api_me_organizations_slug_consent_requests_response_200.additional_properties = d
        return get_api_me_organizations_slug_consent_requests_response_200

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
