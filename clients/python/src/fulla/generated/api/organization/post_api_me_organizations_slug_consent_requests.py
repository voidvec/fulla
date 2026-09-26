from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.post_api_me_organizations_slug_consent_requests_body import PostApiMeOrganizationsSlugConsentRequestsBody
from ...models.post_api_me_organizations_slug_consent_requests_response_200 import (
    PostApiMeOrganizationsSlugConsentRequestsResponse200,
)
from ...types import Response


def _get_kwargs(
    slug: str,
    *,
    body: PostApiMeOrganizationsSlugConsentRequestsBody,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/me/organizations/{slug}/consent-requests".format(
            slug=quote(str(slug), safe=""),
        ),
    }

    _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsResponse200 | None:
    if response.status_code == 200:
        response_200 = PostApiMeOrganizationsSlugConsentRequestsResponse200.from_dict(response.json())

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

    if response.status_code == 409:
        response_409 = ErrorEnvelope.from_dict(response.json())

        return response_409

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsResponse200]:
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
    body: PostApiMeOrganizationsSlugConsentRequestsBody,
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsResponse200]:
    """File Organization Consent Request

     A member asks the org managers to grant an organization consent for an application (body
    {client_id}; #236 plan B). Idempotent: re-filing with an identical pending request returns 200 with
    the existing row. 404 for unknown clients, 409 when the client is already org-owned or already
    consented.

    Args:
        slug (str):
        body (PostApiMeOrganizationsSlugConsentRequestsBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsResponse200]
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
    body: PostApiMeOrganizationsSlugConsentRequestsBody,
) -> ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsResponse200 | None:
    """File Organization Consent Request

     A member asks the org managers to grant an organization consent for an application (body
    {client_id}; #236 plan B). Idempotent: re-filing with an identical pending request returns 200 with
    the existing row. 404 for unknown clients, 409 when the client is already org-owned or already
    consented.

    Args:
        slug (str):
        body (PostApiMeOrganizationsSlugConsentRequestsBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsResponse200
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
    body: PostApiMeOrganizationsSlugConsentRequestsBody,
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsResponse200]:
    """File Organization Consent Request

     A member asks the org managers to grant an organization consent for an application (body
    {client_id}; #236 plan B). Idempotent: re-filing with an identical pending request returns 200 with
    the existing row. 404 for unknown clients, 409 when the client is already org-owned or already
    consented.

    Args:
        slug (str):
        body (PostApiMeOrganizationsSlugConsentRequestsBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsResponse200]
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
    body: PostApiMeOrganizationsSlugConsentRequestsBody,
) -> ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsResponse200 | None:
    """File Organization Consent Request

     A member asks the org managers to grant an organization consent for an application (body
    {client_id}; #236 plan B). Idempotent: re-filing with an identical pending request returns 200 with
    the existing row. 404 for unknown clients, 409 when the client is already org-owned or already
    consented.

    Args:
        slug (str):
        body (PostApiMeOrganizationsSlugConsentRequestsBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsResponse200
    """

    return (
        await asyncio_detailed(
            slug=slug,
            client=client,
            body=body,
        )
    ).parsed
