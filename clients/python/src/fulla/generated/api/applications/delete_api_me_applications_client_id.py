from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.delete_api_me_applications_client_id_response_200 import DeleteApiMeApplicationsClientIdResponse200
from ...models.error_envelope import ErrorEnvelope
from ...types import Response


def _get_kwargs(
    client_id: str,
) -> dict[str, Any]:

    _kwargs: dict[str, Any] = {
        "method": "delete",
        "url": "/api/me/applications/{client_id}".format(
            client_id=quote(str(client_id), safe=""),
        ),
    }

    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> DeleteApiMeApplicationsClientIdResponse200 | ErrorEnvelope | None:
    if response.status_code == 200:
        response_200 = DeleteApiMeApplicationsClientIdResponse200.from_dict(response.json())

        return response_200

    if response.status_code == 401:
        response_401 = ErrorEnvelope.from_dict(response.json())

        return response_401

    if response.status_code == 404:
        response_404 = ErrorEnvelope.from_dict(response.json())

        return response_404

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[DeleteApiMeApplicationsClientIdResponse200 | ErrorEnvelope]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    client_id: str,
    *,
    client: AuthenticatedClient,
) -> Response[DeleteApiMeApplicationsClientIdResponse200 | ErrorEnvelope]:
    """Delete Application

     Soft-delete a self-registered application; it disappears from every flow immediately (authorization,
    token, introspection).

    Args:
        client_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[DeleteApiMeApplicationsClientIdResponse200 | ErrorEnvelope]
    """

    kwargs = _get_kwargs(
        client_id=client_id,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    client_id: str,
    *,
    client: AuthenticatedClient,
) -> DeleteApiMeApplicationsClientIdResponse200 | ErrorEnvelope | None:
    """Delete Application

     Soft-delete a self-registered application; it disappears from every flow immediately (authorization,
    token, introspection).

    Args:
        client_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        DeleteApiMeApplicationsClientIdResponse200 | ErrorEnvelope
    """

    return sync_detailed(
        client_id=client_id,
        client=client,
    ).parsed


async def asyncio_detailed(
    client_id: str,
    *,
    client: AuthenticatedClient,
) -> Response[DeleteApiMeApplicationsClientIdResponse200 | ErrorEnvelope]:
    """Delete Application

     Soft-delete a self-registered application; it disappears from every flow immediately (authorization,
    token, introspection).

    Args:
        client_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[DeleteApiMeApplicationsClientIdResponse200 | ErrorEnvelope]
    """

    kwargs = _get_kwargs(
        client_id=client_id,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    client_id: str,
    *,
    client: AuthenticatedClient,
) -> DeleteApiMeApplicationsClientIdResponse200 | ErrorEnvelope | None:
    """Delete Application

     Soft-delete a self-registered application; it disappears from every flow immediately (authorization,
    token, introspection).

    Args:
        client_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        DeleteApiMeApplicationsClientIdResponse200 | ErrorEnvelope
    """

    return (
        await asyncio_detailed(
            client_id=client_id,
            client=client,
        )
    ).parsed
