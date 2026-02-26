from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable
from dataclasses import asdict
import logging
import time
from typing import TYPE_CHECKING
import uuid
from typing import Any

from mcp_server.event_router import EventRouter
from mcp_server.mcp_facade import MCPFacade, ToolCallResult
from mcp_server.request_broker import RequestTimeoutError
from mcp_server.tool_catalog import ToolCatalog, ToolDefinition
from mcp_server.utils.retry import next_backoff_delay

if TYPE_CHECKING:
    from mcp_server.metrics import RuntimeMetrics

LOGGER = logging.getLogger("mcp_server.tool_passthrough")


class UnknownToolError(KeyError):
    """동기화된 툴 카탈로그에 존재하지 않는 툴."""


class CatalogGuardError(RuntimeError):
    """카탈로그 가드(required_tools/schema_hash) 위반."""


EventCallback = Callable[[dict[str, Any]], Awaitable[None] | None]


class MCPPassThroughService:
    def __init__(
        self,
        *,
        facade: MCPFacade,
        catalog: ToolCatalog,
        event_router: EventRouter | None = None,
        include_schemas: bool = True,
        refresh_interval_s: float = 60.0,
        transient_max_attempts: int = 2,
        retry_backoff_initial_s: float = 0.2,
        retry_backoff_max_s: float = 1.0,
        required_tools: list[str] | tuple[str, ...] = (),
        pin_schema_hash: str = "",
        fail_on_schema_change: bool = False,
        metrics: "RuntimeMetrics | None" = None,
    ) -> None:
        self._facade = facade
        self._catalog = catalog
        self._event_router = event_router
        self._include_schemas = include_schemas
        self._refresh_interval_s = refresh_interval_s
        self._transient_max_attempts = max(transient_max_attempts, 1)
        self._retry_backoff_initial_s = retry_backoff_initial_s
        self._retry_backoff_max_s = retry_backoff_max_s
        self._required_tools = tuple(
            tool.strip() for tool in required_tools if isinstance(tool, str) and tool.strip()
        )
        self._pin_schema_hash = pin_schema_hash.strip().upper()
        self._fail_on_schema_change = fail_on_schema_change
        self._baseline_schema_hash: str | None = None
        self._metrics = metrics

        self._refresh_lock = asyncio.Lock()
        self._stop_event = asyncio.Event()
        self._refresh_task: asyncio.Task[None] | None = None
        self._last_refresh_ms: int = 0

    @property
    def last_refresh_ms(self) -> int:
        return self._last_refresh_ms

    @property
    def protocol_version(self) -> str:
        return self._catalog.protocol_version

    @property
    def schema_hash(self) -> str:
        return self._catalog.schema_hash

    async def start(self) -> None:
        await self.refresh_catalog()
        self._stop_event.clear()

        if self._refresh_interval_s > 0:
            self._refresh_task = asyncio.create_task(
                self._refresh_loop(),
                name="tool-catalog-refresh",
            )

    async def stop(self) -> None:
        self._stop_event.set()
        task = self._refresh_task
        self._refresh_task = None
        if task is not None:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    async def refresh_catalog(self) -> None:
        async with self._refresh_lock:
            attempt = 1
            delay_s = self._retry_backoff_initial_s
            while True:
                try:
                    await self._catalog.refresh(
                        self._facade,
                        include_schemas=self._include_schemas,
                    )
                    self._validate_catalog_guard()
                    self._last_refresh_ms = int(time.time() * 1000)
                    LOGGER.info(
                        "Tool catalog refreshed. tools=%d protocol=%s schema_hash=%s",
                        len(self._catalog.tools),
                        self._catalog.protocol_version,
                        self._catalog.schema_hash,
                    )
                    if self._metrics is not None:
                        self._metrics.inc("tool_passthrough.catalog_refresh_success")
                        self._metrics.set_gauge(
                            "tool_passthrough.catalog_tool_count", len(self._catalog.tools)
                        )
                    return
                except Exception as exc:
                    if self._metrics is not None:
                        self._metrics.inc("tool_passthrough.catalog_refresh_failed")
                    if (
                        attempt >= self._transient_max_attempts
                        or not self._is_retryable_exception(exc)
                    ):
                        raise

                    LOGGER.warning(
                        "Catalog refresh retrying after error. attempt=%d/%d delay=%.2fs error=%s",
                        attempt,
                        self._transient_max_attempts,
                        delay_s,
                        exc,
                    )
                    await asyncio.sleep(delay_s)
                    attempt += 1
                    delay_s = next_backoff_delay(delay_s, self._retry_backoff_max_s)

    def list_tools(self) -> list[ToolDefinition]:
        return self._catalog.tools

    def list_tools_as_dict(self) -> list[dict[str, Any]]:
        return [asdict(tool) for tool in self._catalog.tools]

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
        tool_definition = await self._resolve_tool_definition(tool)
        if not tool_definition.enabled:
            raise UnknownToolError(f"Tool is disabled: {tool}")

        max_attempts = self._transient_max_attempts if allow_retry else 1
        attempt = 1
        retry_count = 0
        delay_s = self._retry_backoff_initial_s
        started_at = time.perf_counter()
        request_id_base = request_id

        while True:
            call_request_id = (
                f"{request_id_base}-r{attempt}"
                if request_id_base is not None and attempt > 1
                else request_id_base
            )

            try:
                result = await self._facade.call_tool(
                    tool=tool,
                    params=params,
                    context=context,
                    timeout_ms=timeout_ms,
                    request_id=call_request_id,
                    raise_on_error=False,
                )
            except Exception as exc:
                if attempt < max_attempts and self._is_retryable_exception(exc):
                    await self._on_retry(tool=tool, attempt=attempt, delay_s=delay_s, reason=str(exc))
                    retry_count += 1
                    attempt += 1
                    await asyncio.sleep(delay_s)
                    delay_s = next_backoff_delay(delay_s, self._retry_backoff_max_s)
                    continue

                duration_ms = int((time.perf_counter() - started_at) * 1000)
                if self._metrics is not None:
                    self._metrics.observe_tool_exception(
                        tool=tool,
                        exception_name=exc.__class__.__name__,
                        duration_ms=duration_ms,
                        retry_attempts=retry_count,
                    )
                raise

            if (
                attempt < max_attempts
                and result.status == "error"
                and self._is_retryable_tool_result(result)
            ):
                await self._on_retry(
                    tool=tool,
                    attempt=attempt,
                    delay_s=delay_s,
                    reason=self._build_retry_reason(result),
                )
                retry_count += 1
                attempt += 1
                await asyncio.sleep(delay_s)
                delay_s = next_backoff_delay(delay_s, self._retry_backoff_max_s)
                continue

            duration_ms = int((time.perf_counter() - started_at) * 1000)
            if self._metrics is not None:
                self._metrics.observe_tool_result(
                    tool=tool,
                    status=result.status,
                    duration_ms=duration_ms,
                    retry_attempts=retry_count,
                )
            return result

    async def call_tool_stream(
        self,
        *,
        tool: str,
        params: dict[str, Any] | None = None,
        context: dict[str, Any] | None = None,
        timeout_ms: int | None = None,
        request_id: str | None = None,
        on_event: EventCallback | None = None,
        poll_interval_s: float = 0.1,
    ) -> ToolCallResult:
        if self._event_router is None:
            raise RuntimeError("Event router is required for call_tool_stream.")
        if poll_interval_s <= 0:
            raise ValueError("poll_interval_s must be > 0")

        resolved_request_id = request_id or f"req-{uuid.uuid4().hex}"
        subscription = self._event_router.subscribe(request_id=resolved_request_id)

        async def _dispatch_event(event: dict[str, Any]) -> None:
            if on_event is None:
                return
            maybe_awaitable = on_event(event)
            if asyncio.iscoroutine(maybe_awaitable):
                await maybe_awaitable

        task = asyncio.create_task(
            self.call_tool(
                tool=tool,
                params=params,
                context=context,
                timeout_ms=timeout_ms,
                request_id=resolved_request_id,
                allow_retry=False,
            ),
            name=f"tool-call-{tool}-{resolved_request_id}",
        )

        try:
            while not task.done():
                event = await subscription.get(timeout_s=poll_interval_s)
                if event is None:
                    continue
                await _dispatch_event(event)

            result = await task

            while True:
                event = subscription.get_nowait()
                if event is None:
                    break
                await _dispatch_event(event)

            return result
        finally:
            subscription.close()

    async def _resolve_tool_definition(self, tool: str) -> ToolDefinition:
        definition = self._catalog.get_tool(tool)
        if definition is not None:
            return definition

        await self.refresh_catalog()
        definition = self._catalog.get_tool(tool)
        if definition is not None:
            return definition

        raise UnknownToolError(f"Unknown tool: {tool}")

    async def _refresh_loop(self) -> None:
        while not self._stop_event.is_set():
            await asyncio.sleep(self._refresh_interval_s)
            if self._stop_event.is_set():
                return

            try:
                await self.refresh_catalog()
            except CatalogGuardError:
                LOGGER.exception("Tool catalog guard failed.")
                if self._fail_on_schema_change:
                    return
            except Exception:
                LOGGER.exception("Tool catalog refresh failed.")

    async def _on_retry(self, *, tool: str, attempt: int, delay_s: float, reason: str) -> None:
        LOGGER.warning(
            "Retrying tool call. tool=%s attempt=%d delay=%.2fs reason=%s",
            tool,
            attempt,
            delay_s,
            reason,
        )
        if self._metrics is not None:
            self._metrics.inc("tool_passthrough.retry")
            self._metrics.inc(f"tool_passthrough.retry_tool.{tool}")

    @staticmethod
    def _is_retryable_exception(exc: Exception) -> bool:
        return isinstance(exc, (RequestTimeoutError, TimeoutError, ConnectionError))

    @staticmethod
    def _is_retryable_tool_result(result: ToolCallResult) -> bool:
        diagnostics = result.diagnostics if isinstance(result.diagnostics, dict) else {}
        errors = diagnostics.get("errors")
        if isinstance(errors, list):
            for error in errors:
                if isinstance(error, dict) and bool(error.get("retriable", False)):
                    return True
        return False

    @staticmethod
    def _build_retry_reason(result: ToolCallResult) -> str:
        diagnostics = result.diagnostics if isinstance(result.diagnostics, dict) else {}
        errors = diagnostics.get("errors")
        if isinstance(errors, list):
            for error in errors:
                if not isinstance(error, dict):
                    continue
                code = error.get("code")
                message = error.get("message")
                if code or message:
                    return f"{code}: {message}"
        return "retryable tool error"

    def _validate_catalog_guard(self) -> None:
        if self._required_tools:
            available_tool_names = {tool.name for tool in self._catalog.tools}
            missing_tools = [tool for tool in self._required_tools if tool not in available_tool_names]
            if missing_tools:
                raise CatalogGuardError(
                    "Missing required tools after catalog refresh: "
                    + ", ".join(missing_tools)
                )

        schema_hash = self._catalog.schema_hash.strip().upper()
        if self._pin_schema_hash:
            if schema_hash != self._pin_schema_hash:
                raise CatalogGuardError(
                    "schema_hash mismatch: "
                    f"expected={self._pin_schema_hash} actual={schema_hash or '-'}"
                )

        if not self._fail_on_schema_change:
            return

        if not schema_hash:
            return

        if self._baseline_schema_hash is None:
            self._baseline_schema_hash = schema_hash
            return

        if schema_hash != self._baseline_schema_hash:
            raise CatalogGuardError(
                "schema_hash changed during runtime: "
                f"baseline={self._baseline_schema_hash} current={schema_hash}"
            )
