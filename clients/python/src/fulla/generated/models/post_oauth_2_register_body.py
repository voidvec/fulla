from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar, cast

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..models.post_oauth_2_register_body_client_type import PostOauth2RegisterBodyClientType
from ..types import UNSET, Unset

T = TypeVar("T", bound="PostOauth2RegisterBody")


@_attrs_define
class PostOauth2RegisterBody:
    """
    Attributes:
        client_name (str | Unset):
        client_type (PostOauth2RegisterBodyClientType | Unset):
        token_endpoint_auth_method (str | Unset):
        redirect_uris (list[str] | Unset):
        grant_types (list[str] | Unset):
        post_logout_redirect_uris (list[str] | Unset): P0-7b: dedicated post-logout redirect registrations (comma-joined
            at rest). Empty/unset: end_session validates post_logout_redirect_uri against the login redirect_uris
            (pre-v1.5.0 behavior).
    """

    client_name: str | Unset = UNSET
    client_type: PostOauth2RegisterBodyClientType | Unset = UNSET
    token_endpoint_auth_method: str | Unset = UNSET
    redirect_uris: list[str] | Unset = UNSET
    grant_types: list[str] | Unset = UNSET
    post_logout_redirect_uris: list[str] | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        client_name = self.client_name

        client_type: str | Unset = UNSET
        if not isinstance(self.client_type, Unset):
            client_type = self.client_type.value

        token_endpoint_auth_method = self.token_endpoint_auth_method

        redirect_uris: list[str] | Unset = UNSET
        if not isinstance(self.redirect_uris, Unset):
            redirect_uris = self.redirect_uris

        grant_types: list[str] | Unset = UNSET
        if not isinstance(self.grant_types, Unset):
            grant_types = self.grant_types

        post_logout_redirect_uris: list[str] | Unset = UNSET
        if not isinstance(self.post_logout_redirect_uris, Unset):
            post_logout_redirect_uris = self.post_logout_redirect_uris

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if client_name is not UNSET:
            field_dict["client_name"] = client_name
        if client_type is not UNSET:
            field_dict["client_type"] = client_type
        if token_endpoint_auth_method is not UNSET:
            field_dict["token_endpoint_auth_method"] = token_endpoint_auth_method
        if redirect_uris is not UNSET:
            field_dict["redirect_uris"] = redirect_uris
        if grant_types is not UNSET:
            field_dict["grant_types"] = grant_types
        if post_logout_redirect_uris is not UNSET:
            field_dict["post_logout_redirect_uris"] = post_logout_redirect_uris

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        client_name = d.pop("client_name", UNSET)

        _client_type = d.pop("client_type", UNSET)
        client_type: PostOauth2RegisterBodyClientType | Unset
        if isinstance(_client_type, Unset):
            client_type = UNSET
        else:
            client_type = PostOauth2RegisterBodyClientType(_client_type)

        token_endpoint_auth_method = d.pop("token_endpoint_auth_method", UNSET)

        redirect_uris = cast(list[str], d.pop("redirect_uris", UNSET))

        grant_types = cast(list[str], d.pop("grant_types", UNSET))

        post_logout_redirect_uris = cast(list[str], d.pop("post_logout_redirect_uris", UNSET))

        post_oauth_2_register_body = cls(
            client_name=client_name,
            client_type=client_type,
            token_endpoint_auth_method=token_endpoint_auth_method,
            redirect_uris=redirect_uris,
            grant_types=grant_types,
            post_logout_redirect_uris=post_logout_redirect_uris,
        )

        post_oauth_2_register_body.additional_properties = d
        return post_oauth_2_register_body

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
