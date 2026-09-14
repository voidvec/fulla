from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

T = TypeVar("T", bound="SocialLoginTokenResponse")


@_attrs_define
class SocialLoginTokenResponse:
    """First-party token pair issued by a social login endpoint (/api/{github,google,wechat}/login) once the provider
    identity is linked to a local account (or auto-created on first login). Issued for the configured first-party client
    (external_auth.social_token_client_id, default fulla-portal) with scope "openid profile email". Recorded as a
    SOCIAL_LOGIN_TOKEN_ISSUED audit event (NOT a consent row) — an explicit social-consent interaction is a registered
    follow-up.

        Attributes:
            access_token (str):
            token_type (str): Always "Bearer".
            expires_in (int):
            refresh_token (str):
    """

    access_token: str
    token_type: str
    expires_in: int
    refresh_token: str
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        access_token = self.access_token

        token_type = self.token_type

        expires_in = self.expires_in

        refresh_token = self.refresh_token

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "access_token": access_token,
                "token_type": token_type,
                "expires_in": expires_in,
                "refresh_token": refresh_token,
            }
        )

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        access_token = d.pop("access_token")

        token_type = d.pop("token_type")

        expires_in = d.pop("expires_in")

        refresh_token = d.pop("refresh_token")

        social_login_token_response = cls(
            access_token=access_token,
            token_type=token_type,
            expires_in=expires_in,
            refresh_token=refresh_token,
        )

        social_login_token_response.additional_properties = d
        return social_login_token_response

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
