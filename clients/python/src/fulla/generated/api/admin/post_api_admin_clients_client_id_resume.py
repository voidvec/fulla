from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.post_api_admin_clients_client_id_resume_body import PostApiAdminClientsClientIdResumeBody
from ...models.post_api_admin_clients_client_id_resume_response_200 import PostApiAdminClientsClientIdResumeResponse200
from ...types import UNSET, Response, Unset


def _get_kwargs(
    client_id: str,
    *,
    body: PostApiAdminClientsClientIdResumeBody | Unset = UNSET,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/admin/clients/{client_id}/resume".format(
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
) -> ErrorEnvelope | PostApiAdminClientsClientIdResumeResponse200 | None:
    if response.status_code == 200:
        response_200 = PostApiAdminClientsClientIdResumeResponse200.from_dict(response.json())

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
) -> Response[ErrorEnvelope | PostApiAdminClientsClientIdResumeResponse200]:
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
    body: PostApiAdminClientsClientIdResumeBody | Unset = UNSET,
) -> Response[ErrorEnvelope | PostApiAdminClientsClientIdResumeResponse200]:
    """Resume Application

     Lift a suspension on a self-registered application (open platform governance). Admin-managed clients
    (no owners row) cannot be suspended/resumed here.

    Args:
        client_id (str):
        body (PostApiAdminClientsClientIdResumeBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiAdminClientsClientIdResumeResponse200]
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
    body: PostApiAdminClientsClientIdResumeBody | Unset = UNSET,
) -> ErrorEnvelope | PostApiAdminClientsClientIdResumeResponse200 | None:
    """Resume Application

     Lift a suspension on a self-registered application (open platform governance). Admin-managed clients
    (no owners row) cannot be suspended/resumed here.

    Args:
        client_id (str):
        body (PostApiAdminClientsClientIdResumeBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiAdminClientsClientIdResumeResponse200
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
    body: PostApiAdminClientsClientIdResumeBody | Unset = UNSET,
) -> Response[ErrorEnvelope | PostApiAdminClientsClientIdResumeResponse200]:
    """Resume Application

     Lift a suspension on a self-registered application (open platform governance). Admin-managed clients
    (no owners row) cannot be suspended/resumed here.

    Args:
        client_id (str):
        body (PostApiAdminClientsClientIdResumeBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PostApiAdminClientsClientIdResumeResponse200]
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
    body: PostApiAdminClientsClientIdResumeBody | Unset = UNSET,
) -> ErrorEnvelope | PostApiAdminClientsClientIdResumeResponse200 | None:
    """Resume Application

     Lift a suspension on a self-registered application (open platform governance). Admin-managed clients
    (no owners row) cannot be suspended/resumed here.

    Args:
        client_id (str):
        body (PostApiAdminClientsClientIdResumeBody | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PostApiAdminClientsClientIdResumeResponse200
    """

    return (
        await asyncio_detailed(
            client_id=client_id,
            client=client,
            body=body,
        )
    ).parsed
