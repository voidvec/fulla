from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="PutApiAdminUsersUserIdBody")


@_attrs_define
class PutApiAdminUsersUserIdBody:
    """
    Attributes:
        username (str | Unset):
        email (str | Unset):
        email_verified (bool | Unset):
        mfa_enabled (bool | Unset):
        must_change_password (bool | Unset): Set/clear the forced password-change flag (#145); enforcement starts at the
            user's next login.
        locked (bool | Unset):
    """

    username: str | Unset = UNSET
    email: str | Unset = UNSET
    email_verified: bool | Unset = UNSET
    mfa_enabled: bool | Unset = UNSET
    must_change_password: bool | Unset = UNSET
    locked: bool | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        username = self.username

        email = self.email

        email_verified = self.email_verified

        mfa_enabled = self.mfa_enabled

        must_change_password = self.must_change_password

        locked = self.locked

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if username is not UNSET:
            field_dict["username"] = username
        if email is not UNSET:
            field_dict["email"] = email
        if email_verified is not UNSET:
            field_dict["email_verified"] = email_verified
        if mfa_enabled is not UNSET:
            field_dict["mfa_enabled"] = mfa_enabled
        if must_change_password is not UNSET:
            field_dict["must_change_password"] = must_change_password
        if locked is not UNSET:
            field_dict["locked"] = locked

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        username = d.pop("username", UNSET)

        email = d.pop("email", UNSET)

        email_verified = d.pop("email_verified", UNSET)

        mfa_enabled = d.pop("mfa_enabled", UNSET)

        must_change_password = d.pop("must_change_password", UNSET)

        locked = d.pop("locked", UNSET)

        put_api_admin_users_user_id_body = cls(
            username=username,
            email=email,
            email_verified=email_verified,
            mfa_enabled=mfa_enabled,
            must_change_password=must_change_password,
            locked=locked,
        )

        put_api_admin_users_user_id_body.additional_properties = d
        return put_api_admin_users_user_id_body

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
