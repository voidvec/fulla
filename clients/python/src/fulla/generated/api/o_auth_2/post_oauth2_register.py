from http import HTTPStatus
from typing import Any, cast

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.post_oauth_2_register_body import PostOauth2RegisterBody
from ...types import Response


def _get_kwargs(
    *,
    body: PostOauth2RegisterBody,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/oauth2/register",
    }

    _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> Any | ErrorEnvelope | None:
    if response.status_code == 201:
        response_201 = cast(Any, None)
        return response_201

    if response.status_code == 400:
        response_400 = cast(Any, None)
        return response_400

    if response.status_code == 401:
        response_401 = cast(Any, None)
        return response_401

    if response.status_code == 403:
        response_403 = ErrorEnvelope.from_dict(response.json())

        return response_403

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> Response[Any | ErrorEnvelope]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    *,
    client: AuthenticatedClient,
    body: PostOauth2RegisterBody,
) -> Response[Any | ErrorEnvelope]:
    """Register Client

     Dynamic client registration (admin-gated). Requires a valid Bearer token and the admin role under
    the shipped AuthorizationFilter rbac_rules. Missing/invalid credentials return 401; an authenticated
    caller without the required role returns 403 (AUTHZ_INSUFFICIENT_PERMISSIONS). This is separate from
    self-service application registration at /api/me/applications. Accepted metadata: client_name,
    client_type (CONFIDENTIAL|PUBLIC), token_endpoint_auth_method, redirect_uris (https enforced,
    loopback exempt), grant_types, and — P0-7b — optional post_logout_redirect_uris (dedicated RP-
    initiated-logout redirect registrations, same scheme policy; when registered they are authoritative
    for end_session's post_logout_redirect_uri validation).

    Args:
        body (PostOauth2RegisterBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | ErrorEnvelope]
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
    body: PostOauth2RegisterBody,
) -> Any | ErrorEnvelope | None:
    """Register Client

     Dynamic client registration (admin-gated). Requires a valid Bearer token and the admin role under
    the shipped AuthorizationFilter rbac_rules. Missing/invalid credentials return 401; an authenticated
    caller without the required role returns 403 (AUTHZ_INSUFFICIENT_PERMISSIONS). This is separate from
    self-service application registration at /api/me/applications. Accepted metadata: client_name,
    client_type (CONFIDENTIAL|PUBLIC), token_endpoint_auth_method, redirect_uris (https enforced,
    loopback exempt), grant_types, and — P0-7b — optional post_logout_redirect_uris (dedicated RP-
    initiated-logout redirect registrations, same scheme policy; when registered they are authoritative
    for end_session's post_logout_redirect_uri validation).

    Args:
        body (PostOauth2RegisterBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | ErrorEnvelope
    """

    return sync_detailed(
        client=client,
        body=body,
    ).parsed


async def asyncio_detailed(
    *,
    client: AuthenticatedClient,
    body: PostOauth2RegisterBody,
) -> Response[Any | ErrorEnvelope]:
    """Register Client

     Dynamic client registration (admin-gated). Requires a valid Bearer token and the admin role under
    the shipped AuthorizationFilter rbac_rules. Missing/invalid credentials return 401; an authenticated
    caller without the required role returns 403 (AUTHZ_INSUFFICIENT_PERMISSIONS). This is separate from
    self-service application registration at /api/me/applications. Accepted metadata: client_name,
    client_type (CONFIDENTIAL|PUBLIC), token_endpoint_auth_method, redirect_uris (https enforced,
    loopback exempt), grant_types, and — P0-7b — optional post_logout_redirect_uris (dedicated RP-
    initiated-logout redirect registrations, same scheme policy; when registered they are authoritative
    for end_session's post_logout_redirect_uri validation).

    Args:
        body (PostOauth2RegisterBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | ErrorEnvelope]
    """

    kwargs = _get_kwargs(
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    *,
    client: AuthenticatedClient,
    body: PostOauth2RegisterBody,
) -> Any | ErrorEnvelope | None:
    """Register Client

     Dynamic client registration (admin-gated). Requires a valid Bearer token and the admin role under
    the shipped AuthorizationFilter rbac_rules. Missing/invalid credentials return 401; an authenticated
    caller without the required role returns 403 (AUTHZ_INSUFFICIENT_PERMISSIONS). This is separate from
    self-service application registration at /api/me/applications. Accepted metadata: client_name,
    client_type (CONFIDENTIAL|PUBLIC), token_endpoint_auth_method, redirect_uris (https enforced,
    loopback exempt), grant_types, and — P0-7b — optional post_logout_redirect_uris (dedicated RP-
    initiated-logout redirect registrations, same scheme policy; when registered they are authoritative
    for end_session's post_logout_redirect_uri validation).

    Args:
        body (PostOauth2RegisterBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | ErrorEnvelope
    """

    return (
        await asyncio_detailed(
            client=client,
            body=body,
        )
    ).parsed
