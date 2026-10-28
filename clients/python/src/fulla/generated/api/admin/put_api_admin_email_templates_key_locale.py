from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.put_api_admin_email_templates_key_locale_body import PutApiAdminEmailTemplatesKeyLocaleBody
from ...types import Response


def _get_kwargs(
    key: str,
    locale: str,
    *,
    body: PutApiAdminEmailTemplatesKeyLocaleBody,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "put",
        "url": "/api/admin/email-templates/{key}/{locale}".format(
            key=quote(str(key), safe=""),
            locale=quote(str(locale), safe=""),
        ),
    }

    _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> Any | None:
    if response.status_code == 200:
        return None

    if response.status_code == 400:
        return None

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> Response[Any]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    key: str,
    locale: str,
    *,
    client: AuthenticatedClient,
    body: PutApiAdminEmailTemplatesKeyLocaleBody,
) -> Response[Any]:
    """Upsert Email Template

     Create or update a template. Save-time content policy rejects script/iframe/form/object/embed/base
    tags, javascript:/data:/vbscript: URLs, and outbound http(s) links outside the configured
    email.allowed_link_domains (EMAIL_TEMPLATE_CONTENT_REJECTED). Variables use {{name}} placeholders;
    substituted values are HTML-escaped by the engine.

    Args:
        key (str):
        locale (str):
        body (PutApiAdminEmailTemplatesKeyLocaleBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any]
    """

    kwargs = _get_kwargs(
        key=key,
        locale=locale,
        body=body,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


async def asyncio_detailed(
    key: str,
    locale: str,
    *,
    client: AuthenticatedClient,
    body: PutApiAdminEmailTemplatesKeyLocaleBody,
) -> Response[Any]:
    """Upsert Email Template

     Create or update a template. Save-time content policy rejects script/iframe/form/object/embed/base
    tags, javascript:/data:/vbscript: URLs, and outbound http(s) links outside the configured
    email.allowed_link_domains (EMAIL_TEMPLATE_CONTENT_REJECTED). Variables use {{name}} placeholders;
    substituted values are HTML-escaped by the engine.

    Args:
        key (str):
        locale (str):
        body (PutApiAdminEmailTemplatesKeyLocaleBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any]
    """

    kwargs = _get_kwargs(
        key=key,
        locale=locale,
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)
