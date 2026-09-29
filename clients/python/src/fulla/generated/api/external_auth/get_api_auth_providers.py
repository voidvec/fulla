from http import HTTPStatus
from typing import Any

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.auth_providers_response import AuthProvidersResponse
from ...types import Response


def _get_kwargs() -> dict[str, Any]:

    _kwargs: dict[str, Any] = {
        "method": "get",
        "url": "/api/auth/providers",
    }

    return _kwargs


def _parse_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> AuthProvidersResponse | None:
    if response.status_code == 200:
        response_200 = AuthProvidersResponse.from_dict(response.json())

        return response_200

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[AuthProvidersResponse]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    *,
    client: AuthenticatedClient | Client,
) -> Response[AuthProvidersResponse]:
    """List Enabled External Login Providers

     Public, unauthenticated discovery of the external login providers this deployment currently offers
    (v1.5.0 provider tiers). The login page renders its provider buttons from this response; an empty
    list means external login is disabled. authorize_url is the fully built provider authorize URL — the
    SPA redirects to it as-is; the redirect target resolves from the per-provider redirect_uri override,
    else frontend.url + /callback/{provider}.

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[AuthProvidersResponse]
    """

    kwargs = _get_kwargs()

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    *,
    client: AuthenticatedClient | Client,
) -> AuthProvidersResponse | None:
    """List Enabled External Login Providers

     Public, unauthenticated discovery of the external login providers this deployment currently offers
    (v1.5.0 provider tiers). The login page renders its provider buttons from this response; an empty
    list means external login is disabled. authorize_url is the fully built provider authorize URL — the
    SPA redirects to it as-is; the redirect target resolves from the per-provider redirect_uri override,
    else frontend.url + /callback/{provider}.

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        AuthProvidersResponse
    """

    return sync_detailed(
        client=client,
    ).parsed


async def asyncio_detailed(
    *,
    client: AuthenticatedClient | Client,
) -> Response[AuthProvidersResponse]:
    """List Enabled External Login Providers

     Public, unauthenticated discovery of the external login providers this deployment currently offers
    (v1.5.0 provider tiers). The login page renders its provider buttons from this response; an empty
    list means external login is disabled. authorize_url is the fully built provider authorize URL — the
    SPA redirects to it as-is; the redirect target resolves from the per-provider redirect_uri override,
    else frontend.url + /callback/{provider}.

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[AuthProvidersResponse]
    """

    kwargs = _get_kwargs()

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    *,
    client: AuthenticatedClient | Client,
) -> AuthProvidersResponse | None:
    """List Enabled External Login Providers

     Public, unauthenticated discovery of the external login providers this deployment currently offers
    (v1.5.0 provider tiers). The login page renders its provider buttons from this response; an empty
    list means external login is disabled. authorize_url is the fully built provider authorize URL — the
    SPA redirects to it as-is; the redirect target resolves from the per-provider redirect_uri override,
    else frontend.url + /callback/{provider}.

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        AuthProvidersResponse
    """

    return (
        await asyncio_detailed(
            client=client,
        )
    ).parsed
