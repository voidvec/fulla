from http import HTTPStatus
from typing import Any

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.get_api_admin_upstream_providers_response_200 import GetApiAdminUpstreamProvidersResponse200
from ...types import Response


def _get_kwargs() -> dict[str, Any]:

    _kwargs: dict[str, Any] = {
        "method": "get",
        "url": "/api/admin/upstream-providers",
    }

    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> GetApiAdminUpstreamProvidersResponse200 | None:
    if response.status_code == 200:
        response_200 = GetApiAdminUpstreamProvidersResponse200.from_dict(response.json())

        return response_200

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[GetApiAdminUpstreamProvidersResponse200]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    *,
    client: AuthenticatedClient,
) -> Response[GetApiAdminUpstreamProvidersResponse200]:
    """List Upstream Providers

     P2-1: list every OIDC-in upstream provider (enabled and disabled). client_secret is write-only
    (client_secret_set bool).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[GetApiAdminUpstreamProvidersResponse200]
    """

    kwargs = _get_kwargs()

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    *,
    client: AuthenticatedClient,
) -> GetApiAdminUpstreamProvidersResponse200 | None:
    """List Upstream Providers

     P2-1: list every OIDC-in upstream provider (enabled and disabled). client_secret is write-only
    (client_secret_set bool).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        GetApiAdminUpstreamProvidersResponse200
    """

    return sync_detailed(
        client=client,
    ).parsed


async def asyncio_detailed(
    *,
    client: AuthenticatedClient,
) -> Response[GetApiAdminUpstreamProvidersResponse200]:
    """List Upstream Providers

     P2-1: list every OIDC-in upstream provider (enabled and disabled). client_secret is write-only
    (client_secret_set bool).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[GetApiAdminUpstreamProvidersResponse200]
    """

    kwargs = _get_kwargs()

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    *,
    client: AuthenticatedClient,
) -> GetApiAdminUpstreamProvidersResponse200 | None:
    """List Upstream Providers

     P2-1: list every OIDC-in upstream provider (enabled and disabled). client_secret is write-only
    (client_secret_set bool).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        GetApiAdminUpstreamProvidersResponse200
    """

    return (
        await asyncio_detailed(
            client=client,
        )
    ).parsed
