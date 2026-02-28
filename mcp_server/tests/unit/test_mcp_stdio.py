from __future__ import annotations

import io
import json
from pathlib import Path
from typing import Any

import pytest

from mcp_server.config import AppConfig, UeConfig
from mcp_server.mcp_facade import ToolCallResult
from mcp_server.mcp_stdio import (
    JSONRPC_INVALID_PARAMS,
    JSONRPC_SERVER_NOT_INITIALIZED,
    MCPRequestDispatcher,
    _build_endpoint_listing_payload,
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
        self.capabilities: tuple[str, ...] = ()
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

    def has_capability(self, capability: str) -> bool:
        return capability in self.capabilities

    def add_tool(self, tool: ToolDefinition) -> None:
        self._tools.append(tool)

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
    tool_names = {tool["name"] for tool in tools}
    assert "system.health" in tool_names
    assert "umg.workflow.compose" in tool_names


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


@pytest.mark.asyncio
async def test_tools_call_virtual_umg_workflow_compose() -> None:
    fake = FakePassThrough()
    fake.add_tool(
        ToolDefinition(
            name="umg.widget.patch",
            domain="umg",
            version="1.0.0",
            enabled=True,
            write=True,
            params_schema={"type": "object"},
            result_schema={"type": "object"},
        )
    )

    dispatcher = MCPRequestDispatcher(fake)  # type: ignore[arg-type]
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
            "id": 12,
            "method": "tools/call",
            "params": {
                "name": "umg.workflow.compose",
                "arguments": {
                    "object_path": "/Game/UI/WBP_Test.WBP_Test",
                    "actions": [
                        {
                            "kind": "widget.patch",
                            "args": {
                                "widget_ref": {"name": "RootCanvas"},
                                "patch": [{"op": "replace", "path": "/RenderOpacity", "value": 0.5}],
                            },
                        }
                    ],
                },
            },
        }
    )

    assert response["result"]["isError"] is False
    assert response["result"]["structuredContent"]["status"] == "ok"
    assert fake.call_requests[-1]["tool"] == "umg.widget.patch"


def test_build_endpoint_listing_payload_includes_selector_hint(tmp_path: Path) -> None:
    project_root = tmp_path / "ProjectHints"
    project_root.mkdir(parents=True, exist_ok=True)
    (project_root / "ProjectHints.uproject").write_text("{}", encoding="utf-8")

    instances_dir = project_root / "Saved" / "UnrealMCP" / "instances"
    instances_dir.mkdir(parents=True, exist_ok=True)
    (instances_dir / "instance-a.json").write_text(
        json.dumps(
            {
                "instance_id": "instance-a",
                "ws_url": "ws://127.0.0.1:19090",
                "project_dir": str(project_root),
                "process_id": 4321,
                "heartbeat_at_ms": 9_999_999_999_999,
                "updated_at_ms": 9_999_999_999_999,
            }
        ),
        encoding="utf-8",
    )

    payload = _build_endpoint_listing_payload(
        AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:18080")),
        endpoint_selector=None,
        env={"UE_MCP_PROJECT_ROOT": str(project_root)},
        cwd=tmp_path,
    )

    assert payload["candidate_count"] == 1
    assert payload["resolved"]["ws_url"] == "ws://127.0.0.1:19090"
    first_candidate = payload["candidates"][0]
    assert first_candidate["selector_hint"]["env"]["UE_MCP_INSTANCE_ID"] == "instance-a"
    assert first_candidate["selector_hint"]["env"]["UE_MCP_PROCESS_ID"] == 4321
