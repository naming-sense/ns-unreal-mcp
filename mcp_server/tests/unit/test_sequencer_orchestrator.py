from __future__ import annotations

from typing import Any

import pytest

from mcp_server.mcp_facade import ToolCallResult
from mcp_server.sequencer_orchestrator import SequencerOrchestrationService
from mcp_server.tool_catalog import ToolDefinition


class FakePassThrough:
    def __init__(self, tool_names: list[str], *, capabilities: tuple[str, ...] = ()) -> None:
        self.calls: list[dict[str, Any]] = []
        self.capabilities = capabilities
        self._tools = [
            ToolDefinition(
                name=tool_name,
                domain="seq",
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
        params = params or {}
        self.calls.append(
            {
                "tool": tool,
                "params": params,
                "request_id": request_id,
                "context": context,
                "timeout_ms": timeout_ms,
                "allow_retry": allow_retry,
            }
        )

        result: dict[str, Any] = {"touched_packages": ["/Game/Seq/Touched"]}
        if tool == "seq.asset.create":
            result["object_path"] = "/Game/Seq/LS_Test.LS_Test"
        elif tool == "seq.inspect":
            result["object_path"] = params.get("object_path", "")

        return ToolCallResult(
            ok=True,
            status="ok",
            request_id=request_id or f"req-{len(self.calls)}",
            result=result,
            diagnostics={"errors": [], "warnings": [], "infos": []},
            raw_envelope={},
        )


@pytest.mark.asyncio
async def test_seq_compose_passes_object_path_from_create() -> None:
    fake = FakePassThrough(
        [
            "seq.asset.create",
            "seq.inspect",
        ],
        capabilities=("sequencer_core_v1",),
    )
    service = SequencerOrchestrationService(fake)  # type: ignore[arg-type]

    result = await service.call_virtual_tool(
        tool_name="seq.workflow.compose",
        request_id="mcp-seq-1",
        arguments={
            "actions": [
                {
                    "kind": "asset.create",
                    "args": {
                        "package_path": "/Game/Seq",
                        "asset_name": "LS_Test",
                    },
                },
                {
                    "kind": "inspect",
                    "args": {},
                },
            ],
        },
    )

    assert result.ok is True
    assert result.status == "ok"
    assert len(fake.calls) == 2
    assert fake.calls[0]["tool"] == "seq.asset.create"
    assert fake.calls[1]["tool"] == "seq.inspect"
    assert fake.calls[1]["params"]["object_path"] == "/Game/Seq/LS_Test.LS_Test"


@pytest.mark.asyncio
async def test_seq_compose_fallbacks_bulk_set_to_key_set_without_capability() -> None:
    fake = FakePassThrough(["seq.key.set"])  # no seq.key.bulk_set tool/capability
    service = SequencerOrchestrationService(fake)  # type: ignore[arg-type]

    result = await service.call_virtual_tool(
        tool_name="seq.workflow.compose",
        arguments={
            "object_path": "/Game/Seq/LS_Test.LS_Test",
            "actions": [
                {
                    "kind": "key.bulk_set",
                    "args": {
                        "channel_id": "SectionA|float|0",
                        "keys": [
                            {"frame": 0, "value": 0.0},
                            {"frame": 10, "value": 1.0},
                        ],
                    },
                }
            ],
        },
    )

    assert result.ok is True
    assert len(fake.calls) == 1
    assert fake.calls[0]["tool"] == "seq.key.set"
    assert fake.calls[0]["params"]["frame"] == 0
    assert fake.calls[0]["params"]["value"] == 0.0
    assert result.result["steps"][0]["fallback"] == "fallback: seq.key.bulk_set -> seq.key.set(first key)"
