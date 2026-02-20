from __future__ import annotations

from mcp_server.metrics import RuntimeMetrics


def test_metrics_snapshot_contains_counters_and_tool_metrics() -> None:
    metrics = RuntimeMetrics()
    metrics.inc("a.counter")
    metrics.set_gauge("a.gauge", 3.5)
    metrics.observe_tool_result(tool="system.health", status="ok", duration_ms=12, retry_attempts=1)
    metrics.observe_tool_exception(
        tool="system.health",
        exception_name="TimeoutError",
        duration_ms=20,
        retry_attempts=2,
    )

    snapshot = metrics.snapshot()
    assert snapshot["counters"]["a.counter"] == 1
    assert snapshot["gauges"]["a.gauge"] == 3.5
    tool_metrics = snapshot["tool_metrics"][0]
    assert tool_metrics["tool"] == "system.health"
    assert tool_metrics["total_requests"] == 2
    assert tool_metrics["ok_count"] == 1
    assert tool_metrics["exception_count"] == 1
    assert tool_metrics["failed_count"] == 1
    assert tool_metrics["failure_rate"] == 0.5
    assert tool_metrics["p95_duration_ms"] == 20
    assert tool_metrics["sample_count"] == 2
