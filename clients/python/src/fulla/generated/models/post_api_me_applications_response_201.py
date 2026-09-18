from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..models.post_api_me_applications_response_201_client_type import PostApiMeApplicationsResponse201ClientType
from ..types import UNSET, Unset

T = TypeVar("T", bound="PostApiMeApplicationsResponse201")


@_attrs_define
class PostApiMeApplicationsResponse201:
    """
    Attributes:
        client_id (str | Unset):
        client_type (PostApiMeApplicationsResponse201ClientType | Unset):
        client_secret (str | Unset): Present exactly once for CONFIDENTIAL apps.
        org_id (int | Unset):
        message (str | Unset):
    """

    client_id: str | Unset = UNSET
    client_type: PostApiMeApplicationsResponse201ClientType | Unset = UNSET
    client_secret: str | Unset = UNSET
    org_id: int | Unset = UNSET
    message: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        client_id = self.client_id

        client_type: str | Unset = UNSET
        if not isinstance(self.client_type, Unset):
            client_type = self.client_type.value

        client_secret = self.client_secret

        org_id = self.org_id

        message = self.message

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if client_id is not UNSET:
            field_dict["client_id"] = client_id
        if client_type is not UNSET:
            field_dict["client_type"] = client_type
        if client_secret is not UNSET:
            field_dict["client_secret"] = client_secret
        if org_id is not UNSET:
            field_dict["org_id"] = org_id
        if message is not UNSET:
            field_dict["message"] = message

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        client_id = d.pop("client_id", UNSET)

        _client_type = d.pop("client_type", UNSET)
        client_type: PostApiMeApplicationsResponse201ClientType | Unset
        if isinstance(_client_type, Unset):
            client_type = UNSET
        else:
            client_type = PostApiMeApplicationsResponse201ClientType(_client_type)

        client_secret = d.pop("client_secret", UNSET)

        org_id = d.pop("org_id", UNSET)

        message = d.pop("message", UNSET)

        post_api_me_applications_response_201 = cls(
            client_id=client_id,
            client_type=client_type,
            client_secret=client_secret,
            org_id=org_id,
            message=message,
        )

        post_api_me_applications_response_201.additional_properties = d
        return post_api_me_applications_response_201

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
