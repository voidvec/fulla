from http import HTTPStatus
from typing import Any, cast

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.introspection_response import IntrospectionResponse
from ...models.o_auth_2_error import OAuth2Error
from ...models.post_oauth_2_introspect_body import PostOauth2IntrospectBody
from ...types import Response


def _get_kwargs(
    *,
    body: PostOauth2IntrospectBody,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/oauth2/introspect",
    }

    _kwargs["data"] = body.to_dict()
    headers["Content-Type"] = "application/x-www-form-urlencoded"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Any | IntrospectionResponse | OAuth2Error | None:
    if response.status_code == 200:
        response_200 = IntrospectionResponse.from_dict(response.json())

        return response_200

    if response.status_code == 400:
        response_400 = OAuth2Error.from_dict(response.json())

        return response_400

    if response.status_code == 401:
        response_401 = OAuth2Error.from_dict(response.json())

        return response_401

    if response.status_code == 429:
        response_429 = cast(Any, None)
        return response_429

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[Any | IntrospectionResponse | OAuth2Error]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    *,
    client: AuthenticatedClient,
    body: PostOauth2IntrospectBody,
) -> Response[Any | IntrospectionResponse | OAuth2Error]:
    """Introspect token

     RFC 7662 OAuth 2.0 Token Introspection. Returns information about a token. The calling client MUST
    authenticate (HTTP Basic preferred; client_id/client_secret form parameters accepted). Check active
    before using metadata; ignore metadata in inactive responses. When returned, org_id is the token's
    stored issuance binding, not a live membership check. Removing a member or an organization consent
    does not itself revoke issued tokens. Expiry, explicit revocation and cache convergence remain
    separate limits. Legacy Redis L2 cache hits can omit org_id; absence must not be used to infer that
    a token has no organization.

    Args:
        body (PostOauth2IntrospectBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | IntrospectionResponse | OAuth2Error]
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
    body: PostOauth2IntrospectBody,
) -> Any | IntrospectionResponse | OAuth2Error | None:
    """Introspect token

     RFC 7662 OAuth 2.0 Token Introspection. Returns information about a token. The calling client MUST
    authenticate (HTTP Basic preferred; client_id/client_secret form parameters accepted). Check active
    before using metadata; ignore metadata in inactive responses. When returned, org_id is the token's
    stored issuance binding, not a live membership check. Removing a member or an organization consent
    does not itself revoke issued tokens. Expiry, explicit revocation and cache convergence remain
    separate limits. Legacy Redis L2 cache hits can omit org_id; absence must not be used to infer that
    a token has no organization.

    Args:
        body (PostOauth2IntrospectBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | IntrospectionResponse | OAuth2Error
    """

    return sync_detailed(
        client=client,
        body=body,
    ).parsed


async def asyncio_detailed(
    *,
    client: AuthenticatedClient,
    body: PostOauth2IntrospectBody,
) -> Response[Any | IntrospectionResponse | OAuth2Error]:
    """Introspect token

     RFC 7662 OAuth 2.0 Token Introspection. Returns information about a token. The calling client MUST
    authenticate (HTTP Basic preferred; client_id/client_secret form parameters accepted). Check active
    before using metadata; ignore metadata in inactive responses. When returned, org_id is the token's
    stored issuance binding, not a live membership check. Removing a member or an organization consent
    does not itself revoke issued tokens. Expiry, explicit revocation and cache convergence remain
    separate limits. Legacy Redis L2 cache hits can omit org_id; absence must not be used to infer that
    a token has no organization.

    Args:
        body (PostOauth2IntrospectBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | IntrospectionResponse | OAuth2Error]
    """

    kwargs = _get_kwargs(
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    *,
    client: AuthenticatedClient,
    body: PostOauth2IntrospectBody,
) -> Any | IntrospectionResponse | OAuth2Error | None:
    """Introspect token

     RFC 7662 OAuth 2.0 Token Introspection. Returns information about a token. The calling client MUST
    authenticate (HTTP Basic preferred; client_id/client_secret form parameters accepted). Check active
    before using metadata; ignore metadata in inactive responses. When returned, org_id is the token's
    stored issuance binding, not a live membership check. Removing a member or an organization consent
    does not itself revoke issued tokens. Expiry, explicit revocation and cache convergence remain
    separate limits. Legacy Redis L2 cache hits can omit org_id; absence must not be used to infer that
    a token has no organization.

    Args:
        body (PostOauth2IntrospectBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | IntrospectionResponse | OAuth2Error
    """

    return (
        await asyncio_detailed(
            client=client,
            body=body,
        )
    ).parsed
