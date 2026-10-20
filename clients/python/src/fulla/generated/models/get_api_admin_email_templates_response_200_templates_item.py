from __future__ import annotations

from collections.abc import Mapping
from typing import Any, TypeVar

from attrs import define as _attrs_define
from attrs import field as _attrs_field

from ..types import UNSET, Unset

T = TypeVar("T", bound="GetApiAdminEmailTemplatesResponse200TemplatesItem")


@_attrs_define
class GetApiAdminEmailTemplatesResponse200TemplatesItem:
    """
    Attributes:
        template_key (str | Unset):
        locale (str | Unset):
        subject (str | Unset):
        body_html (str | Unset):
        body_text (str | Unset):
        updated_at (str | Unset):
    """

    template_key: str | Unset = UNSET
    locale: str | Unset = UNSET
    subject: str | Unset = UNSET
    body_html: str | Unset = UNSET
    body_text: str | Unset = UNSET
    updated_at: str | Unset = UNSET
    additional_properties: dict[str, Any] = _attrs_field(init=False, factory=dict)

    def to_dict(self) -> dict[str, Any]:
        template_key = self.template_key

        locale = self.locale

        subject = self.subject

        body_html = self.body_html

        body_text = self.body_text

        updated_at = self.updated_at

        field_dict: dict[str, Any] = {}
        field_dict.update(self.additional_properties)
        field_dict.update({})
        if template_key is not UNSET:
            field_dict["template_key"] = template_key
        if locale is not UNSET:
            field_dict["locale"] = locale
        if subject is not UNSET:
            field_dict["subject"] = subject
        if body_html is not UNSET:
            field_dict["body_html"] = body_html
        if body_text is not UNSET:
            field_dict["body_text"] = body_text
        if updated_at is not UNSET:
            field_dict["updated_at"] = updated_at

        return field_dict

    @classmethod
    def from_dict(cls: type[T], src_dict: Mapping[str, Any]) -> T:
        d = dict(src_dict)
        template_key = d.pop("template_key", UNSET)

        locale = d.pop("locale", UNSET)

        subject = d.pop("subject", UNSET)

        body_html = d.pop("body_html", UNSET)

        body_text = d.pop("body_text", UNSET)

        updated_at = d.pop("updated_at", UNSET)

        get_api_admin_email_templates_response_200_templates_item = cls(
            template_key=template_key,
            locale=locale,
            subject=subject,
            body_html=body_html,
            body_text=body_text,
            updated_at=updated_at,
        )

        get_api_admin_email_templates_response_200_templates_item.additional_properties = d
        return get_api_admin_email_templates_response_200_templates_item

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
