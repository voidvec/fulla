from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.post_api_me_social_links_provider_authorize_body import PostApiMeSocialLinksProviderAuthorizeBody
from ...models.post_api_me_social_links_provider_authorize_provider import PostApiMeSocialLinksProviderAuthorizeProvider
from ...models.post_api_me_social_links_provider_authorize_response_200 import (
    PostApiMeSocialLinksProviderAuthorizeResponse200,
)
from ...types import UNSET, Response, Unset


def _get_kwargs(
    provider: PostApiMeSocialLinksProviderAuthorizeProvider,
    *,
    body: PostApiMeSocialLinksProviderAuthorizeBody | Unset = UNSET,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/me/social/links/{provider}/authorize".format(
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
) -> ErrorEnvelope | PostApiMeSocialLinksProviderAuthorizeResponse200 | None:
    if response.status_code == 200:
        response_200 = PostApiMeSocialLinksProviderAuthorizeResponse200.from_dict(response.json())

        return response_200

    if response.status_code == 400:
        response_400 = ErrorEnvelope.from_dict(response.json())

        return response_400

    if response.status_code == 401:
        response_401 = ErrorEnvelope.from_dict(response.json())

        return response_401

    if response.status_code == 500:
        response_500 = ErrorEnvelope.from_dict(response.json())

        return response_500

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[ErrorEnvelope | PostApiMeSocialLinksProviderAuthorizeResponse200]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    provider: PostApiMeSocialLinksProviderAuthorizeProvider,
    *,
    client: AuthenticatedClient,
    body: PostApiMeSocialLinksProviderAuthorizeBody | Unset = UNSET,
) -> Response[ErrorEnvelope | PostApiMeSocialLinksProviderAuthorizeResponse200]:
    """Begin Social Link (mint one-time state)

     Begin a social link flow (#71): mint a one-time state bound to (current user, provider) and return
    the provider authorize URL (with the state embedded) the SPA must redirect to. The link-back POST
    must present the same state; tokens are single-use with a short TTL. Fails closed (500) when linking
    or the state store (Redis) is not configured. Requires the `profile` scope.

    Args:
        provider (PostApiMeSocialLinksProviderAuthorizeProvider):
        body (PostApiMeSocialLinksProviderAuthorizeBody | Unset): Optional empty body (action-
            style operation).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeSocialLinksProviderAuthorizeResponse200]
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
    provider: PostApiMeSocialLinksProviderAuthorizeProvider,
    *,
    client: AuthenticatedClient,
    body: PostApiMeSocialLinksProviderAuthorizeBody | Unset = UNSET,
) -> ErrorEnvelope | PostApiMeSocialLinksProviderAuthorizeResponse200 | None:
    """Begin Social Link (mint one-time state)

     Begin a social link flow (#71): mint a one-time state bound to (current user, provider) and return
    the provider authorize URL (with the state embedded) the SPA must redirect to. The link-back POST
    must present the same state; tokens are single-use with a short TTL. Fails closed (500) when linking
    or the state store (Redis) is not configured. Requires the `profile` scope.

    Args:
        provider (PostApiMeSocialLinksProviderAuthorizeProvider):
        body (PostApiMeSocialLinksProviderAuthorizeBody | Unset): Optional empty body (action-
            style operation).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeSocialLinksProviderAuthorizeResponse200
    """

    return sync_detailed(
        provider=provider,
        client=client,
        body=body,
    ).parsed


async def asyncio_detailed(
    provider: PostApiMeSocialLinksProviderAuthorizeProvider,
    *,
    client: AuthenticatedClient,
    body: PostApiMeSocialLinksProviderAuthorizeBody | Unset = UNSET,
) -> Response[ErrorEnvelope | PostApiMeSocialLinksProviderAuthorizeResponse200]:
    """Begin Social Link (mint one-time state)

     Begin a social link flow (#71): mint a one-time state bound to (current user, provider) and return
    the provider authorize URL (with the state embedded) the SPA must redirect to. The link-back POST
    must present the same state; tokens are single-use with a short TTL. Fails closed (500) when linking
    or the state store (Redis) is not configured. Requires the `profile` scope.

    Args:
        provider (PostApiMeSocialLinksProviderAuthorizeProvider):
        body (PostApiMeSocialLinksProviderAuthorizeBody | Unset): Optional empty body (action-
            style operation).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeSocialLinksProviderAuthorizeResponse200]
    """

    kwargs = _get_kwargs(
        provider=provider,
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    provider: PostApiMeSocialLinksProviderAuthorizeProvider,
    *,
    client: AuthenticatedClient,
    body: PostApiMeSocialLinksProviderAuthorizeBody | Unset = UNSET,
) -> ErrorEnvelope | PostApiMeSocialLinksProviderAuthorizeResponse200 | None:
    """Begin Social Link (mint one-time state)

     Begin a social link flow (#71): mint a one-time state bound to (current user, provider) and return
    the provider authorize URL (with the state embedded) the SPA must redirect to. The link-back POST
    must present the same state; tokens are single-use with a short TTL. Fails closed (500) when linking
    or the state store (Redis) is not configured. Requires the `profile` scope.

    Args:
        provider (PostApiMeSocialLinksProviderAuthorizeProvider):
        body (PostApiMeSocialLinksProviderAuthorizeBody | Unset): Optional empty body (action-
            style operation).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeSocialLinksProviderAuthorizeResponse200
    """

    return (
        await asyncio_detailed(
            provider=provider,
            client=client,
            body=body,
        )
    ).parsed
