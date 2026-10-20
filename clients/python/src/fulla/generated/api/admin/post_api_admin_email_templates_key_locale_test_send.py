from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.post_api_admin_email_templates_key_locale_test_send_body import (
    PostApiAdminEmailTemplatesKeyLocaleTestSendBody,
)
from ...types import Response


def _get_kwargs(
    key: str,
    locale: str,
    *,
    body: PostApiAdminEmailTemplatesKeyLocaleTestSendBody,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/admin/email-templates/{key}/{locale}/test-send".format(
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

    if response.status_code == 403:
        return None

    if response.status_code == 404:
        return None

    if response.status_code == 429:
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
    body: PostApiAdminEmailTemplatesKeyLocaleTestSendBody,
) -> Response[Any]:
    """Test-send Email Template

     Render a template with synthetic sample variables (no real user PII) and send it. Recipient must be
    the admin's own address or listed in email.test_recipients (EMAIL_TEST_RECIPIENT_DENIED);
    rolling-24h per-admin quota applies (EMAIL_TEST_QUOTA_EXCEEDED, email.test_send_quota_per_day,
    default 10). Audited as email_test_sent.

    Args:
        key (str):
        locale (str):
        body (PostApiAdminEmailTemplatesKeyLocaleTestSendBody):

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
    body: PostApiAdminEmailTemplatesKeyLocaleTestSendBody,
) -> Response[Any]:
    """Test-send Email Template

     Render a template with synthetic sample variables (no real user PII) and send it. Recipient must be
    the admin's own address or listed in email.test_recipients (EMAIL_TEST_RECIPIENT_DENIED);
    rolling-24h per-admin quota applies (EMAIL_TEST_QUOTA_EXCEEDED, email.test_send_quota_per_day,
    default 10). Audited as email_test_sent.

    Args:
        key (str):
        locale (str):
        body (PostApiAdminEmailTemplatesKeyLocaleTestSendBody):

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
