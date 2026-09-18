from http import HTTPStatus
from typing import Any

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.post_api_me_webauthn_register_begin_body import PostApiMeWebauthnRegisterBeginBody
from ...types import UNSET, Response, Unset


def _get_kwargs(
    *,
    body: PostApiMeWebauthnRegisterBeginBody | Unset = UNSET,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/me/webauthn/register/begin",
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
    *,
    client: AuthenticatedClient,
    body: PostApiMeWebauthnRegisterBeginBody | Unset = UNSET,
) -> Response[Any]:
    """WebAuthn Register Begin

     Start passkey registration (#142): ES256-only pubKeyCredParams, userVerification=required, user.id
    is the base64url of the internal user id bytes, excludeCredentials lists already-registered
    credentials, and the challenge is bound to the Bearer subject (no session cookie contract). Requires
    webauthn.rp_origins to be configured.

    Args:
        body (PostApiMeWebauthnRegisterBeginBody | Unset): Optional empty body (action-style
            operation).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any]
    """

    kwargs = _get_kwargs(
        body=body,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


async def asyncio_detailed(
    *,
    client: AuthenticatedClient,
    body: PostApiMeWebauthnRegisterBeginBody | Unset = UNSET,
) -> Response[Any]:
    """WebAuthn Register Begin

     Start passkey registration (#142): ES256-only pubKeyCredParams, userVerification=required, user.id
    is the base64url of the internal user id bytes, excludeCredentials lists already-registered
    credentials, and the challenge is bound to the Bearer subject (no session cookie contract). Requires
    webauthn.rp_origins to be configured.

    Args:
        body (PostApiMeWebauthnRegisterBeginBody | Unset): Optional empty body (action-style
            operation).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any]
    """

    kwargs = _get_kwargs(
        body=body,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)
