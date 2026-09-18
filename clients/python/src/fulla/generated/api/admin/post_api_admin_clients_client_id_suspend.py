from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.post_api_admin_clients_client_id_suspend_body import PostApiAdminClientsClientIdSuspendBody
from ...models.post_api_admin_clients_client_id_suspend_response_200 import (
    PostApiAdminClientsClientIdSuspendResponse200,
)
from ...types import UNSET, Response, Unset


def _get_kwargs(
    client_id: str,
    *,
    body: PostApiAdminClientsClientIdSuspendBody | Unset = UNSET,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/admin/clients/{client_id}/suspend".format(
            client_id=quote(str(client_id), safe=""),
        ),
    }

    if not isinstance(body, Unset):
        _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> ErrorEnvelope | PostApiAdminClientsClientIdSuspendResponse200 | None:
    if response.status_code == 200:
        response_200 = PostApiAdminClientsClientIdSuspendResponse200.from_dict(response.json())

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
) -> Response[ErrorEnvelope | PostApiAdminClientsClientIdSuspendResponse200]:
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
    body: PostApiAdminClientsClientIdSuspendBody | Unset = UNSET,
) -> Response[ErrorEnvelope | PostApiAdminClientsClientIdSuspendResponse200]:
    """Suspend Application

     Suspend a self-registered application (abuse response). While suspended the client validation fails,
    so no new authorization codes or token exchanges succeed. Admin-managed clients (no owners row)
    cannot be suspended here.

    Args:
        client_id (str):
        body (PostApiAdminClientsClientIdSuspendBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiAdminClientsClientIdSuspendResponse200]
    """

    kwargs = _get_kwargs(
        client_id=client_id,
        body=body,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    client_id: str,
    *,
    client: AuthenticatedClient,
    body: PostApiAdminClientsClientIdSuspendBody | Unset = UNSET,
) -> ErrorEnvelope | PostApiAdminClientsClientIdSuspendResponse200 | None:
    """Suspend Application

     Suspend a self-registered application (abuse response). While suspended the client validation fails,
    so no new authorization codes or token exchanges succeed. Admin-managed clients (no owners row)
    cannot be suspended here.

    Args:
        client_id (str):
        body (PostApiAdminClientsClientIdSuspendBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiAdminClientsClientIdSuspendResponse200
    """

    return sync_detailed(
        client_id=client_id,
        client=client,
        body=body,
    ).parsed


async def asyncio_detailed(
    client_id: str,
    *,
    client: AuthenticatedClient,
    body: PostApiAdminClientsClientIdSuspendBody | Unset = UNSET,
) -> Response[ErrorEnvelope | PostApiAdminClientsClientIdSuspendResponse200]:
    """Suspend Application

     Suspend a self-registered application (abuse response). While suspended the client validation fails,
    so no new authorization codes or token exchanges succeed. Admin-managed clients (no owners row)
    cannot be suspended here.

    Args:
        client_id (str):
        body (PostApiAdminClientsClientIdSuspendBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiAdminClientsClientIdSuspendResponse200]
    """

    kwargs = _get_kwargs(
        client_id=client_id,
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    client_id: str,
    *,
    client: AuthenticatedClient,
    body: PostApiAdminClientsClientIdSuspendBody | Unset = UNSET,
) -> ErrorEnvelope | PostApiAdminClientsClientIdSuspendResponse200 | None:
    """Suspend Application

     Suspend a self-registered application (abuse response). While suspended the client validation fails,
    so no new authorization codes or token exchanges succeed. Admin-managed clients (no owners row)
    cannot be suspended here.

    Args:
        client_id (str):
        body (PostApiAdminClientsClientIdSuspendBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiAdminClientsClientIdSuspendResponse200
    """

    return (
        await asyncio_detailed(
            client_id=client_id,
            client=client,
            body=body,
        )
    ).parsed
