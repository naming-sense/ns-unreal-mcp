from __future__ import annotations

import argparse
import asyncio
from dataclasses import replace
import json
import logging
import signal
from pathlib import Path
from typing import Mapping

from mcp_server.config import AppConfig, ConfigError, load_config
from mcp_server.event_router import EventRouter
from mcp_server.health_monitor import HealthMonitor
from mcp_server.logging_setup import configure_logging
from mcp_server.mcp_facade import MCPFacade
from mcp_server.metrics import RuntimeMetrics
from mcp_server.request_broker import RequestBroker
from mcp_server.tool_catalog import ToolCatalog
from mcp_server.tool_passthrough import CatalogGuardError, MCPPassThroughService, UnknownToolError
from mcp_server.ue_transport import UeWsTransport
from mcp_server.ws_endpoint import (
    WsEndpointCandidate,
    WsEndpointSelectionError,
    WsEndpointSelector,
    list_ws_endpoint_candidates,
    resolve_ws_endpoint,
)

LOGGER = logging.getLogger("mcp_server.app")


async def run(
    config: AppConfig,
    *,
    once_health: bool = False,
    once_tools: bool = False,
    once_endpoints: bool = False,
    call_tool: str | None = None,
    call_tool_params: dict[str, object] | None = None,
    call_tool_context: dict[str, object] | None = None,
    call_tool_timeout_ms: int | None = None,
    stream_events: bool = False,
    print_metrics: bool = False,
    endpoint_selector: WsEndpointSelector | None = None,
) -> int:
    if once_endpoints:
        payload = _build_endpoint_listing_payload(
            config=config,
            endpoint_selector=endpoint_selector,
        )
        print(json.dumps(payload, ensure_ascii=False))
        return 0

    try:
        endpoint_resolution = resolve_ws_endpoint(config, selector=endpoint_selector)
    except WsEndpointSelectionError as exc:
        LOGGER.error("UE endpoint selection failed: %s", exc)
        return 2
    config = replace(config, ue=replace(config.ue, ws_url=endpoint_resolution.ws_url))

    LOGGER.info("Server bootstrap complete.")
    LOGGER.info("UE WS endpoint: %s", config.ue.ws_url)
    LOGGER.info("UE WS endpoint source: %s", endpoint_resolution.source)
    if endpoint_resolution.connection_file:
        LOGGER.info("UE connection file: %s", endpoint_resolution.connection_file)
    if endpoint_resolution.instance_id:
        LOGGER.info(
            "UE endpoint instance: instance_id=%s project_dir=%s process_id=%s project_name=%s",
            endpoint_resolution.instance_id,
            endpoint_resolution.project_dir or "-",
            endpoint_resolution.process_id if endpoint_resolution.process_id is not None else "-",
            endpoint_resolution.project_name or "-",
        )
    LOGGER.info("Default request timeout: %dms", config.request.default_timeout_ms)

    metrics = RuntimeMetrics() if config.metrics.enabled else None
    request_broker = RequestBroker(
        default_timeout_ms=config.request.default_timeout_ms,
        metrics=metrics,
    )
    event_router = EventRouter(metrics=metrics)
    transport = UeWsTransport(
        ws_url=config.ue.ws_url,
        request_broker=request_broker,
        event_router=event_router,
        connect_timeout_s=config.ue.connect_timeout_s,
        ping_interval_s=config.ue.ping_interval_s,
        reconnect_initial_delay_s=config.ue.reconnect.initial_delay_s,
        reconnect_max_delay_s=config.ue.reconnect.max_delay_s,
        metrics=metrics,
        expected_instance_id=endpoint_resolution.instance_id,
        expected_process_id=endpoint_resolution.process_id,
        expected_project_dir=endpoint_resolution.project_dir,
    )
    facade = MCPFacade(transport)
    health_monitor = HealthMonitor(facade)
    tool_catalog = ToolCatalog()
    pass_through = MCPPassThroughService(
        facade=facade,
        catalog=tool_catalog,
        event_router=event_router,
        include_schemas=config.catalog.include_schemas,
        refresh_interval_s=config.catalog.refresh_interval_s,
        transient_max_attempts=config.retry.transient_max_attempts,
        retry_backoff_initial_s=config.retry.backoff_initial_s,
        retry_backoff_max_s=config.retry.backoff_max_s,
        required_tools=config.catalog.required_tools,
        pin_schema_hash=config.catalog.pin_schema_hash,
        fail_on_schema_change=config.catalog.fail_on_schema_change,
        metrics=metrics,
    )

    stop_event = asyncio.Event()
    metrics_task: asyncio.Task[None] | None = None

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop_event.set)
        except NotImplementedError:
            pass

    if metrics is not None:
        metrics.inc("app.start")
    await transport.start()
    try:
        await transport.wait_until_connected(timeout_s=config.ue.connect_timeout_s)
    except TimeoutError:
        LOGGER.error("Failed to connect UE WS within timeout: %.1fs", config.ue.connect_timeout_s)
        await transport.stop()
        return 3

    try:
        if metrics is not None and config.metrics.log_interval_s > 0:
            metrics_task = asyncio.create_task(
                _metrics_log_loop(metrics, config.metrics.log_interval_s, stop_event),
                name="metrics-log-loop",
            )

        try:
            await pass_through.start()
        except CatalogGuardError as exc:
            LOGGER.error("Catalog guard failed: %s", exc)
            _print_error_payload(
                _build_error_payload(
                    code="MCP.SERVER.CATALOG_GUARD_FAILED",
                    message=str(exc),
                    retriable=False,
                ),
                stream_events=stream_events,
            )
            return 7

        if once_health:
            health_snapshot = await health_monitor.check_once()
            LOGGER.info(
                "Health snapshot: ok=%s latency_ms=%d",
                health_snapshot.ok,
                health_snapshot.latency_ms,
            )
            print(json.dumps(health_snapshot.payload, ensure_ascii=False))
            _maybe_print_metrics(
                metrics=metrics,
                print_metrics=print_metrics,
                stream_events=stream_events,
            )
            return 0

        if once_tools:
            payload = {
                "protocol_version": pass_through.protocol_version,
                "schema_hash": pass_through.schema_hash,
                "last_refresh_ms": pass_through.last_refresh_ms,
                "tools": pass_through.list_tools_as_dict(),
            }
            print(json.dumps(payload, ensure_ascii=False))
            _maybe_print_metrics(
                metrics=metrics,
                print_metrics=print_metrics,
                stream_events=stream_events,
            )
            return 0

        if call_tool:
            try:
                if stream_events:
                    def _print_event(event: dict[str, object]) -> None:
                        print(
                            json.dumps(
                                {
                                    "type": "event",
                                    "event": event,
                                },
                                ensure_ascii=False,
                            ),
                            flush=True,
                        )

                    call_result = await pass_through.call_tool_stream(
                        tool=call_tool,
                        params=call_tool_params or {},
                        context=call_tool_context or {},
                        timeout_ms=call_tool_timeout_ms,
                        on_event=_print_event,
                    )
                else:
                    call_result = await pass_through.call_tool(
                        tool=call_tool,
                        params=call_tool_params or {},
                        context=call_tool_context or {},
                        timeout_ms=call_tool_timeout_ms,
                    )
            except UnknownToolError as exc:
                LOGGER.error("%s", exc)
                _print_error_payload(
                    _build_error_payload(
                        code="MCP.SERVER.TOOL_NOT_FOUND",
                        message=str(exc),
                        retriable=False,
                    ),
                    stream_events=stream_events,
                )
                _maybe_print_metrics(
                    metrics=metrics,
                    print_metrics=print_metrics,
                    stream_events=stream_events,
                )
                return 4
            except Exception as exc:
                LOGGER.exception("Tool call failed with unhandled exception.")
                retriable = isinstance(exc, (ConnectionError, TimeoutError))
                _print_error_payload(
                    _build_error_payload(
                        code="MCP.SERVER.TRANSIENT_FAILURE" if retriable else "MCP.SERVER.INTERNAL",
                        message=str(exc),
                        retriable=retriable,
                    ),
                    stream_events=stream_events,
                )
                _maybe_print_metrics(
                    metrics=metrics,
                    print_metrics=print_metrics,
                    stream_events=stream_events,
                )
                return 6

            payload = {
                "ok": call_result.ok,
                "status": call_result.status,
                "request_id": call_result.request_id,
                "result": call_result.result,
                "diagnostics": call_result.diagnostics,
                "raw_envelope": call_result.raw_envelope,
            }
            if stream_events:
                print(
                    json.dumps({"type": "result", "result": payload}, ensure_ascii=False),
                    flush=True,
                )
            else:
                print(json.dumps(payload, ensure_ascii=False))
            _maybe_print_metrics(
                metrics=metrics,
                print_metrics=print_metrics,
                stream_events=stream_events,
            )
            return 0 if call_result.status != "error" and call_result.ok else 5

        await stop_event.wait()
        LOGGER.info("Server shutdown requested.")
    finally:
        stop_event.set()
        if metrics_task is not None:
            metrics_task.cancel()
            await asyncio.gather(metrics_task, return_exceptions=True)
        await pass_through.stop()
        await transport.stop()
        if metrics is not None:
            metrics.inc("app.stop")

    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="UE MCP server")
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        help="Path to YAML config file",
    )
    parser.add_argument(
        "--log-level",
        type=str,
        default=None,
        help="Override log level (DEBUG/INFO/WARNING/ERROR)",
    )
    parser.add_argument(
        "--once-health",
        action="store_true",
        help="Connect to UE and run one system.health call, then exit",
    )
    parser.add_argument(
        "--once-tools",
        action="store_true",
        help="Connect to UE, refresh tools catalog, print it, then exit",
    )
    parser.add_argument(
        "--once-endpoints",
        action="store_true",
        help="List discovered UE endpoint candidates and exit",
    )
    parser.add_argument(
        "--call-tool",
        type=str,
        default=None,
        help="Call one UE MCP tool by name, then exit",
    )
    parser.add_argument(
        "--params-json",
        type=str,
        default="{}",
        help="JSON object passed to tool params when --call-tool is used",
    )
    parser.add_argument(
        "--context-json",
        type=str,
        default="{}",
        help="JSON object passed to tool context when --call-tool is used",
    )
    parser.add_argument(
        "--timeout-ms",
        type=int,
        default=None,
        help="Override timeout_ms for --call-tool",
    )
    parser.add_argument(
        "--stream-events",
        action="store_true",
        help="Stream event.* notifications during --call-tool execution",
    )
    parser.add_argument(
        "--print-metrics",
        action="store_true",
        help="Print runtime metrics snapshot at the end of one-shot mode",
    )
    parser.add_argument(
        "--ue-instance-id",
        type=str,
        default=None,
        help="Prefer UE endpoint with matching instance_id",
    )
    parser.add_argument(
        "--ue-project-dir",
        type=str,
        default=None,
        help="Prefer UE endpoint with matching project_dir",
    )
    parser.add_argument(
        "--ue-process-id",
        type=int,
        default=None,
        help="Prefer UE endpoint with matching process_id",
    )
    return parser.parse_args()


def _parse_json_object_arg(value: str, *, arg_name: str) -> dict[str, object]:
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError as exc:
        raise ValueError(f"{arg_name} must be valid JSON object: {exc}") from exc

    if parsed is None:
        return {}
    if not isinstance(parsed, dict):
        raise ValueError(f"{arg_name} must be a JSON object.")
    return parsed


def main() -> int:
    args = parse_args()

    mode_count = sum(
        [
            1 if args.once_health else 0,
            1 if args.once_tools else 0,
            1 if args.once_endpoints else 0,
            1 if args.call_tool else 0,
        ]
    )
    if mode_count > 1:
        print(
            "Argument error: choose only one mode among "
            "--once-health, --once-tools, --once-endpoints, --call-tool."
        )
        return 2
    if args.stream_events and not args.call_tool:
        print("Argument error: --stream-events requires --call-tool.")
        return 2

    try:
        config = load_config(args.config)
    except ConfigError as exc:
        print(f"Config error: {exc}")
        return 2

    try:
        params_json = _parse_json_object_arg(args.params_json, arg_name="--params-json")
        context_json = _parse_json_object_arg(args.context_json, arg_name="--context-json")
    except ValueError as exc:
        print(f"Argument error: {exc}")
        return 2

    if args.log_level:
        config = replace(config, server=replace(config.server, log_level=args.log_level))

    configure_logging(config.server.log_level, json_logs=config.server.json_logs)

    selector = WsEndpointSelector(
        instance_id=(args.ue_instance_id.strip() if isinstance(args.ue_instance_id, str) and args.ue_instance_id.strip() else None),
        project_dir=(args.ue_project_dir.strip() if isinstance(args.ue_project_dir, str) and args.ue_project_dir.strip() else None),
        process_id=(args.ue_process_id if isinstance(args.ue_process_id, int) and args.ue_process_id > 0 else None),
    )
    endpoint_selector = selector if selector.has_any() else None

    try:
        return asyncio.run(
            run(
                config,
                once_health=args.once_health,
                once_tools=args.once_tools,
                once_endpoints=args.once_endpoints,
                call_tool=args.call_tool,
                call_tool_params=params_json,
                call_tool_context=context_json,
                call_tool_timeout_ms=args.timeout_ms,
                stream_events=args.stream_events,
                print_metrics=args.print_metrics,
                endpoint_selector=endpoint_selector,
            )
        )
    except KeyboardInterrupt:
        return 0


def _build_error_payload(
    *,
    code: str,
    message: str,
    retriable: bool,
) -> dict[str, object]:
    return {
        "code": code,
        "message": message,
        "retriable": retriable,
    }


def _build_endpoint_listing_payload(
    *,
    config: AppConfig,
    endpoint_selector: WsEndpointSelector | None,
    env: Mapping[str, str] | None = None,
    cwd: Path | None = None,
) -> dict[str, object]:
    candidates = list_ws_endpoint_candidates(
        config,
        selector=endpoint_selector,
        env=env,
        cwd=cwd,
    )
    payload: dict[str, object] = {
        "selector": _selector_to_payload(endpoint_selector),
        "candidate_count": len(candidates),
        "candidates": [_candidate_to_payload(candidate) for candidate in candidates],
    }

    try:
        resolution = resolve_ws_endpoint(
            config,
            selector=endpoint_selector,
            env=env,
            cwd=cwd,
        )
        payload["resolved"] = {
            "ws_url": resolution.ws_url,
            "source": resolution.source,
            "instance_id": resolution.instance_id,
            "project_dir": resolution.project_dir,
            "process_id": resolution.process_id,
            "project_name": resolution.project_name,
            "connection_file": resolution.connection_file,
        }
    except WsEndpointSelectionError as exc:
        payload["resolution_error"] = str(exc)
    return payload


def _selector_to_payload(selector: WsEndpointSelector | None) -> dict[str, object]:
    if selector is None:
        return {
            "instance_id": None,
            "project_dir": None,
            "process_id": None,
        }
    return {
        "instance_id": selector.instance_id,
        "project_dir": selector.project_dir,
        "process_id": selector.process_id,
    }


def _candidate_to_payload(candidate: WsEndpointCandidate) -> dict[str, object]:
    return {
        "ws_url": candidate.ws_url,
        "source": candidate.source,
        "instance_id": candidate.instance_id,
        "project_dir": candidate.project_dir,
        "process_id": candidate.process_id,
        "project_name": candidate.project_name,
        "connection_file": candidate.connection_file,
        "descriptor_file": candidate.descriptor_file,
        "heartbeat_at_ms": candidate.heartbeat_at_ms,
        "updated_at_ms": candidate.updated_at_ms,
        "stale": candidate.stale,
        "selector_hint": _candidate_selector_hint(candidate),
    }


def _candidate_selector_hint(candidate: WsEndpointCandidate) -> dict[str, object]:
    env: dict[str, object] = {}
    args: list[str] = []
    if candidate.instance_id:
        env["UE_MCP_INSTANCE_ID"] = candidate.instance_id
        args.extend(["--ue-instance-id", candidate.instance_id])
    if candidate.project_dir:
        env["UE_MCP_PROJECT_DIR"] = candidate.project_dir
        args.extend(["--ue-project-dir", candidate.project_dir])
    if candidate.process_id is not None:
        env["UE_MCP_PROCESS_ID"] = candidate.process_id
        args.extend(["--ue-process-id", str(candidate.process_id)])
    return {
        "env": env,
        "args": args,
    }


def _print_error_payload(payload: dict[str, object], *, stream_events: bool) -> None:
    if stream_events:
        print(json.dumps({"type": "error", "error": payload}, ensure_ascii=False), flush=True)
    else:
        print(json.dumps({"error": payload}, ensure_ascii=False))


def _maybe_print_metrics(
    *,
    metrics: RuntimeMetrics | None,
    print_metrics: bool,
    stream_events: bool,
) -> None:
    if not print_metrics or metrics is None:
        return
    snapshot = metrics.snapshot()
    if stream_events:
        print(json.dumps({"type": "metrics", "metrics": snapshot}, ensure_ascii=False), flush=True)
    else:
        print(json.dumps({"metrics": snapshot}, ensure_ascii=False))


async def _metrics_log_loop(
    metrics: RuntimeMetrics,
    interval_s: float,
    stop_event: asyncio.Event,
) -> None:
    if interval_s <= 0:
        return

    while not stop_event.is_set():
        await asyncio.sleep(interval_s)
        if stop_event.is_set():
            return
        LOGGER.info("runtime.metrics=%s", json.dumps(metrics.snapshot(), ensure_ascii=False))
