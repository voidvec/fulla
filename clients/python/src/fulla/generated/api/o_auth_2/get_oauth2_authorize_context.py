from http import HTTPStatus
from typing import Any, cast

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.get_oauth_2_authorize_context_response_200 import GetOauth2AuthorizeContextResponse200
from ...types import UNSET, Response


def _get_kwargs(
    *,
    client_id: str,
) -> dict[str, Any]:

    params: dict[str, Any] = {}

    params["client_id"] = client_id

    params = {k: v for k, v in params.items() if v is not UNSET and v is not None}

    _kwargs: dict[str, Any] = {
        "method": "get",
        "url": "/oauth2/authorize-context",
        "params": params,
    }

    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Any | GetOauth2AuthorizeContextResponse200 | None:
    if response.status_code == 200:
        response_200 = GetOauth2AuthorizeContextResponse200.from_dict(response.json())

        return response_200

    if response.status_code == 400:
        response_400 = cast(Any, None)
        return response_400

    if response.status_code == 404:
        response_404 = cast(Any, None)
        return response_404

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[Any | GetOauth2AuthorizeContextResponse200]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    *,
    client: AuthenticatedClient | Client,
    client_id: str,
) -> Response[Any | GetOauth2AuthorizeContextResponse200]:
    """Authorize Context (client display name)

     Public application-name discovery for the login/consent screens (P0-6): answers {client_name} for
    active clients, so the user sees WHICH application is asking for credentials before typing a
    password. Unknown or soft-deleted clients answer 404; every other failure degrades to {client_name:
    <client_id>}. Carries only the display name — client_id is not a secret (it appears in every
    authorize redirect).

    Args:
        client_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | GetOauth2AuthorizeContextResponse200]
    """

    kwargs = _get_kwargs(
        client_id=client_id,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    *,
    client: AuthenticatedClient | Client,
    client_id: str,
) -> Any | GetOauth2AuthorizeContextResponse200 | None:
    """Authorize Context (client display name)

     Public application-name discovery for the login/consent screens (P0-6): answers {client_name} for
    active clients, so the user sees WHICH application is asking for credentials before typing a
    password. Unknown or soft-deleted clients answer 404; every other failure degrades to {client_name:
    <client_id>}. Carries only the display name — client_id is not a secret (it appears in every
    authorize redirect).

    Args:
        client_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | GetOauth2AuthorizeContextResponse200
    """

    return sync_detailed(
        client=client,
        client_id=client_id,
    ).parsed


async def asyncio_detailed(
    *,
    client: AuthenticatedClient | Client,
    client_id: str,
) -> Response[Any | GetOauth2AuthorizeContextResponse200]:
    """Authorize Context (client display name)

     Public application-name discovery for the login/consent screens (P0-6): answers {client_name} for
    active clients, so the user sees WHICH application is asking for credentials before typing a
    password. Unknown or soft-deleted clients answer 404; every other failure degrades to {client_name:
    <client_id>}. Carries only the display name — client_id is not a secret (it appears in every
    authorize redirect).

    Args:
        client_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | GetOauth2AuthorizeContextResponse200]
    """

    kwargs = _get_kwargs(
        client_id=client_id,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    *,
    client: AuthenticatedClient | Client,
    client_id: str,
) -> Any | GetOauth2AuthorizeContextResponse200 | None:
    """Authorize Context (client display name)

     Public application-name discovery for the login/consent screens (P0-6): answers {client_name} for
    active clients, so the user sees WHICH application is asking for credentials before typing a
    password. Unknown or soft-deleted clients answer 404; every other failure degrades to {client_name:
    <client_id>}. Carries only the display name — client_id is not a secret (it appears in every
    authorize redirect).

    Args:
        client_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | GetOauth2AuthorizeContextResponse200
    """

    return (
        await asyncio_detailed(
            client=client,
            client_id=client_id,
        )
    ).parsed
