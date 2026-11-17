from http import HTTPStatus
from typing import Any, cast

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.upstream_provider import UpstreamProvider
from ...models.upstream_provider_create import UpstreamProviderCreate
from ...types import Response


def _get_kwargs(
    *,
    body: UpstreamProviderCreate,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/admin/upstream-providers",
    }

    _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> Any | UpstreamProvider | None:
    if response.status_code == 201:
        response_201 = UpstreamProvider.from_dict(response.json())

        return response_201

    if response.status_code == 400:
        response_400 = cast(Any, None)
        return response_400

    if response.status_code == 409:
        response_409 = cast(Any, None)
        return response_409

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[Any | UpstreamProvider]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    *,
    client: AuthenticatedClient,
    body: UpstreamProviderCreate,
) -> Response[Any | UpstreamProvider]:
    """Create Upstream Provider

     P2-1: register an OIDC upstream. provider_key must match ^[a-z0-9][a-z0-9-]{1,62}[a-z0-9]$ and not
    collide with the reserved social keys (VALIDATION_RESOURCE_CONFLICT on the UNIQUE constraint).
    Mutations refresh the login-page provider set immediately.

    Args:
        body (UpstreamProviderCreate): Create/update body for the upstream registry (client_secret
            write-only; empty on update keeps the current value).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | UpstreamProvider]
    """

    kwargs = _get_kwargs(
        body=body,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    *,
    client: AuthenticatedClient,
    body: UpstreamProviderCreate,
) -> Any | UpstreamProvider | None:
    """Create Upstream Provider

     P2-1: register an OIDC upstream. provider_key must match ^[a-z0-9][a-z0-9-]{1,62}[a-z0-9]$ and not
    collide with the reserved social keys (VALIDATION_RESOURCE_CONFLICT on the UNIQUE constraint).
    Mutations refresh the login-page provider set immediately.

    Args:
        body (UpstreamProviderCreate): Create/update body for the upstream registry (client_secret
            write-only; empty on update keeps the current value).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | UpstreamProvider
    """

    return sync_detailed(
        client=client,
        body=body,
    ).parsed


async def asyncio_detailed(
    *,
    client: AuthenticatedClient,
    body: UpstreamProviderCreate,
) -> Response[Any | UpstreamProvider]:
    """Create Upstream Provider

     P2-1: register an OIDC upstream. provider_key must match ^[a-z0-9][a-z0-9-]{1,62}[a-z0-9]$ and not
    collide with the reserved social keys (VALIDATION_RESOURCE_CONFLICT on the UNIQUE constraint).
    Mutations refresh the login-page provider set immediately.

    Args:
        body (UpstreamProviderCreate): Create/update body for the upstream registry (client_secret
            write-only; empty on update keeps the current value).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | UpstreamProvider]
    """

    kwargs = _get_kwargs(
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    *,
    client: AuthenticatedClient,
    body: UpstreamProviderCreate,
) -> Any | UpstreamProvider | None:
    """Create Upstream Provider

     P2-1: register an OIDC upstream. provider_key must match ^[a-z0-9][a-z0-9-]{1,62}[a-z0-9]$ and not
    collide with the reserved social keys (VALIDATION_RESOURCE_CONFLICT on the UNIQUE constraint).
    Mutations refresh the login-page provider set immediately.

    Args:
        body (UpstreamProviderCreate): Create/update body for the upstream registry (client_secret
            write-only; empty on update keeps the current value).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | UpstreamProvider
    """

    return (
        await asyncio_detailed(
            client=client,
            body=body,
        )
    ).parsed
