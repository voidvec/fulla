from http import HTTPStatus
from typing import Any

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.error_envelope import ErrorEnvelope
from ...models.patch_api_me_profile_body import PatchApiMeProfileBody
from ...models.patch_api_me_profile_response_200 import PatchApiMeProfileResponse200
from ...types import Response


def _get_kwargs(
    *,
    body: PatchApiMeProfileBody,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "patch",
        "url": "/api/me/profile",
    }

    _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> ErrorEnvelope | PatchApiMeProfileResponse200 | None:
    if response.status_code == 200:
        response_200 = PatchApiMeProfileResponse200.from_dict(response.json())

        return response_200

    if response.status_code == 400:
        response_400 = ErrorEnvelope.from_dict(response.json())

        return response_400

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
) -> Response[ErrorEnvelope | PatchApiMeProfileResponse200]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    *,
    client: AuthenticatedClient,
    body: PatchApiMeProfileBody,
) -> Response[ErrorEnvelope | PatchApiMeProfileResponse200]:
    """Update User Profile

     Update the current user's editable profile fields (v1.4.0 profile minimal set). Body keys are
    optional; an absent key leaves the field unchanged, an empty string clears it. display_name is
    trimmed and capped at 100 code points without control characters; avatar_url must be an https URL of
    at most 2048 chars (served verbatim, never fetched server-side).

    Args:
        body (PatchApiMeProfileBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PatchApiMeProfileResponse200]
    """

    kwargs = _get_kwargs(
        body=body,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    *,
    client: AuthenticatedClient,
    body: PatchApiMeProfileBody,
) -> ErrorEnvelope | PatchApiMeProfileResponse200 | None:
    """Update User Profile

     Update the current user's editable profile fields (v1.4.0 profile minimal set). Body keys are
    optional; an absent key leaves the field unchanged, an empty string clears it. display_name is
    trimmed and capped at 100 code points without control characters; avatar_url must be an https URL of
    at most 2048 chars (served verbatim, never fetched server-side).

    Args:
        body (PatchApiMeProfileBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PatchApiMeProfileResponse200
    """

    return sync_detailed(
        client=client,
        body=body,
    ).parsed


async def asyncio_detailed(
    *,
    client: AuthenticatedClient,
    body: PatchApiMeProfileBody,
) -> Response[ErrorEnvelope | PatchApiMeProfileResponse200]:
    """Update User Profile

     Update the current user's editable profile fields (v1.4.0 profile minimal set). Body keys are
    optional; an absent key leaves the field unchanged, an empty string clears it. display_name is
    trimmed and capped at 100 code points without control characters; avatar_url must be an https URL of
    at most 2048 chars (served verbatim, never fetched server-side).

    Args:
        body (PatchApiMeProfileBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[ErrorEnvelope | PatchApiMeProfileResponse200]
    """

    kwargs = _get_kwargs(
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    *,
    client: AuthenticatedClient,
    body: PatchApiMeProfileBody,
) -> ErrorEnvelope | PatchApiMeProfileResponse200 | None:
    """Update User Profile

     Update the current user's editable profile fields (v1.4.0 profile minimal set). Body keys are
    optional; an absent key leaves the field unchanged, an empty string clears it. display_name is
    trimmed and capped at 100 code points without control characters; avatar_url must be an https URL of
    at most 2048 chars (served verbatim, never fetched server-side).

    Args:
        body (PatchApiMeProfileBody):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        ErrorEnvelope | PatchApiMeProfileResponse200
    """

    return (
        await asyncio_detailed(
            client=client,
            body=body,
        )
    ).parsed
