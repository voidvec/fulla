from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar, cast

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..models.get_api_me_applications_response_200_applications_item_client_type import (
    GetApiMeApplicationsResponse200ApplicationsItemClientType,
)
from ..models.get_api_me_applications_response_200_applications_item_status import (
    GetApiMeApplicationsResponse200ApplicationsItemStatus,
)
from ..types import UNSET, Unset

T = TypeVar("T", bound="GetApiMeApplicationsResponse200ApplicationsItem")


@_attrs_define
class GetApiMeApplicationsResponse200ApplicationsItem:
    """
    Attributes:
        client_id (str | Unset):
        name (str | Unset):
        client_type (GetApiMeApplicationsResponse200ApplicationsItemClientType | Unset):
        status (GetApiMeApplicationsResponse200ApplicationsItemStatus | Unset):
        org_id (int | None | Unset):
        creator_user_id (int | Unset):
        redirect_uris (list[str] | Unset):
        allowed_grant_types (list[str] | Unset):
        created_at (str | Unset):
    """

    client_id: str | Unset = UNSET
    name: str | Unset = UNSET
    client_type: GetApiMeApplicationsResponse200ApplicationsItemClientType | Unset = UNSET
    status: GetApiMeApplicationsResponse200ApplicationsItemStatus | Unset = UNSET
    org_id: int | None | Unset = UNSET
    creator_user_id: int | Unset = UNSET
    redirect_uris: list[str] | Unset = UNSET
    allowed_grant_types: list[str] | Unset = UNSET
    created_at: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        client_id = self.client_id

        name = self.name

        client_type: str | Unset = UNSET
        if not isinstance(self.client_type, Unset):
            client_type = self.client_type.value

        status: str | Unset = UNSET
        if not isinstance(self.status, Unset):
            status = self.status.value

        org_id: int | None | Unset
        if isinstance(self.org_id, Unset):
            org_id = UNSET
        else:
            org_id = self.org_id

        creator_user_id = self.creator_user_id

        redirect_uris: list[str] | Unset = UNSET
        if not isinstance(self.redirect_uris, Unset):
            redirect_uris = self.redirect_uris

        allowed_grant_types: list[str] | Unset = UNSET
        if not isinstance(self.allowed_grant_types, Unset):
            allowed_grant_types = self.allowed_grant_types

        created_at = self.created_at

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if client_id is not UNSET:
            field_dict["client_id"] = client_id
        if name is not UNSET:
            field_dict["name"] = name
        if client_type is not UNSET:
            field_dict["client_type"] = client_type
        if status is not UNSET:
            field_dict["status"] = status
        if org_id is not UNSET:
            field_dict["org_id"] = org_id
        if creator_user_id is not UNSET:
            field_dict["creator_user_id"] = creator_user_id
        if redirect_uris is not UNSET:
            field_dict["redirect_uris"] = redirect_uris
        if allowed_grant_types is not UNSET:
            field_dict["allowed_grant_types"] = allowed_grant_types
        if created_at is not UNSET:
            field_dict["created_at"] = created_at

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        client_id = d.pop("client_id", UNSET)

        name = d.pop("name", UNSET)

        _client_type = d.pop("client_type", UNSET)
        client_type: GetApiMeApplicationsResponse200ApplicationsItemClientType | Unset
        if isinstance(_client_type, Unset):
            client_type = UNSET
        else:
            client_type = GetApiMeApplicationsResponse200ApplicationsItemClientType(_client_type)

        _status = d.pop("status", UNSET)
        status: GetApiMeApplicationsResponse200ApplicationsItemStatus | Unset
        if isinstance(_status, Unset):
            status = UNSET
        else:
            status = GetApiMeApplicationsResponse200ApplicationsItemStatus(_status)

        def _parse_org_id(data: object) -> int | None | Unset:
            if data is None:
                return data
            if isinstance(data, Unset):
                return data
            return cast(int | None | Unset, data)

        org_id = _parse_org_id(d.pop("org_id", UNSET))

        creator_user_id = d.pop("creator_user_id", UNSET)

        redirect_uris = cast(list[str], d.pop("redirect_uris", UNSET))

        allowed_grant_types = cast(list[str], d.pop("allowed_grant_types", UNSET))

        created_at = d.pop("created_at", UNSET)

        get_api_me_applications_response_200_applications_item = cls(
            client_id=client_id,
            name=name,
            client_type=client_type,
            status=status,
            org_id=org_id,
            creator_user_id=creator_user_id,
            redirect_uris=redirect_uris,
            allowed_grant_types=allowed_grant_types,
            created_at=created_at,
        )

        get_api_me_applications_response_200_applications_item.additional_properties = d
        return get_api_me_applications_response_200_applications_item

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
