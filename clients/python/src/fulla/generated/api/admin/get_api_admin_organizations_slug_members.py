from http import HTTPStatus
from typing import Any, cast
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.get_api_admin_organizations_slug_members_response_200 import (
    GetApiAdminOrganizationsSlugMembersResponse200,
)
from ...types import Response


def _get_kwargs(
    slug: str,
) -> dict[str, Any]:

    _kwargs: dict[str, Any] = {
        "method": "get",
        "url": "/api/admin/organizations/{slug}/members".format(
            slug=quote(str(slug), safe=""),
        ),
    }

    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Any | GetApiAdminOrganizationsSlugMembersResponse200 | None:
    if response.status_code == 200:
        response_200 = GetApiAdminOrganizationsSlugMembersResponse200.from_dict(response.json())

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
) -> Response[Any | GetApiAdminOrganizationsSlugMembersResponse200]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    slug: str,
    *,
    client: AuthenticatedClient,
) -> Response[Any | GetApiAdminOrganizationsSlugMembersResponse200]:
    """List Organization Members

     P1-4 (#281): read-only member roster (user_id, username, role, joined_at).

    Args:
        slug (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | GetApiAdminOrganizationsSlugMembersResponse200]
    """

    kwargs = _get_kwargs(
        slug=slug,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    slug: str,
    *,
    client: AuthenticatedClient,
) -> Any | GetApiAdminOrganizationsSlugMembersResponse200 | None:
    """List Organization Members

     P1-4 (#281): read-only member roster (user_id, username, role, joined_at).

    Args:
        slug (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | GetApiAdminOrganizationsSlugMembersResponse200
    """

    return sync_detailed(
        slug=slug,
        client=client,
    ).parsed


async def asyncio_detailed(
    slug: str,
    *,
    client: AuthenticatedClient,
) -> Response[Any | GetApiAdminOrganizationsSlugMembersResponse200]:
    """List Organization Members

     P1-4 (#281): read-only member roster (user_id, username, role, joined_at).

    Args:
        slug (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | GetApiAdminOrganizationsSlugMembersResponse200]
    """

    kwargs = _get_kwargs(
        slug=slug,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    slug: str,
    *,
    client: AuthenticatedClient,
) -> Any | GetApiAdminOrganizationsSlugMembersResponse200 | None:
    """List Organization Members

     P1-4 (#281): read-only member roster (user_id, username, role, joined_at).

    Args:
        slug (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | GetApiAdminOrganizationsSlugMembersResponse200
    """

    return (
        await asyncio_detailed(
            slug=slug,
            client=client,
        )
    ).parsed
