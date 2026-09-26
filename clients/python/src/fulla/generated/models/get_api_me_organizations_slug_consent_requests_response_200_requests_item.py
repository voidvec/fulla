from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="GetApiMeOrganizationsSlugConsentRequestsResponse200RequestsItem")


@_attrs_define
class GetApiMeOrganizationsSlugConsentRequestsResponse200RequestsItem:
    """
    Attributes:
        id (int | Unset):
        organization_id (int | Unset):
        client_id (str | Unset):
        requested_by (int | Unset):
        requester_username (str | Unset):
        requested_at (str | Unset):
        status (str | Unset):
    """

    id: int | Unset = UNSET
    organization_id: int | Unset = UNSET
    client_id: str | Unset = UNSET
    requested_by: int | Unset = UNSET
    requester_username: str | Unset = UNSET
    requested_at: str | Unset = UNSET
    status: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        id = self.id

        organization_id = self.organization_id

        client_id = self.client_id

        requested_by = self.requested_by

        requester_username = self.requester_username

        requested_at = self.requested_at

        status = self.status

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if id is not UNSET:
            field_dict["id"] = id
        if organization_id is not UNSET:
            field_dict["organization_id"] = organization_id
        if client_id is not UNSET:
            field_dict["client_id"] = client_id
        if requested_by is not UNSET:
            field_dict["requested_by"] = requested_by
        if requester_username is not UNSET:
            field_dict["requester_username"] = requester_username
        if requested_at is not UNSET:
            field_dict["requested_at"] = requested_at
        if status is not UNSET:
            field_dict["status"] = status

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        id = d.pop("id", UNSET)

        organization_id = d.pop("organization_id", UNSET)

        client_id = d.pop("client_id", UNSET)

        requested_by = d.pop("requested_by", UNSET)

        requester_username = d.pop("requester_username", UNSET)

        requested_at = d.pop("requested_at", UNSET)

        status = d.pop("status", UNSET)

        get_api_me_organizations_slug_consent_requests_response_200_requests_item = cls(
            id=id,
            organization_id=organization_id,
            client_id=client_id,
            requested_by=requested_by,
            requester_username=requester_username,
            requested_at=requested_at,
            status=status,
        )

        get_api_me_organizations_slug_consent_requests_response_200_requests_item.additional_properties = d
        return get_api_me_organizations_slug_consent_requests_response_200_requests_item

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
