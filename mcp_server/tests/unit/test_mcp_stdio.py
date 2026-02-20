from __future__ import annotations

import io
import json
from typing import Any

import pytest

from mcp_server.mcp_facade import ToolCallResult
from mcp_server.mcp_stdio import (
    JSONRPC_INVALID_PARAMS,
    JSONRPC_SERVER_NOT_INITIALIZED,
    MCPRequestDispatcher,
    read_framed_message,
    read_stdio_message,
    write_jsonl_message,
    write_framed_message,
    write_stdio_message,
)
from mcp_server.tool_catalog import ToolDefinition
from mcp_server.tool_passthrough import UnknownToolError


class FakePassThrough:
    def __init__(self) -> None:
        self.call_requests: list[dict[str, Any]] = []
        self._tools = [
            ToolDefinition(
                name="system.health",
                domain="system",
                version="1.0.0",
                enabled=True,
                write=False,
                params_schema={"type": "object"},
                result_schema={"type": "object"},
            )
        ]

    def list_tools(self) -> list[ToolDefinition]:
        return self._tools

    async def call_tool(
        self,
        *,
        tool: str,
        params: dict[str, Any] | None = None,
        context: dict[str, Any] | None = None,
        timeout_ms: int | None = None,
        request_id: str | None = None,
        allow_retry: bool = True,
    ) -> ToolCallResult:
        self.call_requests.append(
            {
                "tool": tool,
                "params": params,
                "request_id": request_id,
            }
        )

        if tool == "missing.tool":
            raise UnknownToolError("Unknown tool: missing.tool")
        if tool == "broken.tool":
            raise ConnectionError("transport down")

        return ToolCallResult(
            ok=True,
            status="ok",
            request_id=request_id or "req-1",
            result={"echo": params or {}},
            diagnostics={"errors": []},
            raw_envelope={},
        )


def test_framed_message_roundtrip() -> None:
    stream = io.BytesIO()
    message = {
        "jsonrpc": "2.0",
        "id": 1,
        "result": {"ok": True},
    }

    write_framed_message(stream, message)
    stream.seek(0)

    decoded = read_framed_message(stream)
    assert decoded == message


def test_stdio_message_json_line_roundtrip() -> None:
    stream = io.BytesIO()
    stream.write(b'{"jsonrpc":"2.0","id":1,"method":"ping"}\n')
    stream.seek(0)

    decoded, mode = read_stdio_message(stream)
    assert mode == "jsonl"
    assert decoded == {"jsonrpc": "2.0", "id": 1, "method": "ping"}


def test_write_stdio_message_json_line_mode() -> None:
    stream = io.BytesIO()
    payload = {"jsonrpc": "2.0", "id": 1, "result": {"ok": True}}

    write_stdio_message(stream, payload, "jsonl")
    stream.seek(0)
    json_line = stream.readline()
    assert json.loads(json_line.decode("utf-8")) == payload


def test_write_jsonl_message_roundtrip() -> None:
    stream = io.BytesIO()
    payload = {"jsonrpc": "2.0", "id": 7, "method": "initialize"}

    write_jsonl_message(stream, payload)
    stream.seek(0)
    decoded, mode = read_stdio_message(stream)

    assert mode == "jsonl"
    assert decoded == payload


@pytest.mark.asyncio
async def test_dispatcher_requires_initialize_before_tools() -> None:
    dispatcher = MCPRequestDispatcher(FakePassThrough())  # type: ignore[arg-type]

    response = await dispatcher.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 11,
            "method": "tools/list",
            "params": {},
        }
    )

    assert response["error"]["code"] == JSONRPC_SERVER_NOT_INITIALIZED


@pytest.mark.asyncio
async def test_initialize_then_tools_list() -> None:
    dispatcher = MCPRequestDispatcher(FakePassThrough())  # type: ignore[arg-type]

    init_response = await dispatcher.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-03-26",
                "capabilities": {},
                "clientInfo": {"name": "pytest", "version": "1.0.0"},
            },
        }
    )
    assert init_response["result"]["protocolVersion"] == "2025-03-26"
    assert "tools" in init_response["result"]["capabilities"]

    list_response = await dispatcher.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "tools/list",
            "params": {},
        }
    )

    tools = list_response["result"]["tools"]
    assert len(tools) == 1
    assert tools[0]["name"] == "system.health"
    assert tools[0]["annotations"]["readOnlyHint"] is True


@pytest.mark.asyncio
async def test_tools_call_success_and_error_payloads() -> None:
    fake = FakePassThrough()
    dispatcher = MCPRequestDispatcher(fake)  # type: ignore[arg-type]

    await dispatcher.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"protocolVersion": "2025-03-26"},
        }
    )

    ok_response = await dispatcher.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {
                "name": "system.health",
                "arguments": {"hello": "world"},
            },
        }
    )
    assert ok_response["result"]["isError"] is False
    assert ok_response["result"]["structuredContent"]["status"] == "ok"
    assert fake.call_requests[-1]["request_id"] == "mcp-3"

    missing_response = await dispatcher.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 4,
            "method": "tools/call",
            "params": {
                "name": "missing.tool",
                "arguments": {},
            },
        }
    )
    assert missing_response["result"]["isError"] is True
    assert (
        missing_response["result"]["structuredContent"]["diagnostics"]["errors"][0]["code"]
        == "MCP.SERVER.TOOL_NOT_FOUND"
    )

    broken_response = await dispatcher.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 5,
            "method": "tools/call",
            "params": {
                "name": "broken.tool",
                "arguments": {},
            },
        }
    )
    assert broken_response["result"]["isError"] is True
    assert (
        broken_response["result"]["structuredContent"]["diagnostics"]["errors"][0]["retriable"]
        is True
    )


@pytest.mark.asyncio
async def test_tools_call_rejects_non_object_arguments() -> None:
    dispatcher = MCPRequestDispatcher(FakePassThrough())  # type: ignore[arg-type]
    await dispatcher.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"protocolVersion": "2025-03-26"},
        }
    )

    response = await dispatcher.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "tools/call",
            "params": {
                "name": "system.health",
                "arguments": [1, 2, 3],
            },
        }
    )

    assert response["error"]["code"] == JSONRPC_INVALID_PARAMS
