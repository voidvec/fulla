from __future__ import annotations

from collections.abc import Mapping
from typing import TYPE_CHECKING, Any, TypeVar, cast

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

if TYPE_CHECKING:
    from ..models.user_info_response_org_ctx import UserInfoResponseOrgCtx


T = TypeVar("T", bound="UserInfoResponse")


@_attrs_define
class UserInfoResponse:
    """OIDC Core §5.3 userinfo claims, scope-gated per §5.4 (P0-2): profile claims (name/preferred_username/picture)
    require the profile scope; email/email_verified require the email scope; sub is unconditional. email_verified
    present iff email present; roles present iff non-empty. org_ctx present iff the token was issued in org context,
    carries the org scope, and the user is still a member (v1.4.0; membership is re-checked in real time).

        Attributes:
            sub (str): Subject (public user id, stringified).
            name (str): display_name when set, else username, else email (profile scope).
            preferred_username (str | Unset): Standard OIDC claim name (profile scope); the legacy username key dual-emits
                alongside it until v1.6.0.
            username (str | Unset): Legacy key (deprecated, removed in v1.6.0) — use preferred_username (profile scope).
            picture (str | Unset): Avatar URL (profile scope).
            email (str | Unset): Email address (email scope).
            email_verified (bool | Unset): Email verification status (email scope).
            roles (list[str] | Unset):
            org_ctx (UserInfoResponseOrgCtx | Unset): v1.4.0: the ACTIVE organization context (id + name + the user's
                current roles in it); active-org-only, never the full membership list.
    """

    sub: str
    name: str
    preferred_username: str | Unset = UNSET
    username: str | Unset = UNSET
    picture: str | Unset = UNSET
    email: str | Unset = UNSET
    email_verified: bool | Unset = UNSET
    roles: list[str] | Unset = UNSET
    org_ctx: UserInfoResponseOrgCtx | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        sub = self.sub

        name = self.name

        preferred_username = self.preferred_username

        username = self.username

        picture = self.picture

        email = self.email

        email_verified = self.email_verified

        roles: list[str] | Unset = UNSET
        if not isinstance(self.roles, Unset):
            roles = self.roles

        org_ctx: dict[str, Any] | Unset = UNSET
        if not isinstance(self.org_ctx, Unset):
            org_ctx = self.org_ctx.to_dict()

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update(
            {
                "sub": sub,
                "name": name,
            }
        )
        if preferred_username is not UNSET:
            field_dict["preferred_username"] = preferred_username
        if username is not UNSET:
            field_dict["username"] = username
        if picture is not UNSET:
            field_dict["picture"] = picture
        if email is not UNSET:
            field_dict["email"] = email
        if email_verified is not UNSET:
            field_dict["email_verified"] = email_verified
        if roles is not UNSET:
            field_dict["roles"] = roles
        if org_ctx is not UNSET:
            field_dict["org_ctx"] = org_ctx

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        from ..models.user_info_response_org_ctx import UserInfoResponseOrgCtx

        d = dict(src_dict)
        sub = d.pop("sub")

        name = d.pop("name")

        preferred_username = d.pop("preferred_username", UNSET)

        username = d.pop("username", UNSET)

        picture = d.pop("picture", UNSET)

        email = d.pop("email", UNSET)

        email_verified = d.pop("email_verified", UNSET)

        roles = cast(list[str], d.pop("roles", UNSET))

        _org_ctx = d.pop("org_ctx", UNSET)
        org_ctx: UserInfoResponseOrgCtx | Unset
        if isinstance(_org_ctx, Unset):
            org_ctx = UNSET
        else:
            org_ctx = UserInfoResponseOrgCtx.from_dict(_org_ctx)

        user_info_response = cls(
            sub=sub,
            name=name,
            preferred_username=preferred_username,
            username=username,
            picture=picture,
            email=email,
            email_verified=email_verified,
            roles=roles,
            org_ctx=org_ctx,
        )

        user_info_response.additional_properties = d
        return user_info_response

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
