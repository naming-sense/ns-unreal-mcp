from __future__ import annotations

from dataclasses import dataclass
import os
import socket
import uuid
from typing import Any

from mcp_server.models.ue_messages import UeResponse
from mcp_server.ue_transport import UeWsTransport


class ToolExecutionError(RuntimeError):
    """도구 실행 실패."""


@dataclass(frozen=True)
class ToolCallResult:
    ok: bool
    status: str
    request_id: str
    result: dict[str, Any]
    diagnostics: dict[str, Any]
    raw_envelope: dict[str, Any]


def _build_runtime_session_id() -> str:
    host = socket.gethostname().replace("|", "-")
    process_id = os.getpid()
    suffix = uuid.uuid4().hex[:8]
    return f"mcp-server:{host}:{process_id}:{suffix}"


class MCPFacade:
    def __init__(
        self,
        transport: UeWsTransport,
        *,
        session_id: str | None = None,
    ) -> None:
        self._transport = transport
        self._session_id = session_id or _build_runtime_session_id()

    async def call_tool(
        self,
        *,
        tool: str,
        params: dict[str, Any] | None = None,
        context: dict[str, Any] | None = None,
        timeout_ms: int | None = None,
        request_id: str | None = None,
        session_id: str | None = None,
        raise_on_error: bool = True,
    ) -> ToolCallResult:
        response: UeResponse = await self._transport.request(
            tool=tool,
            params=params,
            context=context,
            timeout_ms=timeout_ms,
            request_id=request_id,
            session_id=session_id or self._session_id,
        )
        envelope = response.envelope
        status = str(envelope.get("status", "error"))
        diagnostics = envelope.get("diagnostics")
        if not isinstance(diagnostics, dict):
            diagnostics = {}

        result = envelope.get("result")
        if not isinstance(result, dict):
            result = {}

        call_result = ToolCallResult(
            ok=response.ok,
            status=status,
            request_id=response.request_id,
            result=result,
            diagnostics=diagnostics,
            raw_envelope=envelope,
        )

        if raise_on_error and (status == "error" or not response.ok):
            raise ToolExecutionError(
                f"Tool execution failed: tool={tool}, request_id={response.request_id}, status={status}, diagnostics={diagnostics}"
            )

        return call_result
