from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.post_api_me_organizations_slug_invitations_body import PostApiMeOrganizationsSlugInvitationsBody
from ...models.post_api_me_organizations_slug_invitations_response_201 import (
    PostApiMeOrganizationsSlugInvitationsResponse201,
)
from ...types import Response


def _get_kwargs(
    slug: str,
    *,
    body: PostApiMeOrganizationsSlugInvitationsBody,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/me/organizations/{slug}/invitations".format(
            slug=quote(str(slug), safe=""),
        ),
    }

    _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> ErrorEnvelope | PostApiMeOrganizationsSlugInvitationsResponse201 | None:
    if response.status_code == 201:
        response_201 = PostApiMeOrganizationsSlugInvitationsResponse201.from_dict(response.json())

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

    if response.status_code == 404:
        response_404 = ErrorEnvelope.from_dict(response.json())

        return response_404

    if response.status_code == 409:
        response_409 = ErrorEnvelope.from_dict(response.json())

        return response_409

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugInvitationsResponse201]:
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
    body: PostApiMeOrganizationsSlugInvitationsBody,
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugInvitationsResponse201]:
    """Invite Organization Member

     Create a single-use 72h invitation (org owner/admin). The token is returned once and must be
    delivered out-of-band; email delivery is not part of v1.4.0.

    Args:
        slug (str):
        body (PostApiMeOrganizationsSlugInvitationsBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeOrganizationsSlugInvitationsResponse201]
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
    body: PostApiMeOrganizationsSlugInvitationsBody,
) -> ErrorEnvelope | PostApiMeOrganizationsSlugInvitationsResponse201 | None:
    """Invite Organization Member

     Create a single-use 72h invitation (org owner/admin). The token is returned once and must be
    delivered out-of-band; email delivery is not part of v1.4.0.

    Args:
        slug (str):
        body (PostApiMeOrganizationsSlugInvitationsBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeOrganizationsSlugInvitationsResponse201
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
    body: PostApiMeOrganizationsSlugInvitationsBody,
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugInvitationsResponse201]:
    """Invite Organization Member

     Create a single-use 72h invitation (org owner/admin). The token is returned once and must be
    delivered out-of-band; email delivery is not part of v1.4.0.

    Args:
        slug (str):
        body (PostApiMeOrganizationsSlugInvitationsBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeOrganizationsSlugInvitationsResponse201]
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
    body: PostApiMeOrganizationsSlugInvitationsBody,
) -> ErrorEnvelope | PostApiMeOrganizationsSlugInvitationsResponse201 | None:
    """Invite Organization Member

     Create a single-use 72h invitation (org owner/admin). The token is returned once and must be
    delivered out-of-band; email delivery is not part of v1.4.0.

    Args:
        slug (str):
        body (PostApiMeOrganizationsSlugInvitationsBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeOrganizationsSlugInvitationsResponse201
    """

    return (
        await asyncio_detailed(
            slug=slug,
            client=client,
            body=body,
        )
    ).parsed
