from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.delete_api_me_organizations_slug_members_user_id_response_200 import (
    DeleteApiMeOrganizationsSlugMembersUserIdResponse200,
)
from ...models.error_envelope import ErrorEnvelope
from ...types import Response


def _get_kwargs(
    slug: str,
    user_id: str,
) -> dict[str, Any]:

    _kwargs: dict[str, Any] = {
        "method": "delete",
        "url": "/api/me/organizations/{slug}/members/{user_id}".format(
            slug=quote(str(slug), safe=""),
            user_id=quote(str(user_id), safe=""),
        ),
    }

    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> DeleteApiMeOrganizationsSlugMembersUserIdResponse200 | ErrorEnvelope | None:
    if response.status_code == 200:
        response_200 = DeleteApiMeOrganizationsSlugMembersUserIdResponse200.from_dict(response.json())

        return response_200

    if response.status_code == 400:
        response_400 = ErrorEnvelope.from_dict(response.json())

        return response_400

    if response.status_code == 401:
        response_401 = ErrorEnvelope.from_dict(response.json())

        return response_401

    if response.status_code == 403:
        response_403 = ErrorEnvelope.from_dict(response.json())

        return response_403

    if response.status_code == 404:
        response_404 = ErrorEnvelope.from_dict(response.json())

        return response_404

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[DeleteApiMeOrganizationsSlugMembersUserIdResponse200 | ErrorEnvelope]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    slug: str,
    user_id: str,
    *,
    client: AuthenticatedClient,
) -> Response[DeleteApiMeOrganizationsSlugMembersUserIdResponse200 | ErrorEnvelope]:
    """Remove Organization Member

     Remove a member. Owner removes anyone except self; admins remove members only; members may only
    remove themselves (leave). The owner seat cannot be removed.

    Args:
        slug (str):
        user_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[DeleteApiMeOrganizationsSlugMembersUserIdResponse200 | ErrorEnvelope]
    """

    kwargs = _get_kwargs(
        slug=slug,
        user_id=user_id,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    slug: str,
    user_id: str,
    *,
    client: AuthenticatedClient,
) -> DeleteApiMeOrganizationsSlugMembersUserIdResponse200 | ErrorEnvelope | None:
    """Remove Organization Member

     Remove a member. Owner removes anyone except self; admins remove members only; members may only
    remove themselves (leave). The owner seat cannot be removed.

    Args:
        slug (str):
        user_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        DeleteApiMeOrganizationsSlugMembersUserIdResponse200 | ErrorEnvelope
    """

    return sync_detailed(
        slug=slug,
        user_id=user_id,
        client=client,
    ).parsed


async def asyncio_detailed(
    slug: str,
    user_id: str,
    *,
    client: AuthenticatedClient,
) -> Response[DeleteApiMeOrganizationsSlugMembersUserIdResponse200 | ErrorEnvelope]:
    """Remove Organization Member

     Remove a member. Owner removes anyone except self; admins remove members only; members may only
    remove themselves (leave). The owner seat cannot be removed.

    Args:
        slug (str):
        user_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[DeleteApiMeOrganizationsSlugMembersUserIdResponse200 | ErrorEnvelope]
    """

    kwargs = _get_kwargs(
        slug=slug,
        user_id=user_id,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    slug: str,
    user_id: str,
    *,
    client: AuthenticatedClient,
) -> DeleteApiMeOrganizationsSlugMembersUserIdResponse200 | ErrorEnvelope | None:
    """Remove Organization Member

     Remove a member. Owner removes anyone except self; admins remove members only; members may only
    remove themselves (leave). The owner seat cannot be removed.

    Args:
        slug (str):
        user_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        DeleteApiMeOrganizationsSlugMembersUserIdResponse200 | ErrorEnvelope
    """

    return (
        await asyncio_detailed(
            slug=slug,
            user_id=user_id,
            client=client,
        )
    ).parsed
