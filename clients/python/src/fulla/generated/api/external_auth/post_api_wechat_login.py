from http import HTTPStatus
from typing import Any, cast

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.post_api_wechat_login_body import PostApiWechatLoginBody
from ...models.social_login_token_response import SocialLoginTokenResponse
from ...types import UNSET, Response, Unset


def _get_kwargs(
    *,
    body: PostApiWechatLoginBody | Unset = UNSET,
    code: str,
) -> dict[str, Any]:
    headers: dict[str, Any] = {}

    params: dict[str, Any] = {}

    params["code"] = code

    params = {k: v for k, v in params.items() if v is not UNSET and v is not None}

    _kwargs: dict[str, Any] = {
        "method": "post",
        "url": "/api/wechat/login",
        "params": params,
    }

    if not isinstance(body, Unset):
        _kwargs["json"] = body.to_dict()

    headers["Content-Type"] = "application/json"

    _kwargs["headers"] = headers
    return _kwargs


def _parse_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Any | SocialLoginTokenResponse | None:
    if response.status_code == 200:
        response_200 = SocialLoginTokenResponse.from_dict(response.json())

        return response_200

    if response.status_code == 400:
        response_400 = cast(Any, None)
        return response_400

    if response.status_code == 403:
        response_403 = cast(Any, None)
        return response_403

    if response.status_code == 502:
        response_502 = cast(Any, None)
        return response_502

    if client.raise_on_unexpected_status:
        raise errors.UnexpectedStatus(response.status_code, response.content)
    else:
        return None


def _build_response(
    *, client: AuthenticatedClient | Client, response: httpx.Response
) -> Response[Any | SocialLoginTokenResponse]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    *,
    client: AuthenticatedClient | Client,
    body: PostApiWechatLoginBody | Unset = UNSET,
    code: str,
) -> Response[Any | SocialLoginTokenResponse]:
    """WeChat OAuth2 Login

     Exchange WeChat authorization code for user information. This endpoint handles the server-side
    OAuth2 flow with WeChat Open Platform.

    Args:
        code (str):
        body (PostApiWechatLoginBody | Unset): Optional empty body (action-style operation).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | SocialLoginTokenResponse]
    """

    kwargs = _get_kwargs(
        body=body,
        code=code,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    *,
    client: AuthenticatedClient | Client,
    body: PostApiWechatLoginBody | Unset = UNSET,
    code: str,
) -> Any | SocialLoginTokenResponse | None:
    """WeChat OAuth2 Login

     Exchange WeChat authorization code for user information. This endpoint handles the server-side
    OAuth2 flow with WeChat Open Platform.

    Args:
        code (str):
        body (PostApiWechatLoginBody | Unset): Optional empty body (action-style operation).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | SocialLoginTokenResponse
    """

    return sync_detailed(
        client=client,
        body=body,
        code=code,
    ).parsed


async def asyncio_detailed(
    *,
    client: AuthenticatedClient | Client,
    body: PostApiWechatLoginBody | Unset = UNSET,
    code: str,
) -> Response[Any | SocialLoginTokenResponse]:
    """WeChat OAuth2 Login

     Exchange WeChat authorization code for user information. This endpoint handles the server-side
    OAuth2 flow with WeChat Open Platform.

    Args:
        code (str):
        body (PostApiWechatLoginBody | Unset): Optional empty body (action-style operation).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | SocialLoginTokenResponse]
    """

    kwargs = _get_kwargs(
        body=body,
        code=code,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    *,
    client: AuthenticatedClient | Client,
    body: PostApiWechatLoginBody | Unset = UNSET,
    code: str,
) -> Any | SocialLoginTokenResponse | None:
    """WeChat OAuth2 Login

     Exchange WeChat authorization code for user information. This endpoint handles the server-side
    OAuth2 flow with WeChat Open Platform.

    Args:
        code (str):
        body (PostApiWechatLoginBody | Unset): Optional empty body (action-style operation).

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | SocialLoginTokenResponse
    """

    return (
        await asyncio_detailed(
            client=client,
            body=body,
            code=code,
        )
    ).parsed
