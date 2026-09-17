from http import HTTPStatus
from typing import Any

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.post_api_me_applications_body import PostApiMeApplicationsBody
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


def _parse_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> Any | None:
    if response.status_code == 201:
        return None

    if response.status_code == 401:
        return None

    if response.status_code == 403:
        return None

    if response.status_code == 409:
        return None

    if response.status_code == 429:
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
    *,
    client: AuthenticatedClient,
    body: PostApiMeApplicationsBody,
) -> Response[Any]:
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
        Response[Any]
    """

    kwargs = _get_kwargs(
        body=body,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


async def asyncio_detailed(
    *,
    client: AuthenticatedClient,
    body: PostApiMeApplicationsBody,
) -> Response[Any]:
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
        Response[Any]
    """

    kwargs = _get_kwargs(
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)
