from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="DeleteApiMeOrganizationsSlugConsentRequestsRequestIdResponse200")


@_attrs_define
class DeleteApiMeOrganizationsSlugConsentRequestsRequestIdResponse200:
    """
    Attributes:
        id (int | Unset):
        status (str | Unset):
        message (str | Unset):
    """

    id: int | Unset = UNSET
    status: str | Unset = UNSET
    message: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        id = self.id

        status = self.status

        message = self.message

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if id is not UNSET:
            field_dict["id"] = id
        if status is not UNSET:
            field_dict["status"] = status
        if message is not UNSET:
            field_dict["message"] = message

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        id = d.pop("id", UNSET)

        status = d.pop("status", UNSET)

        message = d.pop("message", UNSET)

        delete_api_me_organizations_slug_consent_requests_request_id_response_200 = cls(
            id=id,
            status=status,
            message=message,
        )

        delete_api_me_organizations_slug_consent_requests_request_id_response_200.additional_properties = d
        return delete_api_me_organizations_slug_consent_requests_request_id_response_200

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
