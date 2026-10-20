from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..models.post_api_admin_clients_body_client_type import PostApiAdminClientsBodyClientType
from ..types import UNSET, Unset

T = TypeVar("T", bound="PostApiAdminClientsBody")


@_attrs_define
class PostApiAdminClientsBody:
    """
    Attributes:
        name (str):
        client_type (PostApiAdminClientsBodyClientType | Unset):  Default:
            PostApiAdminClientsBodyClientType.CONFIDENTIAL.
        redirect_uris (str | Unset): Comma-separated redirect URIs (https required).
        allowed_grant_types (str | Unset):  Default: 'authorization_code'.
        token_endpoint_auth_method (str | Unset):
        backchannel_logout_uri (str | Unset): OIDC Back-Channel Logout 1.0 RP endpoint. MUST use https (spec §2.3). When
            set, the OP POSTs a signed logout_token here on user logout.
        logo_uri (str | Unset): P1-1 (#279): client brand image for the login/consent screens. https-only (loopback
            hosts exempt for dev); no userinfo/fragment. NOT the redirect registration rule.
    """

    name: str
    client_type: PostApiAdminClientsBodyClientType | Unset = PostApiAdminClientsBodyClientType.CONFIDENTIAL
    redirect_uris: str | Unset = UNSET
    allowed_grant_types: str | Unset = "authorization_code"
    token_endpoint_auth_method: str | Unset = UNSET
    backchannel_logout_uri: str | Unset = UNSET
    logo_uri: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        name = self.name

        client_type: str | Unset = UNSET
        if not isinstance(self.client_type, Unset):
            client_type = self.client_type.value

        redirect_uris = self.redirect_uris

        allowed_grant_types = self.allowed_grant_types

        token_endpoint_auth_method = self.token_endpoint_auth_method

        backchannel_logout_uri = self.backchannel_logout_uri

        logo_uri = self.logo_uri

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "name": name,
            }
        )
        if client_type is not UNSET:
            field_dict["client_type"] = client_type
        if redirect_uris is not UNSET:
            field_dict["redirect_uris"] = redirect_uris
        if allowed_grant_types is not UNSET:
            field_dict["allowed_grant_types"] = allowed_grant_types
        if token_endpoint_auth_method is not UNSET:
            field_dict["token_endpoint_auth_method"] = token_endpoint_auth_method
        if backchannel_logout_uri is not UNSET:
            field_dict["backchannel_logout_uri"] = backchannel_logout_uri
        if logo_uri is not UNSET:
            field_dict["logo_uri"] = logo_uri

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        name = d.pop("name")

        _client_type = d.pop("client_type", UNSET)
        client_type: PostApiAdminClientsBodyClientType | Unset
        if isinstance(_client_type, Unset):
            client_type = UNSET
        else:
            client_type = PostApiAdminClientsBodyClientType(_client_type)

        redirect_uris = d.pop("redirect_uris", UNSET)

        allowed_grant_types = d.pop("allowed_grant_types", UNSET)

        token_endpoint_auth_method = d.pop("token_endpoint_auth_method", UNSET)

        backchannel_logout_uri = d.pop("backchannel_logout_uri", UNSET)

        logo_uri = d.pop("logo_uri", UNSET)

        post_api_admin_clients_body = cls(
            name=name,
            client_type=client_type,
            redirect_uris=redirect_uris,
            allowed_grant_types=allowed_grant_types,
            token_endpoint_auth_method=token_endpoint_auth_method,
            backchannel_logout_uri=backchannel_logout_uri,
            logo_uri=logo_uri,
        )

        post_api_admin_clients_body.additional_properties = d
        return post_api_admin_clients_body

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
