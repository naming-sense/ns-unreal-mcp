from __future__ import annotations

from collections import defaultdict, deque
from dataclasses import dataclass, field
import math
import threading
import time
from typing import Any


@dataclass
class ToolMetricState:
    total_requests: int = 0
    ok_count: int = 0
    partial_count: int = 0
    error_count: int = 0
    exception_count: int = 0
    total_duration_ms: int = 0
    max_duration_ms: int = 0
    last_duration_ms: int = 0
    retry_attempts: int = 0
    duration_samples_ms: deque[int] = field(default_factory=lambda: deque(maxlen=200))


class RuntimeMetrics:
    def __init__(self) -> None:
        self._started_ms = int(time.time() * 1000)
        self._counters: dict[str, int] = defaultdict(int)
        self._gauges: dict[str, float] = {}
        self._tool_metrics: dict[str, ToolMetricState] = defaultdict(ToolMetricState)
        self._lock = threading.Lock()

    def inc(self, name: str, value: int = 1) -> None:
        with self._lock:
            self._counters[name] += value

    def set_gauge(self, name: str, value: float) -> None:
        with self._lock:
            self._gauges[name] = value

    def observe_tool_result(
        self,
        *,
        tool: str,
        status: str,
        duration_ms: int,
        retry_attempts: int = 0,
    ) -> None:
        duration_ms = max(duration_ms, 0)
        with self._lock:
            state = self._tool_metrics[tool]
            state.total_requests += 1
            state.total_duration_ms += duration_ms
            state.max_duration_ms = max(state.max_duration_ms, duration_ms)
            state.last_duration_ms = duration_ms
            state.retry_attempts += max(retry_attempts, 0)
            state.duration_samples_ms.append(duration_ms)

            if status == "ok":
                state.ok_count += 1
            elif status == "partial":
                state.partial_count += 1
            else:
                state.error_count += 1

    def observe_tool_exception(
        self,
        *,
        tool: str,
        exception_name: str,
        duration_ms: int,
        retry_attempts: int = 0,
    ) -> None:
        duration_ms = max(duration_ms, 0)
        with self._lock:
            state = self._tool_metrics[tool]
            state.total_requests += 1
            state.exception_count += 1
            state.total_duration_ms += duration_ms
            state.max_duration_ms = max(state.max_duration_ms, duration_ms)
            state.last_duration_ms = duration_ms
            state.retry_attempts += max(retry_attempts, 0)
            state.duration_samples_ms.append(duration_ms)
            self._counters[f"tool_exception.{tool}.{exception_name}"] += 1

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            uptime_ms = max(int(time.time() * 1000) - self._started_ms, 0)
            tools = []
            for tool_name in sorted(self._tool_metrics.keys()):
                state = self._tool_metrics[tool_name]
                avg_duration_ms = (
                    int(state.total_duration_ms / state.total_requests)
                    if state.total_requests > 0
                    else 0
                )
                failed_count = state.error_count + state.exception_count
                failure_rate = (
                    float(failed_count) / float(state.total_requests)
                    if state.total_requests > 0
                    else 0.0
                )
                tools.append(
                    {
                        "tool": tool_name,
                        "total_requests": state.total_requests,
                        "ok_count": state.ok_count,
                        "partial_count": state.partial_count,
                        "error_count": state.error_count,
                        "exception_count": state.exception_count,
                        "retry_attempts": state.retry_attempts,
                        "avg_duration_ms": avg_duration_ms,
                        "p95_duration_ms": self._p95_duration_ms(state.duration_samples_ms),
                        "max_duration_ms": state.max_duration_ms,
                        "last_duration_ms": state.last_duration_ms,
                        "failed_count": failed_count,
                        "failure_rate": round(failure_rate, 4),
                        "sample_count": len(state.duration_samples_ms),
                    }
                )

            return {
                "started_at_ms": self._started_ms,
                "uptime_ms": uptime_ms,
                "counters": dict(sorted(self._counters.items(), key=lambda x: x[0])),
                "gauges": dict(sorted(self._gauges.items(), key=lambda x: x[0])),
                "tool_metrics": tools,
            }

    @staticmethod
    def _p95_duration_ms(samples: deque[int]) -> int:
        if not samples:
            return 0
        sorted_samples = sorted(samples)
        index = max(math.ceil(len(sorted_samples) * 0.95) - 1, 0)
        return sorted_samples[index]
