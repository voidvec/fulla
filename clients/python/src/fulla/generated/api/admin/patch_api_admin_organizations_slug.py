from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.patch_api_admin_organizations_slug_body import PatchApiAdminOrganizationsSlugBody
from ...types import Response


def _get_kwargs(
    slug: str,
    *,
    body: PatchApiAdminOrganizationsSlugBody,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "patch",
        "url": "/api/admin/organizations/{slug}".format(
            slug=quote(str(slug), safe=""),
        ),
    }

    _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> Any | None:
    if response.status_code == 200:
        return None

    if response.status_code == 400:
        return None

    if response.status_code == 404:
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
    slug: str,
    *,
    client: AuthenticatedClient,
    body: PatchApiAdminOrganizationsSlugBody,
) -> Response[Any]:
    """Update Organization

     P1-4 (#281): edit name/logo_uri/primary_color (slug is immutable — external reference anchor). Brand
    fields validated with the P1-1 rules (https-only logo + loopback exemption; color
    ^#[0-9a-fA-F]{3,6}$). Audited as org_updated.

    Args:
        slug (str):
        body (PatchApiAdminOrganizationsSlugBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any]
    """

    kwargs = _get_kwargs(
        slug=slug,
        body=body,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


async def asyncio_detailed(
    slug: str,
    *,
    client: AuthenticatedClient,
    body: PatchApiAdminOrganizationsSlugBody,
) -> Response[Any]:
    """Update Organization

     P1-4 (#281): edit name/logo_uri/primary_color (slug is immutable — external reference anchor). Brand
    fields validated with the P1-1 rules (https-only logo + loopback exemption; color
    ^#[0-9a-fA-F]{3,6}$). Audited as org_updated.

    Args:
        slug (str):
        body (PatchApiAdminOrganizationsSlugBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any]
    """

    kwargs = _get_kwargs(
        slug=slug,
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)
