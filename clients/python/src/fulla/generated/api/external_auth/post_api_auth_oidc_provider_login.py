from http import HTTPStatus
from typing import Any, cast
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.post_api_auth_oidc_provider_login_data_body import PostApiAuthOidcProviderLoginDataBody
from ...models.post_api_auth_oidc_provider_login_json_body import PostApiAuthOidcProviderLoginJsonBody
from ...models.post_api_auth_oidc_provider_login_response_200 import PostApiAuthOidcProviderLoginResponse200
from ...types import UNSET, Response


def _get_kwargs(
    provider: str,
    *,
    body: PostApiAuthOidcProviderLoginJsonBody | PostApiAuthOidcProviderLoginDataBody | Unset = UNSET,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/auth/oidc/{provider}/login".format(
            provider=quote(str(provider), safe=""),
        ),
    }

    if isinstance(body, PostApiAuthOidcProviderLoginJsonBody):
        _kwargs["json"] = body.to_dict()

        headers["Content-Type"] = "application/json"
    if isinstance(body, PostApiAuthOidcProviderLoginDataBody):
        _kwargs["data"] = body.to_dict()
        headers["Content-Type"] = "application/x-www-form-urlencoded"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Any | PostApiAuthOidcProviderLoginResponse200 | None:
    if response.status_code == 200:
        response_200 = PostApiAuthOidcProviderLoginResponse200.from_dict(response.json())

        return response_200

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

    if response.status_code == 502:
        response_502 = cast(Any, None)
        return response_502

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[Any | PostApiAuthOidcProviderLoginResponse200]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    provider: str,
    *,
    client: AuthenticatedClient | Client,
    body: PostApiAuthOidcProviderLoginJsonBody | PostApiAuthOidcProviderLoginDataBody | Unset = UNSET,
) -> Response[Any | PostApiAuthOidcProviderLoginResponse200]:
    """OIDC Upstream Login (registry provider)

     P2-1: exchange an authorization code from a registry-configured OIDC upstream (upstream_providers
    row) for a first-party token pair: code -> token endpoint -> userinfo -> claims mapping -> the same
    find-or-create account flow as google/wechat/github (oauth2_subject_mappings.provider =
    provider_key). Requires the one-time state from the authorize endpoint, an allowed Origin and the
    browser binding cookie.

    Args:
        provider (str): Enabled registry provider key (upstream_providers.provider_key).
        body (PostApiAuthOidcProviderLoginJsonBody):
        body (PostApiAuthOidcProviderLoginDataBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | PostApiAuthOidcProviderLoginResponse200]
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
    provider: str,
    *,
    client: AuthenticatedClient | Client,
    body: PostApiAuthOidcProviderLoginJsonBody | PostApiAuthOidcProviderLoginDataBody | Unset = UNSET,
) -> Any | PostApiAuthOidcProviderLoginResponse200 | None:
    """OIDC Upstream Login (registry provider)

     P2-1: exchange an authorization code from a registry-configured OIDC upstream (upstream_providers
    row) for a first-party token pair: code -> token endpoint -> userinfo -> claims mapping -> the same
    find-or-create account flow as google/wechat/github (oauth2_subject_mappings.provider =
    provider_key). Requires the one-time state from the authorize endpoint, an allowed Origin and the
    browser binding cookie.

    Args:
        provider (str): Enabled registry provider key (upstream_providers.provider_key).
        body (PostApiAuthOidcProviderLoginJsonBody):
        body (PostApiAuthOidcProviderLoginDataBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | PostApiAuthOidcProviderLoginResponse200
    """

    return sync_detailed(
        provider=provider,
        client=client,
        body=body,
    ).parsed


async def asyncio_detailed(
    provider: str,
    *,
    client: AuthenticatedClient | Client,
    body: PostApiAuthOidcProviderLoginJsonBody | PostApiAuthOidcProviderLoginDataBody | Unset = UNSET,
) -> Response[Any | PostApiAuthOidcProviderLoginResponse200]:
    """OIDC Upstream Login (registry provider)

     P2-1: exchange an authorization code from a registry-configured OIDC upstream (upstream_providers
    row) for a first-party token pair: code -> token endpoint -> userinfo -> claims mapping -> the same
    find-or-create account flow as google/wechat/github (oauth2_subject_mappings.provider =
    provider_key). Requires the one-time state from the authorize endpoint, an allowed Origin and the
    browser binding cookie.

    Args:
        provider (str): Enabled registry provider key (upstream_providers.provider_key).
        body (PostApiAuthOidcProviderLoginJsonBody):
        body (PostApiAuthOidcProviderLoginDataBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | PostApiAuthOidcProviderLoginResponse200]
    """

    kwargs = _get_kwargs(
        provider=provider,
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    provider: str,
    *,
    client: AuthenticatedClient | Client,
    body: PostApiAuthOidcProviderLoginJsonBody | PostApiAuthOidcProviderLoginDataBody | Unset = UNSET,
) -> Any | PostApiAuthOidcProviderLoginResponse200 | None:
    """OIDC Upstream Login (registry provider)

     P2-1: exchange an authorization code from a registry-configured OIDC upstream (upstream_providers
    row) for a first-party token pair: code -> token endpoint -> userinfo -> claims mapping -> the same
    find-or-create account flow as google/wechat/github (oauth2_subject_mappings.provider =
    provider_key). Requires the one-time state from the authorize endpoint, an allowed Origin and the
    browser binding cookie.

    Args:
        provider (str): Enabled registry provider key (upstream_providers.provider_key).
        body (PostApiAuthOidcProviderLoginJsonBody):
        body (PostApiAuthOidcProviderLoginDataBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | PostApiAuthOidcProviderLoginResponse200
    """

    return (
        await asyncio_detailed(
            provider=provider,
            client=client,
            body=body,
        )
    ).parsed
