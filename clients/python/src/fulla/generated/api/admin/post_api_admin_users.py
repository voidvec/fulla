from http import HTTPStatus
from typing import Any

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.post_api_admin_users_body import PostApiAdminUsersBody
from ...types import Response


def _get_kwargs(
    *,
    body: PostApiAdminUsersBody,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/admin/users",
    }

    _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> Any | None:
    if response.status_code == 201:
        return None

    if response.status_code == 400:
        return None

    if response.status_code == 401:
        return None

    if response.status_code == 403:
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
    body: PostApiAdminUsersBody,
) -> Response[Any]:
    """Create User

     Create a new user. Requires username and password; email, roles, mfa_enabled, email_verified, and
    must_change_password are optional. must_change_password (default false, #145) forces the user to
    change the password at first login. The deprecated users.org_id column is read-only since v1.5.0:
    requests containing org_id are rejected with 400 (organization membership is managed via the
    organization APIs).

    Args:
        body (PostApiAdminUsersBody):

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
    body: PostApiAdminUsersBody,
) -> Response[Any]:
    """Create User

     Create a new user. Requires username and password; email, roles, mfa_enabled, email_verified, and
    must_change_password are optional. must_change_password (default false, #145) forces the user to
    change the password at first login. The deprecated users.org_id column is read-only since v1.5.0:
    requests containing org_id are rejected with 400 (organization membership is managed via the
    organization APIs).

    Args:
        body (PostApiAdminUsersBody):

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
