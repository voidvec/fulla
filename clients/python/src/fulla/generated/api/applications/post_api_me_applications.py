from http import HTTPStatus
from typing import Any

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.post_api_me_applications_body import PostApiMeApplicationsBody
from ...models.post_api_me_applications_response_201 import PostApiMeApplicationsResponse201
from ...types import Response


def _get_kwargs(
    *,
    body: PostApiMeApplicationsBody,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/me/applications",
    }

    _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> ErrorEnvelope | PostApiMeApplicationsResponse201 | None:
    if response.status_code == 201:
        response_201 = PostApiMeApplicationsResponse201.from_dict(response.json())

        return response_201

    if response.status_code == 400:
        response_400 = ErrorEnvelope.from_dict(response.json())

        return response_400

    if response.status_code == 401:
        response_401 = ErrorEnvelope.from_dict(response.json())

        return response_401

    if response.status_code == 403:
        response_403 = ErrorEnvelope.from_dict(response.json())

        return response_403

    if response.status_code == 409:
        response_409 = ErrorEnvelope.from_dict(response.json())

        return response_409

    if response.status_code == 429:
        response_429 = ErrorEnvelope.from_dict(response.json())

        return response_429

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[ErrorEnvelope | PostApiMeApplicationsResponse201]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    *,
    client: AuthenticatedClient,
    body: PostApiMeApplicationsBody,
) -> Response[ErrorEnvelope | PostApiMeApplicationsResponse201]:
    """Register Application (self-service)

     Register an OAuth2/OIDC application (RP self-registration; v1.4.0 open platform). Gated by the
    open_platform config (enabled, require_org, per-user/per-org quotas, 24h creation rate limit, the
    self_service scope allowlist and the grant-type whitelist). The client secret (CONFIDENTIAL) is
    returned exactly once.

    Args:
        body (PostApiMeApplicationsBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeApplicationsResponse201]
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
    body: PostApiMeApplicationsBody,
) -> ErrorEnvelope | PostApiMeApplicationsResponse201 | None:
    """Register Application (self-service)

     Register an OAuth2/OIDC application (RP self-registration; v1.4.0 open platform). Gated by the
    open_platform config (enabled, require_org, per-user/per-org quotas, 24h creation rate limit, the
    self_service scope allowlist and the grant-type whitelist). The client secret (CONFIDENTIAL) is
    returned exactly once.

    Args:
        body (PostApiMeApplicationsBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeApplicationsResponse201
    """

    return sync_detailed(
        client=client,
        body=body,
    ).parsed


async def asyncio_detailed(
    *,
    client: AuthenticatedClient,
    body: PostApiMeApplicationsBody,
) -> Response[ErrorEnvelope | PostApiMeApplicationsResponse201]:
    """Register Application (self-service)

     Register an OAuth2/OIDC application (RP self-registration; v1.4.0 open platform). Gated by the
    open_platform config (enabled, require_org, per-user/per-org quotas, 24h creation rate limit, the
    self_service scope allowlist and the grant-type whitelist). The client secret (CONFIDENTIAL) is
    returned exactly once.

    Args:
        body (PostApiMeApplicationsBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeApplicationsResponse201]
    """

    kwargs = _get_kwargs(
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    *,
    client: AuthenticatedClient,
    body: PostApiMeApplicationsBody,
) -> ErrorEnvelope | PostApiMeApplicationsResponse201 | None:
    """Register Application (self-service)

     Register an OAuth2/OIDC application (RP self-registration; v1.4.0 open platform). Gated by the
    open_platform config (enabled, require_org, per-user/per-org quotas, 24h creation rate limit, the
    self_service scope allowlist and the grant-type whitelist). The client secret (CONFIDENTIAL) is
    returned exactly once.

    Args:
        body (PostApiMeApplicationsBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeApplicationsResponse201
    """

    return (
        await asyncio_detailed(
            client=client,
            body=body,
        )
    ).parsed
