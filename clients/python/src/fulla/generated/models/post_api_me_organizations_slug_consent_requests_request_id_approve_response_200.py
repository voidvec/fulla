from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar, cast

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200")


@_attrs_define
class PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200:
    """
    Attributes:
        id (int | Unset):
        client_id (str | Unset):
        status (str | Unset):
        scopes (list[str] | Unset):
        message (str | Unset):
    """

    id: int | Unset = UNSET
    client_id: str | Unset = UNSET
    status: str | Unset = UNSET
    scopes: list[str] | Unset = UNSET
    message: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        id = self.id

        client_id = self.client_id

        status = self.status

        scopes: list[str] | Unset = UNSET
        if not isinstance(self.scopes, Unset):
            scopes = self.scopes

        message = self.message

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if id is not UNSET:
            field_dict["id"] = id
        if client_id is not UNSET:
            field_dict["client_id"] = client_id
        if status is not UNSET:
            field_dict["status"] = status
        if scopes is not UNSET:
            field_dict["scopes"] = scopes
        if message is not UNSET:
            field_dict["message"] = message

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        id = d.pop("id", UNSET)

        client_id = d.pop("client_id", UNSET)

        status = d.pop("status", UNSET)

        scopes = cast(list[str], d.pop("scopes", UNSET))

        message = d.pop("message", UNSET)

        post_api_me_organizations_slug_consent_requests_request_id_approve_response_200 = cls(
            id=id,
            client_id=client_id,
            status=status,
            scopes=scopes,
            message=message,
        )

        post_api_me_organizations_slug_consent_requests_request_id_approve_response_200.additional_properties = d
        return post_api_me_organizations_slug_consent_requests_request_id_approve_response_200

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
