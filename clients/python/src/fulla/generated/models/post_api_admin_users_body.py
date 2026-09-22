from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar, cast

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="PostApiAdminUsersBody")


@_attrs_define
class PostApiAdminUsersBody:
    """
    Attributes:
        username (str):
        password (str):
        email (str | Unset):
        email_verified (bool | Unset):
        mfa_enabled (bool | Unset):
        must_change_password (bool | Unset): Force a password change at first login (#145); while flagged, no
            authorization codes are issued for the account. Default false.
        roles (list[str] | Unset):
    """

    username: str
    password: str
    email: str | Unset = UNSET
    email_verified: bool | Unset = UNSET
    mfa_enabled: bool | Unset = UNSET
    must_change_password: bool | Unset = UNSET
    roles: list[str] | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        username = self.username

        password = self.password

        email = self.email

        email_verified = self.email_verified

        mfa_enabled = self.mfa_enabled

        must_change_password = self.must_change_password

        roles: list[str] | Unset = UNSET
        if not isinstance(self.roles, Unset):
            roles = self.roles

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "username": username,
                "password": password,
            }
        )
        if email is not UNSET:
            field_dict["email"] = email
        if email_verified is not UNSET:
            field_dict["email_verified"] = email_verified
        if mfa_enabled is not UNSET:
            field_dict["mfa_enabled"] = mfa_enabled
        if must_change_password is not UNSET:
            field_dict["must_change_password"] = must_change_password
        if roles is not UNSET:
            field_dict["roles"] = roles

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        username = d.pop("username")

        password = d.pop("password")

        email = d.pop("email", UNSET)

        email_verified = d.pop("email_verified", UNSET)

        mfa_enabled = d.pop("mfa_enabled", UNSET)

        must_change_password = d.pop("must_change_password", UNSET)

        roles = cast(list[str], d.pop("roles", UNSET))

        post_api_admin_users_body = cls(
            username=username,
            password=password,
            email=email,
            email_verified=email_verified,
            mfa_enabled=mfa_enabled,
            must_change_password=must_change_password,
            roles=roles,
        )

        post_api_admin_users_body.additional_properties = d
        return post_api_admin_users_body

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
