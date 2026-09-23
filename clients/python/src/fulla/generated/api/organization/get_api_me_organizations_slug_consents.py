from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.get_api_me_organizations_slug_consents_response_200 import GetApiMeOrganizationsSlugConsentsResponse200
from ...types import Response


def _get_kwargs(
    slug: str,
) -> dict[str, Any]:

    _kwargs: dict[str, Any] = {
        "method": "get",
        "url": "/api/me/organizations/{slug}/consents".format(
            slug=quote(str(slug), safe=""),
        ),
    }

    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> ErrorEnvelope | GetApiMeOrganizationsSlugConsentsResponse200 | None:
    if response.status_code == 200:
        response_200 = GetApiMeOrganizationsSlugConsentsResponse200.from_dict(response.json())

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
) -> Response[ErrorEnvelope | GetApiMeOrganizationsSlugConsentsResponse200]:
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
) -> Response[ErrorEnvelope | GetApiMeOrganizationsSlugConsentsResponse200]:
    """List Organization Consents

     Active organization consents grouped by client, each scope with its own granted_by / granted_at (org
    owner/admin; v1.5.0 M2). Members of the org skip the personal consent prompt for scopes covered by
    these rows.

    Args:
        slug (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | GetApiMeOrganizationsSlugConsentsResponse200]
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
) -> ErrorEnvelope | GetApiMeOrganizationsSlugConsentsResponse200 | None:
    """List Organization Consents

     Active organization consents grouped by client, each scope with its own granted_by / granted_at (org
    owner/admin; v1.5.0 M2). Members of the org skip the personal consent prompt for scopes covered by
    these rows.

    Args:
        slug (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | GetApiMeOrganizationsSlugConsentsResponse200
    """

    return sync_detailed(
        slug=slug,
        client=client,
    ).parsed


async def asyncio_detailed(
    slug: str,
    *,
    client: AuthenticatedClient,
) -> Response[ErrorEnvelope | GetApiMeOrganizationsSlugConsentsResponse200]:
    """List Organization Consents

     Active organization consents grouped by client, each scope with its own granted_by / granted_at (org
    owner/admin; v1.5.0 M2). Members of the org skip the personal consent prompt for scopes covered by
    these rows.

    Args:
        slug (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | GetApiMeOrganizationsSlugConsentsResponse200]
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
) -> ErrorEnvelope | GetApiMeOrganizationsSlugConsentsResponse200 | None:
    """List Organization Consents

     Active organization consents grouped by client, each scope with its own granted_by / granted_at (org
    owner/admin; v1.5.0 M2). Members of the org skip the personal consent prompt for scopes covered by
    these rows.

    Args:
        slug (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | GetApiMeOrganizationsSlugConsentsResponse200
    """

    return (
        await asyncio_detailed(
            slug=slug,
            client=client,
        )
    ).parsed
