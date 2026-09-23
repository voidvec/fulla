from __future__ import annotations

from collections.abc import Mapping
from typing import TYPE_CHECKING, Any, TypeVar, cast

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..models.get_api_me_organizations_response_200_organizations_item_role import (
    GetApiMeOrganizationsResponse200OrganizationsItemRole,
)
from ..types import UNSET, Unset

if TYPE_CHECKING:
    from ..models.get_api_me_organizations_response_200_organizations_item_successor_nomination_type_0 import (
        GetApiMeOrganizationsResponse200OrganizationsItemSuccessorNominationType0,
    )


T = TypeVar("T", bound="GetApiMeOrganizationsResponse200OrganizationsItem")


@_attrs_define
class GetApiMeOrganizationsResponse200OrganizationsItem:
    """
    Attributes:
        id (int | Unset):
        slug (str | Unset):
        name (str | Unset):
        logo_uri (str | Unset):
        primary_color (str | Unset):
        role (GetApiMeOrganizationsResponse200OrganizationsItemRole | Unset):
        successor_nomination (GetApiMeOrganizationsResponse200OrganizationsItemSuccessorNominationType0 | None | Unset):
            The org's pending succession nomination; visible to owner/admin callers only (null otherwise).
    """

    id: int | Unset = UNSET
    slug: str | Unset = UNSET
    name: str | Unset = UNSET
    logo_uri: str | Unset = UNSET
    primary_color: str | Unset = UNSET
    role: GetApiMeOrganizationsResponse200OrganizationsItemRole | Unset = UNSET
    successor_nomination: GetApiMeOrganizationsResponse200OrganizationsItemSuccessorNominationType0 | None | Unset = (
        UNSET
    )
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        from ..models.get_api_me_organizations_response_200_organizations_item_successor_nomination_type_0 import (
            GetApiMeOrganizationsResponse200OrganizationsItemSuccessorNominationType0,
        )

        id = self.id

        slug = self.slug

        name = self.name

        logo_uri = self.logo_uri

        primary_color = self.primary_color

        role: str | Unset = UNSET
        if not isinstance(self.role, Unset):
            role = self.role.value

        successor_nomination: dict[str, Any] | None | Unset
        if isinstance(self.successor_nomination, Unset):
            successor_nomination = UNSET
        elif isinstance(
            self.successor_nomination, GetApiMeOrganizationsResponse200OrganizationsItemSuccessorNominationType0
        ):
            successor_nomination = self.successor_nomination.to_dict()
        else:
            successor_nomination = self.successor_nomination

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if id is not UNSET:
            field_dict["id"] = id
        if slug is not UNSET:
            field_dict["slug"] = slug
        if name is not UNSET:
            field_dict["name"] = name
        if logo_uri is not UNSET:
            field_dict["logo_uri"] = logo_uri
        if primary_color is not UNSET:
            field_dict["primary_color"] = primary_color
        if role is not UNSET:
            field_dict["role"] = role
        if successor_nomination is not UNSET:
            field_dict["successor_nomination"] = successor_nomination

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        from ..models.get_api_me_organizations_response_200_organizations_item_successor_nomination_type_0 import (
            GetApiMeOrganizationsResponse200OrganizationsItemSuccessorNominationType0,
        )

        d = dict(src_dict)
        id = d.pop("id", UNSET)

        slug = d.pop("slug", UNSET)

        name = d.pop("name", UNSET)

        logo_uri = d.pop("logo_uri", UNSET)

        primary_color = d.pop("primary_color", UNSET)

        _role = d.pop("role", UNSET)
        role: GetApiMeOrganizationsResponse200OrganizationsItemRole | Unset
        if isinstance(_role, Unset):
            role = UNSET
        else:
            role = GetApiMeOrganizationsResponse200OrganizationsItemRole(_role)

        def _parse_successor_nomination(
            data: object,
        ) -> GetApiMeOrganizationsResponse200OrganizationsItemSuccessorNominationType0 | None | Unset:
            if data is None:
                return data
            if isinstance(data, Unset):
                return data
            try:
                if not isinstance(data, dict):
                    raise TypeError()
                successor_nomination_type_0 = (
                    GetApiMeOrganizationsResponse200OrganizationsItemSuccessorNominationType0.from_dict(data)
                )

                return successor_nomination_type_0
            except (TypeError, ValueError, AttributeError, KeyError):
                pass
            return cast(GetApiMeOrganizationsResponse200OrganizationsItemSuccessorNominationType0 | None | Unset, data)

        successor_nomination = _parse_successor_nomination(d.pop("successor_nomination", UNSET))

        get_api_me_organizations_response_200_organizations_item = cls(
            id=id,
            slug=slug,
            name=name,
            logo_uri=logo_uri,
            primary_color=primary_color,
            role=role,
            successor_nomination=successor_nomination,
        )

        get_api_me_organizations_response_200_organizations_item.additional_properties = d
        return get_api_me_organizations_response_200_organizations_item

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
