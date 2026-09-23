from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.delete_api_me_organizations_slug_consents_client_id_response_200 import (
    DeleteApiMeOrganizationsSlugConsentsClientIdResponse200,
)
from ...models.error_envelope import ErrorEnvelope
from ...types import Response


def _get_kwargs(
    slug: str,
    client_id: str,
) -> dict[str, Any]:

    _kwargs: dict[str, Any] = {
        "method": "delete",
        "url": "/api/me/organizations/{slug}/consents/{client_id}".format(
            slug=quote(str(slug), safe=""),
            client_id=quote(str(client_id), safe=""),
        ),
    }

    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> DeleteApiMeOrganizationsSlugConsentsClientIdResponse200 | ErrorEnvelope | None:
    if response.status_code == 200:
        response_200 = DeleteApiMeOrganizationsSlugConsentsClientIdResponse200.from_dict(response.json())

        return response_200

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
) -> Response[DeleteApiMeOrganizationsSlugConsentsClientIdResponse200 | ErrorEnvelope]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    slug: str,
    client_id: str,
    *,
    client: AuthenticatedClient,
) -> Response[DeleteApiMeOrganizationsSlugConsentsClientIdResponse200 | ErrorEnvelope]:
    """Revoke Organization Consents

     Revoke every active consent of the (org, client) pair (org owner/admin). Revocation only affects
    FUTURE authorizations; issued tokens are not revoked (O4). 404 when no active rows remain.

    Args:
        slug (str):
        client_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[DeleteApiMeOrganizationsSlugConsentsClientIdResponse200 | ErrorEnvelope]
    """

    kwargs = _get_kwargs(
        slug=slug,
        client_id=client_id,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    slug: str,
    client_id: str,
    *,
    client: AuthenticatedClient,
) -> DeleteApiMeOrganizationsSlugConsentsClientIdResponse200 | ErrorEnvelope | None:
    """Revoke Organization Consents

     Revoke every active consent of the (org, client) pair (org owner/admin). Revocation only affects
    FUTURE authorizations; issued tokens are not revoked (O4). 404 when no active rows remain.

    Args:
        slug (str):
        client_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        DeleteApiMeOrganizationsSlugConsentsClientIdResponse200 | ErrorEnvelope
    """

    return sync_detailed(
        slug=slug,
        client_id=client_id,
        client=client,
    ).parsed


async def asyncio_detailed(
    slug: str,
    client_id: str,
    *,
    client: AuthenticatedClient,
) -> Response[DeleteApiMeOrganizationsSlugConsentsClientIdResponse200 | ErrorEnvelope]:
    """Revoke Organization Consents

     Revoke every active consent of the (org, client) pair (org owner/admin). Revocation only affects
    FUTURE authorizations; issued tokens are not revoked (O4). 404 when no active rows remain.

    Args:
        slug (str):
        client_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[DeleteApiMeOrganizationsSlugConsentsClientIdResponse200 | ErrorEnvelope]
    """

    kwargs = _get_kwargs(
        slug=slug,
        client_id=client_id,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    slug: str,
    client_id: str,
    *,
    client: AuthenticatedClient,
) -> DeleteApiMeOrganizationsSlugConsentsClientIdResponse200 | ErrorEnvelope | None:
    """Revoke Organization Consents

     Revoke every active consent of the (org, client) pair (org owner/admin). Revocation only affects
    FUTURE authorizations; issued tokens are not revoked (O4). 404 when no active rows remain.

    Args:
        slug (str):
        client_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        DeleteApiMeOrganizationsSlugConsentsClientIdResponse200 | ErrorEnvelope
    """

    return (
        await asyncio_detailed(
            slug=slug,
            client_id=client_id,
            client=client,
        )
    ).parsed
