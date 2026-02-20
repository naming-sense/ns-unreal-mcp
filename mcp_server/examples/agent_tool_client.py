#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import json
from dataclasses import replace
from pathlib import Path
from typing import Any

from mcp_server.config import AppConfig, ConfigError, load_config
from mcp_server.event_router import EventRouter
from mcp_server.logging_setup import configure_logging
from mcp_server.mcp_facade import MCPFacade, ToolCallResult
from mcp_server.metrics import RuntimeMetrics
from mcp_server.request_broker import RequestBroker
from mcp_server.tool_catalog import ToolCatalog
from mcp_server.tool_passthrough import MCPPassThroughService
from mcp_server.ue_transport import UeWsTransport
from mcp_server.ws_endpoint import resolve_ws_endpoint


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Example Python client for Unreal MCP bridge"
    )
    parser.add_argument("--config", type=Path, default=None, help="Path to YAML config")
    parser.add_argument("--log-level", type=str, default=None, help="Override log level")
    parser.add_argument("--list-tools", action="store_true", help="List synced tools and exit")
    parser.add_argument("--tool", type=str, default=None, help="Tool name to call")
    parser.add_argument(
        "--params-json",
        type=str,
        default="{}",
        help="JSON object for tool params",
    )
    parser.add_argument(
        "--context-json",
        type=str,
        default="{}",
        help="JSON object for tool context",
    )
    parser.add_argument("--timeout-ms", type=int, default=None, help="Override timeout in ms")
    parser.add_argument(
        "--stream-events",
        action="store_true",
        help="Emit event lines while calling the tool",
    )
    return parser.parse_args()


def _parse_json_object_arg(value: str, *, arg_name: str) -> dict[str, Any]:
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError as exc:
        raise ValueError(f"{arg_name} must be valid JSON object: {exc}") from exc

    if parsed is None:
        return {}
    if not isinstance(parsed, dict):
        raise ValueError(f"{arg_name} must be a JSON object.")
    return parsed


def _result_to_dict(result: ToolCallResult) -> dict[str, Any]:
    return {
        "ok": result.ok,
        "status": result.status,
        "request_id": result.request_id,
        "result": result.result,
        "diagnostics": result.diagnostics,
        "raw_envelope": result.raw_envelope,
    }


async def run_client(
    config: AppConfig,
    *,
    list_tools: bool,
    tool: str | None,
    params: dict[str, Any],
    context: dict[str, Any],
    timeout_ms: int | None,
    stream_events: bool,
) -> int:
    endpoint_resolution = resolve_ws_endpoint(config)
    config = replace(config, ue=replace(config.ue, ws_url=endpoint_resolution.ws_url))

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
    )
    facade = MCPFacade(transport)
    catalog = ToolCatalog()
    pass_through = MCPPassThroughService(
        facade=facade,
        catalog=catalog,
        event_router=event_router,
        include_schemas=config.catalog.include_schemas,
        refresh_interval_s=config.catalog.refresh_interval_s,
        transient_max_attempts=config.retry.transient_max_attempts,
        retry_backoff_initial_s=config.retry.backoff_initial_s,
        retry_backoff_max_s=config.retry.backoff_max_s,
        metrics=metrics,
    )

    await transport.start()
    try:
        await transport.wait_until_connected(timeout_s=config.ue.connect_timeout_s)
    except TimeoutError:
        print(
            json.dumps(
                {
                    "error": {
                        "code": "MCP.SERVER.CONNECT_TIMEOUT",
                        "message": f"Failed to connect UE WS: {config.ue.ws_url}",
                        "retriable": True,
                    }
                },
                ensure_ascii=False,
            )
        )
        await transport.stop()
        return 3

    try:
        await pass_through.start()

        if list_tools:
            payload = {
                "protocol_version": pass_through.protocol_version,
                "schema_hash": pass_through.schema_hash,
                "last_refresh_ms": pass_through.last_refresh_ms,
                "tools": pass_through.list_tools_as_dict(),
            }
            print(json.dumps(payload, ensure_ascii=False))
            return 0

        if tool is None:
            print(
                "Argument error: --tool is required unless --list-tools is set."
            )
            return 2

        if stream_events:

            def _on_event(event: dict[str, Any]) -> None:
                print(json.dumps({"type": "event", "event": event}, ensure_ascii=False), flush=True)

            result = await pass_through.call_tool_stream(
                tool=tool,
                params=params,
                context=context,
                timeout_ms=timeout_ms,
                on_event=_on_event,
            )
            print(
                json.dumps({"type": "result", "result": _result_to_dict(result)}, ensure_ascii=False),
                flush=True,
            )
        else:
            result = await pass_through.call_tool(
                tool=tool,
                params=params,
                context=context,
                timeout_ms=timeout_ms,
            )
            print(json.dumps(_result_to_dict(result), ensure_ascii=False))

        return 0 if result.status != "error" and result.ok else 4
    finally:
        await pass_through.stop()
        await transport.stop()


def main() -> int:
    args = parse_args()
    if args.list_tools and args.tool:
        print("Argument error: choose either --list-tools or --tool.")
        return 2
    if args.stream_events and not args.tool:
        print("Argument error: --stream-events requires --tool.")
        return 2

    try:
        config = load_config(args.config)
    except ConfigError as exc:
        print(f"Config error: {exc}")
        return 2

    if args.log_level:
        config = replace(config, server=replace(config.server, log_level=args.log_level))

    try:
        params = _parse_json_object_arg(args.params_json, arg_name="--params-json")
        context = _parse_json_object_arg(args.context_json, arg_name="--context-json")
    except ValueError as exc:
        print(f"Argument error: {exc}")
        return 2

    configure_logging(config.server.log_level, json_logs=config.server.json_logs)

    try:
        return asyncio.run(
            run_client(
                config,
                list_tools=args.list_tools,
                tool=args.tool,
                params=params,
                context=context,
                timeout_ms=args.timeout_ms,
                stream_events=args.stream_events,
            )
        )
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
