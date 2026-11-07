from http import HTTPStatus
from typing import Any, cast
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.post_api_auth_providers_provider_authorize_body import PostApiAuthProvidersProviderAuthorizeBody
from ...models.post_api_auth_providers_provider_authorize_provider import PostApiAuthProvidersProviderAuthorizeProvider
from ...models.social_login_authorization import SocialLoginAuthorization
from ...types import UNSET, Response, Unset


def _get_kwargs(
    provider: PostApiAuthProvidersProviderAuthorizeProvider,
    *,
    body: PostApiAuthProvidersProviderAuthorizeBody | Unset = UNSET,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/auth/providers/{provider}/authorize".format(
            provider=quote(str(provider), safe=""),
        ),
    }

    if not isinstance(body, Unset):
        _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Any | SocialLoginAuthorization | None:
    if response.status_code == 200:
        response_200 = SocialLoginAuthorization.from_dict(response.json())

        return response_200

    if response.status_code == 204:
        response_204 = cast(Any, None)
        return response_204

    if response.status_code == 400:
        response_400 = cast(Any, None)
        return response_400

    if response.status_code == 403:
        response_403 = cast(Any, None)
        return response_403

    if response.status_code == 404:
        response_404 = cast(Any, None)
        return response_404

    if response.status_code == 500:
        response_500 = cast(Any, None)
        return response_500

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[Any | SocialLoginAuthorization]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    provider: PostApiAuthProvidersProviderAuthorizeProvider,
    *,
    client: AuthenticatedClient | Client,
    body: PostApiAuthProvidersProviderAuthorizeBody | Unset = UNSET,
) -> Response[Any | SocialLoginAuthorization]:
    """Start Browser-Bound Social Login

     Requires an exact allowed Origin and credentials-enabled cookies. The first call initializes an
    HttpOnly browser binding cookie and returns 204 without issuing state. Retry once with that cookie
    to receive a transaction bound to the provider, client, redirect URI and browser. State expires
    after 600 seconds and can be consumed once. All responses use Cache-Control no-store. No bearer
    token is required.

    Args:
        provider (PostApiAuthProvidersProviderAuthorizeProvider):
        body (PostApiAuthProvidersProviderAuthorizeBody | Unset): Optional empty initialization
            body.

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | SocialLoginAuthorization]
    """

    kwargs = _get_kwargs(
        provider=provider,
        body=body,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    provider: PostApiAuthProvidersProviderAuthorizeProvider,
    *,
    client: AuthenticatedClient | Client,
    body: PostApiAuthProvidersProviderAuthorizeBody | Unset = UNSET,
) -> Any | SocialLoginAuthorization | None:
    """Start Browser-Bound Social Login

     Requires an exact allowed Origin and credentials-enabled cookies. The first call initializes an
    HttpOnly browser binding cookie and returns 204 without issuing state. Retry once with that cookie
    to receive a transaction bound to the provider, client, redirect URI and browser. State expires
    after 600 seconds and can be consumed once. All responses use Cache-Control no-store. No bearer
    token is required.

    Args:
        provider (PostApiAuthProvidersProviderAuthorizeProvider):
        body (PostApiAuthProvidersProviderAuthorizeBody | Unset): Optional empty initialization
            body.

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | SocialLoginAuthorization
    """

    return sync_detailed(
        provider=provider,
        client=client,
        body=body,
    ).parsed


async def asyncio_detailed(
    provider: PostApiAuthProvidersProviderAuthorizeProvider,
    *,
    client: AuthenticatedClient | Client,
    body: PostApiAuthProvidersProviderAuthorizeBody | Unset = UNSET,
) -> Response[Any | SocialLoginAuthorization]:
    """Start Browser-Bound Social Login

     Requires an exact allowed Origin and credentials-enabled cookies. The first call initializes an
    HttpOnly browser binding cookie and returns 204 without issuing state. Retry once with that cookie
    to receive a transaction bound to the provider, client, redirect URI and browser. State expires
    after 600 seconds and can be consumed once. All responses use Cache-Control no-store. No bearer
    token is required.

    Args:
        provider (PostApiAuthProvidersProviderAuthorizeProvider):
        body (PostApiAuthProvidersProviderAuthorizeBody | Unset): Optional empty initialization
            body.

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | SocialLoginAuthorization]
    """

    kwargs = _get_kwargs(
        provider=provider,
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    provider: PostApiAuthProvidersProviderAuthorizeProvider,
    *,
    client: AuthenticatedClient | Client,
    body: PostApiAuthProvidersProviderAuthorizeBody | Unset = UNSET,
) -> Any | SocialLoginAuthorization | None:
    """Start Browser-Bound Social Login

     Requires an exact allowed Origin and credentials-enabled cookies. The first call initializes an
    HttpOnly browser binding cookie and returns 204 without issuing state. Retry once with that cookie
    to receive a transaction bound to the provider, client, redirect URI and browser. State expires
    after 600 seconds and can be consumed once. All responses use Cache-Control no-store. No bearer
    token is required.

    Args:
        provider (PostApiAuthProvidersProviderAuthorizeProvider):
        body (PostApiAuthProvidersProviderAuthorizeBody | Unset): Optional empty initialization
            body.

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | SocialLoginAuthorization
    """

    return (
        await asyncio_detailed(
            provider=provider,
            client=client,
            body=body,
        )
    ).parsed
