from __future__ import annotations

import asyncio
import json

import pytest

from mcp_server.request_broker import RequestBroker, RequestTimeoutError


@pytest.mark.asyncio
async def test_send_request_and_resolve_response() -> None:
    broker = RequestBroker(default_timeout_ms=1_000)

    async def send_json(message: dict) -> None:
        request_id = message["request"]["request_id"]
        response_envelope = {
            "request_id": request_id,
            "status": "ok",
            "result": {"value": 1},
            "diagnostics": {"errors": [], "warnings": [], "infos": []},
        }
        response_message = {
            "type": "mcp.response",
            "ok": True,
            "response_json": json.dumps(response_envelope),
        }

        async def resolve_later() -> None:
            await asyncio.sleep(0)
            await broker.resolve_from_message(response_message)

        asyncio.create_task(resolve_later())

    response = await broker.send_request(
        send_json=send_json,
        tool="system.health",
        params={},
    )

    assert response.request_id.startswith("req-")
    assert response.status == "ok"
    assert response.ok is True
    assert response.envelope["result"]["value"] == 1


@pytest.mark.asyncio
async def test_send_request_timeout() -> None:
    broker = RequestBroker(default_timeout_ms=30)

    async def send_json(_: dict) -> None:
        return None

    with pytest.raises(RequestTimeoutError):
        await broker.send_request(
            send_json=send_json,
            tool="system.health",
            params={},
            timeout_ms=20,
        )
