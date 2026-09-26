from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.post_api_me_organizations_slug_consent_requests_request_id_approve_body import (
    PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveBody,
)
from ...models.post_api_me_organizations_slug_consent_requests_request_id_approve_response_200 import (
    PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200,
)
from ...types import UNSET, Response, Unset


def _get_kwargs(
    slug: str,
    request_id: int,
    *,
    body: PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveBody | Unset = UNSET,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/me/organizations/{slug}/consent-requests/{request_id}/approve".format(
            slug=quote(str(slug), safe=""),
            request_id=quote(str(request_id), safe=""),
        ),
    }

    if not isinstance(body, Unset):
        _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200 | None:
    if response.status_code == 200:
        response_200 = PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200.from_dict(response.json())

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

    if response.status_code == 500:
        response_500 = ErrorEnvelope.from_dict(response.json())

        return response_500

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    slug: str,
    request_id: int,
    *,
    client: AuthenticatedClient,
    body: PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveBody | Unset = UNSET,
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200]:
    """Approve Consent Request

     Approve a pending request (org owner/admin): one organization_consents row per scope of the client's
    registered set is written and sibling pending requests for the same (org, client) are auto-approved.
    Idempotent on already-approved rows; 409 on rejected rows.

    Args:
        slug (str):
        request_id (int):
        body (PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200]
    """

    kwargs = _get_kwargs(
        slug=slug,
        request_id=request_id,
        body=body,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    slug: str,
    request_id: int,
    *,
    client: AuthenticatedClient,
    body: PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveBody | Unset = UNSET,
) -> ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200 | None:
    """Approve Consent Request

     Approve a pending request (org owner/admin): one organization_consents row per scope of the client's
    registered set is written and sibling pending requests for the same (org, client) are auto-approved.
    Idempotent on already-approved rows; 409 on rejected rows.

    Args:
        slug (str):
        request_id (int):
        body (PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200
    """

    return sync_detailed(
        slug=slug,
        request_id=request_id,
        client=client,
        body=body,
    ).parsed


async def asyncio_detailed(
    slug: str,
    request_id: int,
    *,
    client: AuthenticatedClient,
    body: PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveBody | Unset = UNSET,
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200]:
    """Approve Consent Request

     Approve a pending request (org owner/admin): one organization_consents row per scope of the client's
    registered set is written and sibling pending requests for the same (org, client) are auto-approved.
    Idempotent on already-approved rows; 409 on rejected rows.

    Args:
        slug (str):
        request_id (int):
        body (PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200]
    """

    kwargs = _get_kwargs(
        slug=slug,
        request_id=request_id,
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    slug: str,
    request_id: int,
    *,
    client: AuthenticatedClient,
    body: PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveBody | Unset = UNSET,
) -> ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200 | None:
    """Approve Consent Request

     Approve a pending request (org owner/admin): one organization_consents row per scope of the client's
    registered set is written and sibling pending requests for the same (org, client) are auto-approved.
    Idempotent on already-approved rows; 409 on rejected rows.

    Args:
        slug (str):
        request_id (int):
        body (PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdApproveResponse200
    """

    return (
        await asyncio_detailed(
            slug=slug,
            request_id=request_id,
            client=client,
            body=body,
        )
    ).parsed
