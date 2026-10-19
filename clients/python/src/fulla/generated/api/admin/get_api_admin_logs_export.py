from http import HTTPStatus
from typing import Any, cast

import httpx

from ... import errors
from ...client import AuthenticatedClient, Client
from ...models.get_api_admin_logs_export_all import GetApiAdminLogsExportAll
from ...types import UNSET, Response, Unset


def _get_kwargs(
    *,
    action: str | Unset = UNSET,
    outcome: str | Unset = UNSET,
    actor_id: str | Unset = UNSET,
    from_: int | Unset = UNSET,
    to: int | Unset = UNSET,
    all_: GetApiAdminLogsExportAll | Unset = UNSET,
    limit: int | Unset = UNSET,
) -> dict[str, Any]:

    params: dict[str, Any] = {}

    params["action"] = action

    params["outcome"] = outcome

    params["actor_id"] = actor_id

    params["from"] = from_

    params["to"] = to

    json_all_: str | Unset = UNSET
    if not isinstance(all_, Unset):
        json_all_ = all_.value

    params["all"] = json_all_

    params["limit"] = limit

    params = {k: v for k, v in params.items() if v is not UNSET and v is not None}

    _kwargs: dict[str, Any] = {
        "method": "get",
        "url": "/api/admin/logs/export",
        "params": params,
    }

    return _kwargs


def _parse_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> Any | str | None:
    if response.status_code == 200:
        response_200 = response.text
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


def _build_response(*, client: AuthenticatedClient | Client, response: httpx.Response) -> Response[Any | str]:
    return Response(
        status_code=HTTPStatus(response.status_code),
        content=response.content,
        headers=response.headers,
        parsed=_parse_response(client=client, response=response),
    )


def sync_detailed(
    *,
    client: AuthenticatedClient,
    action: str | Unset = UNSET,
    outcome: str | Unset = UNSET,
    actor_id: str | Unset = UNSET,
    from_: int | Unset = UNSET,
    to: int | Unset = UNSET,
    all_: GetApiAdminLogsExportAll | Unset = UNSET,
    limit: int | Unset = UNSET,
) -> Response[Any | str]:
    """Export Audit Logs (CSV)

     P1-6 (#283): CSV export of the audit trail. Same filters as the list endpoint
    (action/outcome/actor_id) plus a mandatory time window (from/to, epoch seconds) unless an explicit
    all=true is given; row cap (audit.export_max_rows, default 500k; the per-request `limit` may only
    lower it) — over-cap answers 400 AUDIT_EXPORT_TOO_LARGE. Cells are formula-injection hardened
    (leading = + - @ tab CR get a '' prefix). Audited as audit_exported. Memory deployments answer 404.

    Args:
        action (str | Unset):
        outcome (str | Unset):
        actor_id (str | Unset):
        from_ (int | Unset):
        to (int | Unset):
        all_ (GetApiAdminLogsExportAll | Unset):
        limit (int | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | str]
    """

    kwargs = _get_kwargs(
        action=action,
        outcome=outcome,
        actor_id=actor_id,
        from_=from_,
        to=to,
        all_=all_,
        limit=limit,
    )

    response = client.get_httpx_client().request(
        **kwargs,
    )

    return _build_response(client=client, response=response)


def sync(
    *,
    client: AuthenticatedClient,
    action: str | Unset = UNSET,
    outcome: str | Unset = UNSET,
    actor_id: str | Unset = UNSET,
    from_: int | Unset = UNSET,
    to: int | Unset = UNSET,
    all_: GetApiAdminLogsExportAll | Unset = UNSET,
    limit: int | Unset = UNSET,
) -> Any | str | None:
    """Export Audit Logs (CSV)

     P1-6 (#283): CSV export of the audit trail. Same filters as the list endpoint
    (action/outcome/actor_id) plus a mandatory time window (from/to, epoch seconds) unless an explicit
    all=true is given; row cap (audit.export_max_rows, default 500k; the per-request `limit` may only
    lower it) — over-cap answers 400 AUDIT_EXPORT_TOO_LARGE. Cells are formula-injection hardened
    (leading = + - @ tab CR get a '' prefix). Audited as audit_exported. Memory deployments answer 404.

    Args:
        action (str | Unset):
        outcome (str | Unset):
        actor_id (str | Unset):
        from_ (int | Unset):
        to (int | Unset):
        all_ (GetApiAdminLogsExportAll | Unset):
        limit (int | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | str
    """

    return sync_detailed(
        client=client,
        action=action,
        outcome=outcome,
        actor_id=actor_id,
        from_=from_,
        to=to,
        all_=all_,
        limit=limit,
    ).parsed


async def asyncio_detailed(
    *,
    client: AuthenticatedClient,
    action: str | Unset = UNSET,
    outcome: str | Unset = UNSET,
    actor_id: str | Unset = UNSET,
    from_: int | Unset = UNSET,
    to: int | Unset = UNSET,
    all_: GetApiAdminLogsExportAll | Unset = UNSET,
    limit: int | Unset = UNSET,
) -> Response[Any | str]:
    """Export Audit Logs (CSV)

     P1-6 (#283): CSV export of the audit trail. Same filters as the list endpoint
    (action/outcome/actor_id) plus a mandatory time window (from/to, epoch seconds) unless an explicit
    all=true is given; row cap (audit.export_max_rows, default 500k; the per-request `limit` may only
    lower it) — over-cap answers 400 AUDIT_EXPORT_TOO_LARGE. Cells are formula-injection hardened
    (leading = + - @ tab CR get a '' prefix). Audited as audit_exported. Memory deployments answer 404.

    Args:
        action (str | Unset):
        outcome (str | Unset):
        actor_id (str | Unset):
        from_ (int | Unset):
        to (int | Unset):
        all_ (GetApiAdminLogsExportAll | Unset):
        limit (int | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Response[Any | str]
    """

    kwargs = _get_kwargs(
        action=action,
        outcome=outcome,
        actor_id=actor_id,
        from_=from_,
        to=to,
        all_=all_,
        limit=limit,
    )

    response = await client.get_async_httpx_client().request(**kwargs)

    return _build_response(client=client, response=response)


async def asyncio(
    *,
    client: AuthenticatedClient,
    action: str | Unset = UNSET,
    outcome: str | Unset = UNSET,
    actor_id: str | Unset = UNSET,
    from_: int | Unset = UNSET,
    to: int | Unset = UNSET,
    all_: GetApiAdminLogsExportAll | Unset = UNSET,
    limit: int | Unset = UNSET,
) -> Any | str | None:
    """Export Audit Logs (CSV)

     P1-6 (#283): CSV export of the audit trail. Same filters as the list endpoint
    (action/outcome/actor_id) plus a mandatory time window (from/to, epoch seconds) unless an explicit
    all=true is given; row cap (audit.export_max_rows, default 500k; the per-request `limit` may only
    lower it) — over-cap answers 400 AUDIT_EXPORT_TOO_LARGE. Cells are formula-injection hardened
    (leading = + - @ tab CR get a '' prefix). Audited as audit_exported. Memory deployments answer 404.

    Args:
        action (str | Unset):
        outcome (str | Unset):
        actor_id (str | Unset):
        from_ (int | Unset):
        to (int | Unset):
        all_ (GetApiAdminLogsExportAll | Unset):
        limit (int | Unset):

    Raises:
        errors.UnexpectedStatus: If the server returns an undocumented status code and Client.raise_on_unexpected_status is True.
        httpx.TimeoutException: If the request takes longer than Client.timeout.

    Returns:
        Any | str
    """

    return (
        await asyncio_detailed(
            client=client,
            action=action,
            outcome=outcome,
            actor_id=actor_id,
            from_=from_,
            to=to,
            all_=all_,
            limit=limit,
        )
    ).parsed
