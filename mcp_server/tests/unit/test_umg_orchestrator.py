from __future__ import annotations

from typing import Any

import pytest

from mcp_server.mcp_facade import ToolCallResult
from mcp_server.tool_catalog import ToolDefinition
from mcp_server.umg_orchestrator import UMGOrchestrationService


class FakePassThrough:
    def __init__(self, tool_names: list[str], *, capabilities: tuple[str, ...] = ()) -> None:
        self.calls: list[dict[str, Any]] = []
        self.capabilities = capabilities
        self._tools = [
            ToolDefinition(
                name=tool_name,
                domain="umg",
                version="1.0.0",
                enabled=True,
                write=True,
                params_schema={"type": "object"},
                result_schema={"type": "object"},
            )
            for tool_name in tool_names
        ]

    def list_tools(self) -> list[ToolDefinition]:
        return self._tools

    def has_capability(self, capability: str) -> bool:
        return capability in self.capabilities

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
        self.calls.append(
            {
                "tool": tool,
                "params": params or {},
                "request_id": request_id,
                "context": context,
                "timeout_ms": timeout_ms,
                "allow_retry": allow_retry,
            }
        )
        return ToolCallResult(
            ok=True,
            status="ok",
            request_id=request_id or f"req-{len(self.calls)}",
            result={"touched_packages": [f"/Game/Touched/{len(self.calls)}"]},
            diagnostics={"errors": [], "warnings": [], "infos": []},
            raw_envelope={},
        )


@pytest.mark.asyncio
async def test_compose_prefers_v2_and_applies_compile_save_flags() -> None:
    fake = FakePassThrough(
        [
            "umg.widget.patch",
            "umg.widget.patch.v2",
            "umg.widget.event.bind",
        ],
        capabilities=("umg_widget_event_k2_v1",),
    )
    service = UMGOrchestrationService(fake)  # type: ignore[arg-type]

    result = await service.call_virtual_tool(
        tool_name="umg.workflow.compose",
        request_id="mcp-11",
        arguments={
            "object_path": "/Game/UI/WBP_Test.WBP_Test",
            "auto_save": True,
            "compile_on_finish": True,
            "actions": [
                {
                    "kind": "widget.patch",
                    "args": {
                        "widget_ref": {"name": "RootCanvas"},
                        "patch": [{"op": "replace", "path": "/RenderOpacity", "value": 0.5}],
                    },
                },
                {
                    "kind": "widget.event.bind",
                    "args": {
                        "widget_ref": {"name": "RuntimeButton"},
                        "event_name": "OnClicked",
                        "function_name": "HandleRuntimeButtonClicked",
                    },
                },
            ],
        },
    )

    assert result.ok is True
    assert result.status == "ok"
    assert len(fake.calls) == 2
    assert fake.calls[0]["tool"] == "umg.widget.patch.v2"
    assert fake.calls[0]["params"]["compile_on_success"] is False
    assert fake.calls[0]["params"]["save"]["auto_save"] is True
    assert fake.calls[1]["tool"] == "umg.widget.event.bind"
    assert fake.calls[1]["params"]["compile_on_success"] is True
    assert result.result["step_count"] == 2
    assert result.result["failed_count"] == 0


@pytest.mark.asyncio
async def test_compose_fallbacks_event_bind_to_binding_set() -> None:
    fake = FakePassThrough(
        [
            "umg.binding.set",
        ]
    )
    service = UMGOrchestrationService(fake)  # type: ignore[arg-type]

    result = await service.call_virtual_tool(
        tool_name="umg.workflow.compose",
        arguments={
            "object_path": "/Game/UI/WBP_Test.WBP_Test",
            "compile_on_finish": False,
            "actions": [
                {
                    "kind": "widget.event.bind",
                    "args": {
                        "widget_ref": {"name": "RuntimeButton"},
                        "event_name": "OnClicked",
                        "function_name": "HandleRuntimeButtonClicked",
                    },
                }
            ],
        },
    )

    assert result.ok is True
    assert len(fake.calls) == 1
    assert fake.calls[0]["tool"] == "umg.binding.set"
    assert fake.calls[0]["params"]["property_name"] == "OnClicked"
    assert fake.calls[0]["params"]["function_name"] == "HandleRuntimeButtonClicked"
    assert result.result["steps"][0]["fallback"] == "fallback: umg.widget.event.bind -> umg.binding.set"


@pytest.mark.asyncio
async def test_compose_rejects_invalid_actions() -> None:
    fake = FakePassThrough(["umg.widget.patch"])
    service = UMGOrchestrationService(fake)  # type: ignore[arg-type]

    with pytest.raises(ValueError):
        await service.call_virtual_tool(
            tool_name="umg.workflow.compose",
            arguments={
                "object_path": "/Game/UI/WBP_Test.WBP_Test",
                "actions": [{"kind": "unknown.kind", "args": {}}],
            },
        )
