from __future__ import annotations

from dataclasses import dataclass
import time
from typing import Any

from mcp_server.mcp_facade import MCPFacade


@dataclass(frozen=True)
class HealthSnapshot:
    captured_at_ms: int
    ok: bool
    latency_ms: int
    payload: dict[str, Any]


class HealthMonitor:
    def __init__(self, facade: MCPFacade) -> None:
        self._facade = facade

    async def check_once(self) -> HealthSnapshot:
        start_ns = time.perf_counter_ns()
        response = await self._facade.call_tool(tool="system.health", params={})
        latency_ms = int((time.perf_counter_ns() - start_ns) / 1_000_000)
        return HealthSnapshot(
            captured_at_ms=int(time.time() * 1000),
            ok=response.ok,
            latency_ms=latency_ms,
            payload=response.result,
        )
