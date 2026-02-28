from __future__ import annotations

from dataclasses import asdict
from typing import Any

import pytest

from mcp_server.event_router import EventRouter
from mcp_server.mcp_facade import ToolCallResult
from mcp_server.request_broker import RequestTimeoutError
from mcp_server.tool_catalog import ToolCatalog
from mcp_server.tool_passthrough import CatalogGuardError, MCPPassThroughService, UnknownToolError


class FakeFacade:
    def __init__(self, event_router: EventRouter | None = None) -> None:
        self.calls: list[dict[str, Any]] = []
        self.response_status = "ok"
        self.response_ok = True
        self._event_router = event_router
        self._pending_failures: list[Exception] = []
        self._pending_results: list[ToolCallResult] = []
        self.tools_list_schema_hash = "hash-001"
        self.tools_list_tools = [
            {
                "name": "system.health",
                "domain": "system",
                "version": "1.0.0",
                "enabled": True,
                "write": False,
                "params_schema": {"type": "object"},
                "result_schema": {"type": "object"},
            }
        ]

    def queue_failure(self, exc: Exception) -> None:
        self._pending_failures.append(exc)

    def queue_result(self, result: ToolCallResult) -> None:
        self._pending_results.append(result)

    async def call_tool(
        self,
        *,
        tool: str,
        params: dict[str, Any] | None = None,
        context: dict[str, Any] | None = None,
        timeout_ms: int | None = None,
        request_id: str | None = None,
        raise_on_error: bool = True,
    ) -> ToolCallResult:
        self.calls.append(
            {
                "tool": tool,
                "params": params,
                "context": context,
                "timeout_ms": timeout_ms,
                "request_id": request_id,
                "raise_on_error": raise_on_error,
            }
        )
        if tool == "tools.list":
            return ToolCallResult(
                ok=True,
                status="ok",
                request_id="req-tools-list",
                result={
                    "protocol_version": "unreal-mcp/1.0",
                    "schema_hash": self.tools_list_schema_hash,
                    "capabilities": ["core_tools_v1", "umg_widget_event_k2_v1"],
                    "tools": self.tools_list_tools,
                },
                diagnostics={},
                raw_envelope={},
            )

        if self._pending_failures:
            raise self._pending_failures.pop(0)
        if self._pending_results:
            return self._pending_results.pop(0)

        if self._event_router is not None and request_id:
            self._event_router.publish(
                {
                    "event_id": "evt-progress",
                    "event_type": "event.progress",
                    "request_id": request_id,
                    "timestamp_ms": 1000,
                    "payload": {"percent": 50, "phase": "testing"},
                }
            )
            self._event_router.publish(
                {
                    "event_id": "evt-log",
                    "event_type": "event.log",
                    "request_id": request_id,
                    "timestamp_ms": 1001,
                    "payload": {"level": "info", "message": "hello"},
                }
            )

        return ToolCallResult(
            ok=self.response_ok,
            status=self.response_status,
            request_id=request_id or "req-call",
            result={"echo": params or {}},
            diagnostics={"errors": []},
            raw_envelope={"status": self.response_status},
        )


@pytest.mark.asyncio
async def test_tool_catalog_refresh_from_tools_list() -> None:
    facade = FakeFacade()
    catalog = ToolCatalog()

    await catalog.refresh(facade, include_schemas=True)  # type: ignore[arg-type]

    assert catalog.protocol_version == "unreal-mcp/1.0"
    assert catalog.schema_hash == "hash-001"
    assert catalog.capabilities == ("core_tools_v1", "umg_widget_event_k2_v1")
    assert catalog.get_tool("system.health") is not None


@pytest.mark.asyncio
async def test_pass_through_calls_tool_and_preserves_error_status() -> None:
    facade = FakeFacade()
    facade.response_status = "error"
    facade.response_ok = False

    service = MCPPassThroughService(
        facade=facade,  # type: ignore[arg-type]
        catalog=ToolCatalog(),
        include_schemas=True,
        refresh_interval_s=0,
    )
    await service.start()
    try:
        assert service.has_capability("core_tools_v1")
        result = await service.call_tool(
            tool="system.health",
            params={"a": 1},
            context={"timeout_ms": 1000},
            timeout_ms=900,
        )
    finally:
        await service.stop()

    assert result.status == "error"
    assert result.ok is False
    assert result.result["echo"] == {"a": 1}
    last_call = facade.calls[-1]
    assert last_call["tool"] == "system.health"
    assert last_call["raise_on_error"] is False


@pytest.mark.asyncio
async def test_pass_through_raises_unknown_tool() -> None:
    facade = FakeFacade()
    service = MCPPassThroughService(
        facade=facade,  # type: ignore[arg-type]
        catalog=ToolCatalog(),
        include_schemas=True,
        refresh_interval_s=0,
    )
    await service.start()
    try:
        with pytest.raises(UnknownToolError):
            await service.call_tool(tool="mat.instance.params.get")
    finally:
        await service.stop()

    tool_list_calls = [c for c in facade.calls if c["tool"] == "tools.list"]
    assert len(tool_list_calls) >= 2
    tools_dump = service.list_tools_as_dict()
    assert tools_dump
    assert asdict(service.list_tools()[0]) == tools_dump[0]


@pytest.mark.asyncio
async def test_call_tool_stream_emits_normalized_events() -> None:
    event_router = EventRouter()
    facade = FakeFacade(event_router=event_router)
    service = MCPPassThroughService(
        facade=facade,  # type: ignore[arg-type]
        catalog=ToolCatalog(),
        event_router=event_router,
        include_schemas=True,
        refresh_interval_s=0,
    )
    await service.start()

    events: list[dict[str, Any]] = []
    try:
        result = await service.call_tool_stream(
            tool="system.health",
            params={"k": "v"},
            on_event=events.append,
        )
    finally:
        await service.stop()

    assert result.status == "ok"
    assert len(events) == 2
    assert events[0]["notification_kind"] == "progress"
    assert events[0]["phase"] == "testing"
    assert events[1]["notification_kind"] == "log"
    assert events[1]["message"] == "hello"


@pytest.mark.asyncio
async def test_call_tool_retries_transient_exception() -> None:
    facade = FakeFacade()
    facade.queue_failure(RequestTimeoutError("timeout"))
    service = MCPPassThroughService(
        facade=facade,  # type: ignore[arg-type]
        catalog=ToolCatalog(),
        include_schemas=True,
        refresh_interval_s=0,
        transient_max_attempts=2,
        retry_backoff_initial_s=0.001,
        retry_backoff_max_s=0.001,
    )
    await service.start()
    try:
        result = await service.call_tool(tool="system.health", params={"x": 1})
    finally:
        await service.stop()

    assert result.status == "ok"
    tool_calls = [call for call in facade.calls if call["tool"] == "system.health"]
    assert len(tool_calls) == 2


@pytest.mark.asyncio
async def test_call_tool_retries_retryable_tool_result() -> None:
    facade = FakeFacade()
    facade.queue_result(
        ToolCallResult(
            ok=False,
            status="error",
            request_id="req-retryable",
            result={},
            diagnostics={
                "errors": [
                    {
                        "code": "MCP.SERVER.TRANSIENT_FAILURE",
                        "message": "temporary network blip",
                        "retriable": True,
                    }
                ]
            },
            raw_envelope={"status": "error"},
        )
    )

    service = MCPPassThroughService(
        facade=facade,  # type: ignore[arg-type]
        catalog=ToolCatalog(),
        include_schemas=True,
        refresh_interval_s=0,
        transient_max_attempts=2,
        retry_backoff_initial_s=0.001,
        retry_backoff_max_s=0.001,
    )
    await service.start()
    try:
        result = await service.call_tool(tool="system.health", params={"x": 1})
    finally:
        await service.stop()

    assert result.status == "ok"
    tool_calls = [call for call in facade.calls if call["tool"] == "system.health"]
    assert len(tool_calls) == 2


@pytest.mark.asyncio
async def test_catalog_guard_required_tools_missing() -> None:
    facade = FakeFacade()
    service = MCPPassThroughService(
        facade=facade,  # type: ignore[arg-type]
        catalog=ToolCatalog(),
        include_schemas=True,
        refresh_interval_s=0,
        required_tools=("asset.create",),
    )

    with pytest.raises(CatalogGuardError):
        await service.start()


@pytest.mark.asyncio
async def test_catalog_guard_pin_schema_hash_mismatch() -> None:
    facade = FakeFacade()
    service = MCPPassThroughService(
        facade=facade,  # type: ignore[arg-type]
        catalog=ToolCatalog(),
        include_schemas=True,
        refresh_interval_s=0,
        pin_schema_hash="HASH-XYZ",
    )

    with pytest.raises(CatalogGuardError):
        await service.start()


@pytest.mark.asyncio
async def test_catalog_guard_fail_on_schema_change() -> None:
    facade = FakeFacade()
    service = MCPPassThroughService(
        facade=facade,  # type: ignore[arg-type]
        catalog=ToolCatalog(),
        include_schemas=True,
        refresh_interval_s=0,
        fail_on_schema_change=True,
    )
    await service.start()
    try:
        facade.tools_list_schema_hash = "hash-002"
        with pytest.raises(CatalogGuardError):
            await service.refresh_catalog()
    finally:
        await service.stop()
