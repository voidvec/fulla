from http import HTTPStatus
from typing import Any, cast
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.post_api_admin_organizations_slug_transfer_ownership_body import (
    PostApiAdminOrganizationsSlugTransferOwnershipBody,
)
from ...models.post_api_admin_organizations_slug_transfer_ownership_response_200 import (
    PostApiAdminOrganizationsSlugTransferOwnershipResponse200,
)
from ...types import Response


def _get_kwargs(
    slug: str,
    *,
    body: PostApiAdminOrganizationsSlugTransferOwnershipBody,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/admin/organizations/{slug}/transfer-ownership".format(
            slug=quote(str(slug), safe=""),
        ),
    }

    _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Any | ErrorEnvelope | PostApiAdminOrganizationsSlugTransferOwnershipResponse200 | None:
    if response.status_code == 200:
        response_200 = PostApiAdminOrganizationsSlugTransferOwnershipResponse200.from_dict(response.json())

        return response_200

    if response.status_code == 400:
        response_400 = ErrorEnvelope.from_dict(response.json())

        return response_400

    if response.status_code == 401:
        response_401 = cast(Any, None)
        return response_401

    if response.status_code == 404:
        response_404 = cast(Any, None)
        return response_404

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[Any | ErrorEnvelope | PostApiAdminOrganizationsSlugTransferOwnershipResponse200]:
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
    body: PostApiAdminOrganizationsSlugTransferOwnershipBody,
) -> Response[Any | ErrorEnvelope | PostApiAdminOrganizationsSlugTransferOwnershipResponse200]:
    """Transfer Organization Ownership

     Reassign the organization's owner seat to a live user (#221 admin override). The target is promoted
    (or added) as owner; previous owner rows are demoted to admin. Resolves the soft-deleted-owner
    deadlock where an org becomes unmanageable.

    Args:
        slug (str):
        body (PostApiAdminOrganizationsSlugTransferOwnershipBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | ErrorEnvelope | PostApiAdminOrganizationsSlugTransferOwnershipResponse200]
    """

    kwargs = _get_kwargs(
        slug=slug,
        body=body,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    slug: str,
    *,
    client: AuthenticatedClient,
    body: PostApiAdminOrganizationsSlugTransferOwnershipBody,
) -> Any | ErrorEnvelope | PostApiAdminOrganizationsSlugTransferOwnershipResponse200 | None:
    """Transfer Organization Ownership

     Reassign the organization's owner seat to a live user (#221 admin override). The target is promoted
    (or added) as owner; previous owner rows are demoted to admin. Resolves the soft-deleted-owner
    deadlock where an org becomes unmanageable.

    Args:
        slug (str):
        body (PostApiAdminOrganizationsSlugTransferOwnershipBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | ErrorEnvelope | PostApiAdminOrganizationsSlugTransferOwnershipResponse200
    """

    return sync_detailed(
        slug=slug,
        client=client,
        body=body,
    ).parsed


async def asyncio_detailed(
    slug: str,
    *,
    client: AuthenticatedClient,
    body: PostApiAdminOrganizationsSlugTransferOwnershipBody,
) -> Response[Any | ErrorEnvelope | PostApiAdminOrganizationsSlugTransferOwnershipResponse200]:
    """Transfer Organization Ownership

     Reassign the organization's owner seat to a live user (#221 admin override). The target is promoted
    (or added) as owner; previous owner rows are demoted to admin. Resolves the soft-deleted-owner
    deadlock where an org becomes unmanageable.

    Args:
        slug (str):
        body (PostApiAdminOrganizationsSlugTransferOwnershipBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | ErrorEnvelope | PostApiAdminOrganizationsSlugTransferOwnershipResponse200]
    """

    kwargs = _get_kwargs(
        slug=slug,
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    slug: str,
    *,
    client: AuthenticatedClient,
    body: PostApiAdminOrganizationsSlugTransferOwnershipBody,
) -> Any | ErrorEnvelope | PostApiAdminOrganizationsSlugTransferOwnershipResponse200 | None:
    """Transfer Organization Ownership

     Reassign the organization's owner seat to a live user (#221 admin override). The target is promoted
    (or added) as owner; previous owner rows are demoted to admin. Resolves the soft-deleted-owner
    deadlock where an org becomes unmanageable.

    Args:
        slug (str):
        body (PostApiAdminOrganizationsSlugTransferOwnershipBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | ErrorEnvelope | PostApiAdminOrganizationsSlugTransferOwnershipResponse200
    """

    return (
        await asyncio_detailed(
            slug=slug,
            client=client,
            body=body,
        )
    ).parsed
