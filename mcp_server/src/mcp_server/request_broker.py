from __future__ import annotations

import asyncio
from dataclasses import dataclass
import logging
import time
import uuid
from typing import TYPE_CHECKING
from typing import Any, Awaitable, Callable

from mcp_server.models.ue_messages import UeResponse, parse_mcp_response

if TYPE_CHECKING:
    from mcp_server.metrics import RuntimeMetrics

LOGGER = logging.getLogger("mcp_server.request_broker")


class BrokerError(RuntimeError):
    """요청 브로커 처리 실패."""


class RequestTimeoutError(TimeoutError):
    """요청 타임아웃."""


@dataclass
class _PendingRequest:
    request_id: str
    tool: str
    timeout_ms: int
    created_at_s: float
    future: asyncio.Future[UeResponse]


class RequestBroker:
    def __init__(
        self,
        default_timeout_ms: int = 30_000,
        *,
        metrics: "RuntimeMetrics | None" = None,
    ):
        if default_timeout_ms <= 0:
            raise ValueError("default_timeout_ms must be > 0")

        self._default_timeout_ms = default_timeout_ms
        self._pending_by_request_id: dict[str, _PendingRequest] = {}
        self._lock = asyncio.Lock()
        self._metrics = metrics

    @property
    def default_timeout_ms(self) -> int:
        return self._default_timeout_ms

    async def pending_count(self) -> int:
        async with self._lock:
            return len(self._pending_by_request_id)

    async def send_request(
        self,
        *,
        send_json: Callable[[dict[str, Any]], Awaitable[None]],
        tool: str,
        params: dict[str, Any] | None = None,
        context: dict[str, Any] | None = None,
        timeout_ms: int | None = None,
        request_id: str | None = None,
        protocol: str = "unreal-mcp/1.0",
        session_id: str = "default-session",
    ) -> UeResponse:
        if not tool:
            raise BrokerError("tool is required.")
        if self._metrics is not None:
            self._metrics.inc("request_broker.request_started")

        final_timeout_ms = self._resolve_timeout_ms(timeout_ms, context)
        final_request_id = request_id or f"req-{uuid.uuid4().hex}"
        request_context = dict(context or {})
        request_context.setdefault("timeout_ms", final_timeout_ms)

        envelope: dict[str, Any] = {
            "protocol": protocol,
            "request_id": final_request_id,
            "session_id": session_id,
            "tool": tool,
            "params": params or {},
            "context": request_context,
        }
        message: dict[str, Any] = {
            "type": "mcp.request",
            "request": envelope,
        }

        pending = await self._register_pending(
            request_id=final_request_id,
            tool=tool,
            timeout_ms=final_timeout_ms,
        )

        try:
            await send_json(message)
        except Exception:
            await self._remove_pending(final_request_id)
            if self._metrics is not None:
                self._metrics.inc("request_broker.send_failed")
            raise

        try:
            response = await asyncio.wait_for(
                pending.future,
                timeout=final_timeout_ms / 1000.0,
            )
        except asyncio.TimeoutError as exc:
            await self._remove_pending(final_request_id)
            if self._metrics is not None:
                self._metrics.inc("request_broker.request_timeout")
            raise RequestTimeoutError(
                f"Request timed out: request_id={final_request_id}, tool={tool}, timeout_ms={final_timeout_ms}"
            ) from exc

        if self._metrics is not None:
            self._metrics.inc("request_broker.request_resolved")
        return response

    async def resolve_from_message(self, message: dict[str, Any]) -> bool:
        parsed_response = parse_mcp_response(message)
        if parsed_response is None:
            return False
        return await self.resolve_response(parsed_response)

    async def resolve_response(self, response: UeResponse) -> bool:
        pending = await self._remove_pending(response.request_id)
        if pending is None:
            LOGGER.warning("Received response for unknown request_id: %s", response.request_id)
            if self._metrics is not None:
                self._metrics.inc("request_broker.unknown_response")
            return False

        if not pending.future.done():
            pending.future.set_result(response)
        if self._metrics is not None:
            self._metrics.inc("request_broker.response_mapped")
        return True

    async def fail_all(self, error: Exception) -> None:
        async with self._lock:
            pending_items = list(self._pending_by_request_id.values())
            self._pending_by_request_id.clear()
            if self._metrics is not None:
                self._metrics.set_gauge("request_broker.pending", 0)

        for pending in pending_items:
            if not pending.future.done():
                pending.future.set_exception(error)
        if self._metrics is not None and pending_items:
            self._metrics.inc("request_broker.fail_all", len(pending_items))

    async def _register_pending(
        self,
        *,
        request_id: str,
        tool: str,
        timeout_ms: int,
    ) -> _PendingRequest:
        loop = asyncio.get_running_loop()
        future: asyncio.Future[UeResponse] = loop.create_future()
        pending = _PendingRequest(
            request_id=request_id,
            tool=tool,
            timeout_ms=timeout_ms,
            created_at_s=time.monotonic(),
            future=future,
        )

        async with self._lock:
            if request_id in self._pending_by_request_id:
                raise BrokerError(f"Duplicate request_id: {request_id}")
            self._pending_by_request_id[request_id] = pending
            if self._metrics is not None:
                self._metrics.set_gauge("request_broker.pending", len(self._pending_by_request_id))

        return pending

    async def _remove_pending(self, request_id: str) -> _PendingRequest | None:
        async with self._lock:
            pending = self._pending_by_request_id.pop(request_id, None)
            if self._metrics is not None:
                self._metrics.set_gauge("request_broker.pending", len(self._pending_by_request_id))
            return pending

    def _resolve_timeout_ms(
        self,
        timeout_ms: int | None,
        context: dict[str, Any] | None,
    ) -> int:
        if timeout_ms is not None:
            if timeout_ms <= 0:
                raise BrokerError("timeout_ms must be > 0")
            return timeout_ms

        if context and "timeout_ms" in context:
            context_timeout = int(context["timeout_ms"])
            if context_timeout <= 0:
                raise BrokerError("context.timeout_ms must be > 0")
            return context_timeout

        return self._default_timeout_ms
