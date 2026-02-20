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
async def test_ue_transport_send_json_works_with_websockets_15_client_connection() -> None:
    received: list[dict[str, Any]] = []

    async def handler(conn: Any) -> None:
        async for raw in conn:
            received.append(json.loads(raw))
            await conn.send(json.dumps({"type": "pong"}))

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
            await transport.send_json({"type": "ping"})
            # Let the server receive the frame.
            await asyncio.sleep(0.05)
            assert received and received[0]["type"] == "ping"
        finally:
            await transport.stop()

