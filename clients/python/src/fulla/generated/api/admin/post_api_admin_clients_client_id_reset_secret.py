from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.post_api_admin_clients_client_id_reset_secret_body import PostApiAdminClientsClientIdResetSecretBody
from ...types import UNSET, Response, Unset


def _get_kwargs(
    client_id: str,
    *,
    body: PostApiAdminClientsClientIdResetSecretBody | Unset = UNSET,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/admin/clients/{client_id}/reset-secret".format(
            client_id=quote(str(client_id), safe=""),
        ),
    }

    if not isinstance(body, Unset):
        _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> Any | None:
    if response.status_code == 200:
        return None

    if response.status_code == 401:
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
    client_id: str,
    *,
    client: AuthenticatedClient,
    body: PostApiAdminClientsClientIdResetSecretBody | Unset = UNSET,
) -> Response[Any]:
    """Reset Client Secret

     Reset the secret of a specific OAuth2 client.

    Args:
        client_id (str):
        body (PostApiAdminClientsClientIdResetSecretBody | Unset): Optional empty body (action-
            style operation).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any]
    """

    kwargs = _get_kwargs(
        client_id=client_id,
        body=body,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


async def asyncio_detailed(
    client_id: str,
    *,
    client: AuthenticatedClient,
    body: PostApiAdminClientsClientIdResetSecretBody | Unset = UNSET,
) -> Response[Any]:
    """Reset Client Secret

     Reset the secret of a specific OAuth2 client.

    Args:
        client_id (str):
        body (PostApiAdminClientsClientIdResetSecretBody | Unset): Optional empty body (action-
            style operation).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any]
    """

    kwargs = _get_kwargs(
        client_id=client_id,
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)
