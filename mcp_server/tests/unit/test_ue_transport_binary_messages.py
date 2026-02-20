from __future__ import annotations

import asyncio
import json
from typing import Any

import pytest
import websockets

from mcp_server.event_router import EventRouter
from mcp_server.request_broker import RequestBroker
from mcp_server.ue_transport import UeWsTransport


@pytest.mark.asyncio
async def test_ue_transport_handles_binary_json_messages() -> None:
    async def handler(conn: Any) -> None:
        # Send handshake as binary payload (UE plugin style).
        await conn.send(
            json.dumps({"type": "mcp.transport.connected", "connection_id": 1}).encode("utf-8")
        )

        async for raw in conn:
            if isinstance(raw, (bytes, bytearray)):
                raw = raw.decode("utf-8")
            msg = json.loads(raw)
            if msg.get("type") == "mcp.request":
                request = msg.get("request") or {}
                request_id = request.get("request_id", "req-unknown")
                envelope = {
                    "request_id": request_id,
                    "status": "ok",
                    "result": {"ok": True},
                    "diagnostics": {"errors": [], "warnings": [], "infos": []},
                }
                response = {
                    "type": "mcp.response",
                    "ok": True,
                    "response_json": json.dumps(envelope, separators=(",", ":")),
                }
                await conn.send(json.dumps(response).encode("utf-8"))

    async with websockets.serve(handler, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        transport = UeWsTransport(
            ws_url=f"ws://127.0.0.1:{port}",
            request_broker=RequestBroker(default_timeout_ms=1000),
            event_router=EventRouter(),
            connect_timeout_s=1.0,
            ping_interval_s=1000.0,
        )

        await transport.start()
        try:
            await transport.wait_until_connected(timeout_s=1.0)
            response = await transport.request(tool="system.health", params={})
            assert response.request_id
            assert response.ok is True
            assert response.envelope["status"] == "ok"
        finally:
            await transport.stop()

