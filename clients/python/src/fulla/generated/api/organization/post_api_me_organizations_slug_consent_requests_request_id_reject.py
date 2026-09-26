from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.post_api_me_organizations_slug_consent_requests_request_id_reject_body import (
    PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectBody,
)
from ...models.post_api_me_organizations_slug_consent_requests_request_id_reject_response_200 import (
    PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectResponse200,
)
from ...types import UNSET, Response, Unset


def _get_kwargs(
    slug: str,
    request_id: int,
    *,
    body: PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectBody | Unset = UNSET,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/me/organizations/{slug}/consent-requests/{request_id}/reject".format(
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
) -> ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectResponse200 | None:
    if response.status_code == 200:
        response_200 = PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectResponse200.from_dict(response.json())

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
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectResponse200]:
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
    body: PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectBody | Unset = UNSET,
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectResponse200]:
    """Reject Consent Request

     Reject a pending request (org owner/admin); optional body {reason}. Does not cascade to sibling
    requests; the member may re-file.

    Args:
        slug (str):
        request_id (int):
        body (PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectResponse200]
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
    body: PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectBody | Unset = UNSET,
) -> ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectResponse200 | None:
    """Reject Consent Request

     Reject a pending request (org owner/admin); optional body {reason}. Does not cascade to sibling
    requests; the member may re-file.

    Args:
        slug (str):
        request_id (int):
        body (PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectResponse200
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
    body: PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectBody | Unset = UNSET,
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectResponse200]:
    """Reject Consent Request

     Reject a pending request (org owner/admin); optional body {reason}. Does not cascade to sibling
    requests; the member may re-file.

    Args:
        slug (str):
        request_id (int):
        body (PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectResponse200]
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
    body: PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectBody | Unset = UNSET,
) -> ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectResponse200 | None:
    """Reject Consent Request

     Reject a pending request (org owner/admin); optional body {reason}. Does not cascade to sibling
    requests; the member may re-file.

    Args:
        slug (str):
        request_id (int):
        body (PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeOrganizationsSlugConsentRequestsRequestIdRejectResponse200
    """

    return (
        await asyncio_detailed(
            slug=slug,
            request_id=request_id,
            client=client,
            body=body,
        )
    ).parsed
