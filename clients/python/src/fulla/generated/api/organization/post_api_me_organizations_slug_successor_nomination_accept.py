from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.post_api_me_organizations_slug_successor_nomination_accept_body import (
    PostApiMeOrganizationsSlugSuccessorNominationAcceptBody,
)
from ...models.post_api_me_organizations_slug_successor_nomination_accept_response_200 import (
    PostApiMeOrganizationsSlugSuccessorNominationAcceptResponse200,
)
from ...types import UNSET, Response, Unset


def _get_kwargs(
    slug: str,
    *,
    body: PostApiMeOrganizationsSlugSuccessorNominationAcceptBody | Unset = UNSET,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/me/organizations/{slug}/successor-nomination/accept".format(
            slug=quote(str(slug), safe=""),
        ),
    }

    if not isinstance(body, Unset):
        _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> ErrorEnvelope | PostApiMeOrganizationsSlugSuccessorNominationAcceptResponse200 | None:
    if response.status_code == 200:
        response_200 = PostApiMeOrganizationsSlugSuccessorNominationAcceptResponse200.from_dict(response.json())

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

    if response.status_code == 409:
        response_409 = ErrorEnvelope.from_dict(response.json())

        return response_409

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugSuccessorNominationAcceptResponse200]:
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
    body: PostApiMeOrganizationsSlugSuccessorNominationAcceptBody | Unset = UNSET,
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugSuccessorNominationAcceptResponse200]:
    """Accept Succession

     The nominated successor accepts. The seat swap is one transaction: the nominee's membership becomes
    owner, every other owner row demotes to admin, and the nomination is marked accepted (all or
    nothing).

    Args:
        slug (str):
        body (PostApiMeOrganizationsSlugSuccessorNominationAcceptBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeOrganizationsSlugSuccessorNominationAcceptResponse200]
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
    body: PostApiMeOrganizationsSlugSuccessorNominationAcceptBody | Unset = UNSET,
) -> ErrorEnvelope | PostApiMeOrganizationsSlugSuccessorNominationAcceptResponse200 | None:
    """Accept Succession

     The nominated successor accepts. The seat swap is one transaction: the nominee's membership becomes
    owner, every other owner row demotes to admin, and the nomination is marked accepted (all or
    nothing).

    Args:
        slug (str):
        body (PostApiMeOrganizationsSlugSuccessorNominationAcceptBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeOrganizationsSlugSuccessorNominationAcceptResponse200
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
    body: PostApiMeOrganizationsSlugSuccessorNominationAcceptBody | Unset = UNSET,
) -> Response[ErrorEnvelope | PostApiMeOrganizationsSlugSuccessorNominationAcceptResponse200]:
    """Accept Succession

     The nominated successor accepts. The seat swap is one transaction: the nominee's membership becomes
    owner, every other owner row demotes to admin, and the nomination is marked accepted (all or
    nothing).

    Args:
        slug (str):
        body (PostApiMeOrganizationsSlugSuccessorNominationAcceptBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiMeOrganizationsSlugSuccessorNominationAcceptResponse200]
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
    body: PostApiMeOrganizationsSlugSuccessorNominationAcceptBody | Unset = UNSET,
) -> ErrorEnvelope | PostApiMeOrganizationsSlugSuccessorNominationAcceptResponse200 | None:
    """Accept Succession

     The nominated successor accepts. The seat swap is one transaction: the nominee's membership becomes
    owner, every other owner row demotes to admin, and the nomination is marked accepted (all or
    nothing).

    Args:
        slug (str):
        body (PostApiMeOrganizationsSlugSuccessorNominationAcceptBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiMeOrganizationsSlugSuccessorNominationAcceptResponse200
    """

    return (
        await asyncio_detailed(
            slug=slug,
            client=client,
            body=body,
        )
    ).parsed
