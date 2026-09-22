from http import HTTPStatus
from typing import Any, cast

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.get_oauth_2_consent_context_response_200 import GetOauth2ConsentContextResponse200
from ...types import UNSET, Response, Unset


def _get_kwargs(
    *,
    consent_csrf: str,
    client_id: str,
    redirect_uri: str,
    state: str | Unset = UNSET,
    user_id: str,
) -> dict[str, Any]:

    params: dict[str, Any] = {}

    params["consent_csrf"] = consent_csrf

    params["client_id"] = client_id

    params["redirect_uri"] = redirect_uri

    params["state"] = state

    params["user_id"] = user_id

    params = {k: v for k, v in params.items() if v is not UNSET and v is not None}

    _kwargs: dict[str, Any] = {
        "method": "get",
        "url": "/oauth2/consent/context",
        "params": params,
    }

    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Any | GetOauth2ConsentContextResponse200 | None:
    if response.status_code == 200:
        response_200 = GetOauth2ConsentContextResponse200.from_dict(response.json())

        return response_200

    if response.status_code == 400:
        response_400 = cast(Any, None)
        return response_400

    if response.status_code == 401:
        response_401 = cast(Any, None)
        return response_401

    if response.status_code == 403:
        response_403 = cast(Any, None)
        return response_403

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[Any | GetOauth2ConsentContextResponse200]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    *,
    client: AuthenticatedClient | Client,
    consent_csrf: str,
    client_id: str,
    redirect_uri: str,
    state: str | Unset = UNSET,
    user_id: str,
) -> Response[Any | GetOauth2ConsentContextResponse200]:
    """Consent screen context (owner attribution + org banner)

     Server-side consent-screen context (v1.5.0 M1b, #223 second half): the owner attribution label and
    the organization-membership banner for the consent flow whose server-minted consent_csrf nonce is
    presented. The nonce is validated but NOT consumed (the one-shot consume stays with POST
    /oauth2/consent). The (client_id, redirect_uri) pair must be registered, mirroring what reaching the
    consent screen via authorize already requires; the org block comes from the binding stashed server-
    side at authorize time (unknown or missing state, or a flow without an org context, yields
    org=null).

    Args:
        consent_csrf (str):
        client_id (str):
        redirect_uri (str):
        state (str | Unset):
        user_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | GetOauth2ConsentContextResponse200]
    """

    kwargs = _get_kwargs(
        consent_csrf=consent_csrf,
        client_id=client_id,
        redirect_uri=redirect_uri,
        state=state,
        user_id=user_id,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    *,
    client: AuthenticatedClient | Client,
    consent_csrf: str,
    client_id: str,
    redirect_uri: str,
    state: str | Unset = UNSET,
    user_id: str,
) -> Any | GetOauth2ConsentContextResponse200 | None:
    """Consent screen context (owner attribution + org banner)

     Server-side consent-screen context (v1.5.0 M1b, #223 second half): the owner attribution label and
    the organization-membership banner for the consent flow whose server-minted consent_csrf nonce is
    presented. The nonce is validated but NOT consumed (the one-shot consume stays with POST
    /oauth2/consent). The (client_id, redirect_uri) pair must be registered, mirroring what reaching the
    consent screen via authorize already requires; the org block comes from the binding stashed server-
    side at authorize time (unknown or missing state, or a flow without an org context, yields
    org=null).

    Args:
        consent_csrf (str):
        client_id (str):
        redirect_uri (str):
        state (str | Unset):
        user_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | GetOauth2ConsentContextResponse200
    """

    return sync_detailed(
        client=client,
        consent_csrf=consent_csrf,
        client_id=client_id,
        redirect_uri=redirect_uri,
        state=state,
        user_id=user_id,
    ).parsed


async def asyncio_detailed(
    *,
    client: AuthenticatedClient | Client,
    consent_csrf: str,
    client_id: str,
    redirect_uri: str,
    state: str | Unset = UNSET,
    user_id: str,
) -> Response[Any | GetOauth2ConsentContextResponse200]:
    """Consent screen context (owner attribution + org banner)

     Server-side consent-screen context (v1.5.0 M1b, #223 second half): the owner attribution label and
    the organization-membership banner for the consent flow whose server-minted consent_csrf nonce is
    presented. The nonce is validated but NOT consumed (the one-shot consume stays with POST
    /oauth2/consent). The (client_id, redirect_uri) pair must be registered, mirroring what reaching the
    consent screen via authorize already requires; the org block comes from the binding stashed server-
    side at authorize time (unknown or missing state, or a flow without an org context, yields
    org=null).

    Args:
        consent_csrf (str):
        client_id (str):
        redirect_uri (str):
        state (str | Unset):
        user_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | GetOauth2ConsentContextResponse200]
    """

    kwargs = _get_kwargs(
        consent_csrf=consent_csrf,
        client_id=client_id,
        redirect_uri=redirect_uri,
        state=state,
        user_id=user_id,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    *,
    client: AuthenticatedClient | Client,
    consent_csrf: str,
    client_id: str,
    redirect_uri: str,
    state: str | Unset = UNSET,
    user_id: str,
) -> Any | GetOauth2ConsentContextResponse200 | None:
    """Consent screen context (owner attribution + org banner)

     Server-side consent-screen context (v1.5.0 M1b, #223 second half): the owner attribution label and
    the organization-membership banner for the consent flow whose server-minted consent_csrf nonce is
    presented. The nonce is validated but NOT consumed (the one-shot consume stays with POST
    /oauth2/consent). The (client_id, redirect_uri) pair must be registered, mirroring what reaching the
    consent screen via authorize already requires; the org block comes from the binding stashed server-
    side at authorize time (unknown or missing state, or a flow without an org context, yields
    org=null).

    Args:
        consent_csrf (str):
        client_id (str):
        redirect_uri (str):
        state (str | Unset):
        user_id (str):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | GetOauth2ConsentContextResponse200
    """

    return (
        await asyncio_detailed(
            client=client,
            consent_csrf=consent_csrf,
            client_id=client_id,
            redirect_uri=redirect_uri,
            state=state,
            user_id=user_id,
        )
    ).parsed
