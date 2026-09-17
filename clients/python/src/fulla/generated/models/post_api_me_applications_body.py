from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar, cast

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..models.post_api_me_applications_body_client_type import PostApiMeApplicationsBodyClientType
from ..types import UNSET, Unset

T = TypeVar("T", bound="PostApiMeApplicationsBody")


@_attrs_define
class PostApiMeApplicationsBody:
    """
    Attributes:
        name (str | Unset):
        client_type (PostApiMeApplicationsBodyClientType | Unset):
        redirect_uris (list[str] | Unset):
        scopes (list[str] | Unset):
        allowed_grant_types (list[str] | Unset):
        org_slug (str | Unset): Register under this organization (caller must be its owner/admin); omit for a personal
            application.
    """

    name: str | Unset = UNSET
    client_type: PostApiMeApplicationsBodyClientType | Unset = UNSET
    redirect_uris: list[str] | Unset = UNSET
    scopes: list[str] | Unset = UNSET
    allowed_grant_types: list[str] | Unset = UNSET
    org_slug: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        name = self.name

        client_type: str | Unset = UNSET
        if not isinstance(self.client_type, Unset):
            client_type = self.client_type.value

        redirect_uris: list[str] | Unset = UNSET
        if not isinstance(self.redirect_uris, Unset):
            redirect_uris = self.redirect_uris

        scopes: list[str] | Unset = UNSET
        if not isinstance(self.scopes, Unset):
            scopes = self.scopes

        allowed_grant_types: list[str] | Unset = UNSET
        if not isinstance(self.allowed_grant_types, Unset):
            allowed_grant_types = self.allowed_grant_types

        org_slug = self.org_slug

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if name is not UNSET:
            field_dict["name"] = name
        if client_type is not UNSET:
            field_dict["client_type"] = client_type
        if redirect_uris is not UNSET:
            field_dict["redirect_uris"] = redirect_uris
        if scopes is not UNSET:
            field_dict["scopes"] = scopes
        if allowed_grant_types is not UNSET:
            field_dict["allowed_grant_types"] = allowed_grant_types
        if org_slug is not UNSET:
            field_dict["org_slug"] = org_slug

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        name = d.pop("name", UNSET)

        _client_type = d.pop("client_type", UNSET)
        client_type: PostApiMeApplicationsBodyClientType | Unset
        if isinstance(_client_type, Unset):
            client_type = UNSET
        else:
            client_type = PostApiMeApplicationsBodyClientType(_client_type)

        redirect_uris = cast(list[str], d.pop("redirect_uris", UNSET))

        scopes = cast(list[str], d.pop("scopes", UNSET))

        allowed_grant_types = cast(list[str], d.pop("allowed_grant_types", UNSET))

        org_slug = d.pop("org_slug", UNSET)

        post_api_me_applications_body = cls(
            name=name,
            client_type=client_type,
            redirect_uris=redirect_uris,
            scopes=scopes,
            allowed_grant_types=allowed_grant_types,
            org_slug=org_slug,
        )

        post_api_me_applications_body.additional_properties = d
        return post_api_me_applications_body

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
