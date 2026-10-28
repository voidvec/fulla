from http import HTTPStatus
from typing import Any, cast
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.get_api_admin_roles_role_id_permissions_response_200 import GetApiAdminRolesRoleIdPermissionsResponse200
from ...types import Response


def _get_kwargs(
    role_id: int,
) -> dict[str, Any]:

    _kwargs: dict[str, Any] = {
        "method": "get",
        "url": "/api/admin/roles/{role_id}/permissions".format(
            role_id=quote(str(role_id), safe=""),
        ),
    }

    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Any | GetApiAdminRolesRoleIdPermissionsResponse200 | None:
    if response.status_code == 200:
        response_200 = GetApiAdminRolesRoleIdPermissionsResponse200.from_dict(response.json())

        return response_200

    if response.status_code == 404:
        response_404 = cast(Any, None)
        return response_404

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[Any | GetApiAdminRolesRoleIdPermissionsResponse200]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    role_id: int,
    *,
    client: AuthenticatedClient,
) -> Response[Any | GetApiAdminRolesRoleIdPermissionsResponse200]:
    """List Role Permissions

     P1-8 (#285): the role's permission set (delegated administration surface).

    Args:
        role_id (int):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | GetApiAdminRolesRoleIdPermissionsResponse200]
    """

    kwargs = _get_kwargs(
        role_id=role_id,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    role_id: int,
    *,
    client: AuthenticatedClient,
) -> Any | GetApiAdminRolesRoleIdPermissionsResponse200 | None:
    """List Role Permissions

     P1-8 (#285): the role's permission set (delegated administration surface).

    Args:
        role_id (int):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | GetApiAdminRolesRoleIdPermissionsResponse200
    """

    return sync_detailed(
        role_id=role_id,
        client=client,
    ).parsed


async def asyncio_detailed(
    role_id: int,
    *,
    client: AuthenticatedClient,
) -> Response[Any | GetApiAdminRolesRoleIdPermissionsResponse200]:
    """List Role Permissions

     P1-8 (#285): the role's permission set (delegated administration surface).

    Args:
        role_id (int):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | GetApiAdminRolesRoleIdPermissionsResponse200]
    """

    kwargs = _get_kwargs(
        role_id=role_id,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    role_id: int,
    *,
    client: AuthenticatedClient,
) -> Any | GetApiAdminRolesRoleIdPermissionsResponse200 | None:
    """List Role Permissions

     P1-8 (#285): the role's permission set (delegated administration surface).

    Args:
        role_id (int):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | GetApiAdminRolesRoleIdPermissionsResponse200
    """

    return (
        await asyncio_detailed(
            role_id=role_id,
            client=client,
        )
    ).parsed
