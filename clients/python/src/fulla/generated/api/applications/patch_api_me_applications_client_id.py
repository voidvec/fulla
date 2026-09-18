from http import HTTPStatus
from typing import Any
from urllib.parse import quote

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.patch_api_me_applications_client_id_body import PatchApiMeApplicationsClientIdBody
from ...models.patch_api_me_applications_client_id_response_200 import PatchApiMeApplicationsClientIdResponse200
from ...types import Response


def _get_kwargs(
    client_id: str,
    *,
    body: PatchApiMeApplicationsClientIdBody,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "patch",
        "url": "/api/me/applications/{client_id}".format(
            client_id=quote(str(client_id), safe=""),
        ),
    }

    _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> ErrorEnvelope | PatchApiMeApplicationsClientIdResponse200 | None:
    if response.status_code == 200:
        response_200 = PatchApiMeApplicationsClientIdResponse200.from_dict(response.json())

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

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[ErrorEnvelope | PatchApiMeApplicationsClientIdResponse200]:
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
    body: PatchApiMeApplicationsClientIdBody,
) -> Response[ErrorEnvelope | PatchApiMeApplicationsClientIdResponse200]:
    """Update Application

     Update name / redirect_uris / allowed_grant_types / scopes of a self-registered application
    (personal apps are managed by the creator, org apps by org owner/admin members). Absent keys are
    left unchanged.

    Args:
        client_id (str):
        body (PatchApiMeApplicationsClientIdBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PatchApiMeApplicationsClientIdResponse200]
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
    body: PatchApiMeApplicationsClientIdBody,
) -> ErrorEnvelope | PatchApiMeApplicationsClientIdResponse200 | None:
    """Update Application

     Update name / redirect_uris / allowed_grant_types / scopes of a self-registered application
    (personal apps are managed by the creator, org apps by org owner/admin members). Absent keys are
    left unchanged.

    Args:
        client_id (str):
        body (PatchApiMeApplicationsClientIdBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PatchApiMeApplicationsClientIdResponse200
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
    body: PatchApiMeApplicationsClientIdBody,
) -> Response[ErrorEnvelope | PatchApiMeApplicationsClientIdResponse200]:
    """Update Application

     Update name / redirect_uris / allowed_grant_types / scopes of a self-registered application
    (personal apps are managed by the creator, org apps by org owner/admin members). Absent keys are
    left unchanged.

    Args:
        client_id (str):
        body (PatchApiMeApplicationsClientIdBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PatchApiMeApplicationsClientIdResponse200]
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
    body: PatchApiMeApplicationsClientIdBody,
) -> ErrorEnvelope | PatchApiMeApplicationsClientIdResponse200 | None:
    """Update Application

     Update name / redirect_uris / allowed_grant_types / scopes of a self-registered application
    (personal apps are managed by the creator, org apps by org owner/admin members). Absent keys are
    left unchanged.

    Args:
        client_id (str):
        body (PatchApiMeApplicationsClientIdBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PatchApiMeApplicationsClientIdResponse200
    """

    return (
        await asyncio_detailed(
            client_id=client_id,
            client=client,
            body=body,
        )
    ).parsed
