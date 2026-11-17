from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.upstream_provider_create import UpstreamProviderCreate
from ...types import Response


def _get_kwargs(
    provider_key: str,
    *,
    body: UpstreamProviderCreate,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "put",
        "url": "/api/admin/upstream-providers/{provider_key}".format(
            provider_key=quote(str(provider_key), safe=""),
        ),
    }

    _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> Any | None:
    if response.status_code == 200:
        return None

    if response.status_code == 404:
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
    provider_key: str,
    *,
    client: AuthenticatedClient,
    body: UpstreamProviderCreate,
) -> Response[Any]:
    """Update Upstream Provider

     P2-1: full-row update keyed by provider_key (immutable -- it keys oauth2_subject_mappings). Empty
    client_secret keeps the current one. Mutations refresh the login-page provider set immediately.

    Args:
        provider_key (str):
        body (UpstreamProviderCreate): Create/update body for the upstream registry (client_secret
            write-only; empty on update keeps the current value).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any]
    """

    kwargs = _get_kwargs(
        provider_key=provider_key,
        body=body,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


async def asyncio_detailed(
    provider_key: str,
    *,
    client: AuthenticatedClient,
    body: UpstreamProviderCreate,
) -> Response[Any]:
    """Update Upstream Provider

     P2-1: full-row update keyed by provider_key (immutable -- it keys oauth2_subject_mappings). Empty
    client_secret keeps the current one. Mutations refresh the login-page provider set immediately.

    Args:
        provider_key (str):
        body (UpstreamProviderCreate): Create/update body for the upstream registry (client_secret
            write-only; empty on update keeps the current value).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any]
    """

    kwargs = _get_kwargs(
        provider_key=provider_key,
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)
