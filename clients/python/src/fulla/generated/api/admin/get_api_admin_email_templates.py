from http import HTTPStatus
from typing import Any, cast

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.get_api_admin_email_templates_response_200 import GetApiAdminEmailTemplatesResponse200
from ...types import Response


def _get_kwargs() -> dict[str, Any]:

    _kwargs: dict[str, Any] = {
        "method": "get",
        "url": "/api/admin/email-templates",
    }

    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Any | GetApiAdminEmailTemplatesResponse200 | None:
    if response.status_code == 200:
        response_200 = GetApiAdminEmailTemplatesResponse200.from_dict(response.json())

        return response_200

    if response.status_code == 404:
        response_404 = cast(Any, None)
        return response_404

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[Any | GetApiAdminEmailTemplatesResponse200]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    *,
    client: AuthenticatedClient,
) -> Response[Any | GetApiAdminEmailTemplatesResponse200]:
    """List Email Templates

     P1-2 (#278): list all email templates (template_key x locale). Memory-storage deployments answer 404
    (no template store).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | GetApiAdminEmailTemplatesResponse200]
    """

    kwargs = _get_kwargs()

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    *,
    client: AuthenticatedClient,
) -> Any | GetApiAdminEmailTemplatesResponse200 | None:
    """List Email Templates

     P1-2 (#278): list all email templates (template_key x locale). Memory-storage deployments answer 404
    (no template store).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | GetApiAdminEmailTemplatesResponse200
    """

    return sync_detailed(
        client=client,
    ).parsed


async def asyncio_detailed(
    *,
    client: AuthenticatedClient,
) -> Response[Any | GetApiAdminEmailTemplatesResponse200]:
    """List Email Templates

     P1-2 (#278): list all email templates (template_key x locale). Memory-storage deployments answer 404
    (no template store).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | GetApiAdminEmailTemplatesResponse200]
    """

    kwargs = _get_kwargs()

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    *,
    client: AuthenticatedClient,
) -> Any | GetApiAdminEmailTemplatesResponse200 | None:
    """List Email Templates

     P1-2 (#278): list all email templates (template_key x locale). Memory-storage deployments answer 404
    (no template store).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | GetApiAdminEmailTemplatesResponse200
    """

    return (
        await asyncio_detailed(
            client=client,
        )
    ).parsed
