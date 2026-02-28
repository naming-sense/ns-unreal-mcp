from __future__ import annotations

import argparse
import asyncio
from dataclasses import replace
import json
import logging
from pathlib import Path
import sys
from typing import Any, BinaryIO, Mapping

from mcp_server.config import AppConfig, ConfigError, load_config
from mcp_server.event_router import EventRouter
from mcp_server.logging_setup import configure_logging
from mcp_server.mcp_facade import MCPFacade
from mcp_server.metrics import RuntimeMetrics
from mcp_server.request_broker import RequestBroker
from mcp_server.tool_catalog import ToolCatalog, ToolDefinition
from mcp_server.tool_passthrough import CatalogGuardError, MCPPassThroughService, UnknownToolError
from mcp_server.umg_orchestrator import UMGOrchestrationService
from mcp_server.ue_transport import UeWsTransport
from mcp_server.ws_endpoint import (
    WsEndpointCandidate,
    WsEndpointSelectionError,
    WsEndpointSelector,
    list_ws_endpoint_candidates,
    resolve_ws_endpoint,
)
from mcp_server import __version__

LOGGER = logging.getLogger("mcp_server.mcp_stdio")

JSONRPC_VERSION = "2.0"
DEFAULT_MCP_PROTOCOL_VERSION = "2025-03-26"

JSONRPC_PARSE_ERROR = -32700
JSONRPC_INVALID_REQUEST = -32600
JSONRPC_METHOD_NOT_FOUND = -32601
JSONRPC_INVALID_PARAMS = -32602
JSONRPC_INTERNAL_ERROR = -32603
JSONRPC_SERVER_NOT_INITIALIZED = -32002


def _read_line(stream: BinaryIO) -> bytes | None:
    line = stream.readline()
    if line == b"":
        return None
    return line.rstrip(b"\r\n")


def _parse_json_payload(payload: bytes, *, error_message: str) -> dict[str, Any] | list[Any]:
    try:
        return json.loads(payload.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(error_message) from exc


def read_framed_message(stream: BinaryIO) -> dict[str, Any] | list[Any] | None:
    headers: dict[str, str] = {}

    while True:
        line = _read_line(stream)
        if line is None:
            return None
        if not line:
            continue
        break

    while True:
        if not line:
            break

        if b":" not in line:
            raise ValueError("Malformed header line.")

        key_bytes, value_bytes = line.split(b":", 1)
        key = key_bytes.decode("ascii", errors="strict").strip().lower()
        value = value_bytes.decode("ascii", errors="strict").strip()
        headers[key] = value

        next_line = _read_line(stream)
        if next_line is None:
            return None
        line = next_line

    if "content-length" not in headers:
        raise ValueError("Missing Content-Length header.")

    try:
        content_length = int(headers["content-length"])
    except ValueError as exc:
        raise ValueError("Invalid Content-Length header.") from exc

    if content_length < 0:
        raise ValueError("Content-Length must be >= 0.")

    payload = stream.read(content_length)
    if len(payload) != content_length:
        return None

    return _parse_json_payload(payload, error_message="Invalid JSON payload.")


def read_stdio_message(stream: BinaryIO) -> tuple[dict[str, Any] | list[Any] | None, str]:
    """
    Read one stdio message and detect framing mode.

    Returns:
      - (None, "framed") on EOF
      - (message, "framed") for Content-Length framed JSON
      - (message, "jsonl") for single-line JSON payload
    """
    while True:
        first_line = _read_line(stream)
        if first_line is None:
            return None, "framed"
        if not first_line:
            continue
        break

    if first_line.startswith((b"{", b"[")):
        return _parse_json_payload(first_line, error_message="Invalid JSON line payload."), "jsonl"

    headers: dict[str, str] = {}
    line = first_line
    while True:
        if not line:
            break

        if b":" not in line:
            raise ValueError("Malformed header line.")

        key_bytes, value_bytes = line.split(b":", 1)
        key = key_bytes.decode("ascii", errors="strict").strip().lower()
        value = value_bytes.decode("ascii", errors="strict").strip()
        headers[key] = value

        next_line = _read_line(stream)
        if next_line is None:
            return None, "framed"
        line = next_line

    if "content-length" not in headers:
        raise ValueError("Missing Content-Length header.")

    try:
        content_length = int(headers["content-length"])
    except ValueError as exc:
        raise ValueError("Invalid Content-Length header.") from exc

    if content_length < 0:
        raise ValueError("Content-Length must be >= 0.")

    payload = stream.read(content_length)
    if len(payload) != content_length:
        return None, "framed"

    return _parse_json_payload(payload, error_message="Invalid JSON payload."), "framed"


def write_jsonl_message(stream: BinaryIO, message: dict[str, Any]) -> None:
    payload = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    stream.write(payload)
    stream.write(b"\n")
    stream.flush()


def write_stdio_message(stream: BinaryIO, message: dict[str, Any], mode: str) -> None:
    if mode == "jsonl":
        write_jsonl_message(stream, message)
        return

    write_framed_message(stream, message)


def read_framed_message_legacy(stream: BinaryIO) -> dict[str, Any] | list[Any] | None:
    """
    Backward-compatible helper for tests and callers that only expect framed mode.
    """
    message, _ = read_stdio_message(stream)
    return message


def read_framed_message(stream: BinaryIO) -> dict[str, Any] | list[Any] | None:
    # Preserve original public API name.
    return read_framed_message_legacy(stream)


def read_json_line_message(stream: BinaryIO) -> dict[str, Any] | list[Any] | None:
    while True:
        line = _read_line(stream)
        if line is None:
            return None
        if not line:
            continue
        break

    if not line.startswith((b"{", b"[")):
        raise ValueError("JSON line payload must start with '{' or '['.")

    try:
        return json.loads(line.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError("Invalid JSON line payload.") from exc


def write_framed_message(stream: BinaryIO, message: dict[str, Any]) -> None:
    payload = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
    stream.write(header)
    stream.write(payload)
    stream.flush()


def make_jsonrpc_error(
    request_id: str | int | None,
    code: int,
    message: str,
    *,
    data: Any = None,
) -> dict[str, Any]:
    error: dict[str, Any] = {
        "code": code,
        "message": message,
    }
    if data is not None:
        error["data"] = data

    return {
        "jsonrpc": JSONRPC_VERSION,
        "id": request_id,
        "error": error,
    }


def make_jsonrpc_result(request_id: str | int | None, result: dict[str, Any]) -> dict[str, Any]:
    return {
        "jsonrpc": JSONRPC_VERSION,
        "id": request_id,
        "result": result,
    }


class MCPRequestDispatcher:
    def __init__(
        self,
        pass_through: MCPPassThroughService,
        *,
        umg_orchestration: UMGOrchestrationService | None = None,
    ) -> None:
        self._pass_through = pass_through
        self._umg_orchestration = umg_orchestration or UMGOrchestrationService(pass_through)
        self._initialized = False

    async def handle_request(self, request: dict[str, Any]) -> dict[str, Any]:
        request_id = request.get("id")
        method = request.get("method")
        params = request.get("params", {})

        if not isinstance(method, str):
            return make_jsonrpc_error(
                request_id,
                JSONRPC_INVALID_REQUEST,
                "Request method must be a string.",
            )

        if params is None:
            params = {}
        if not isinstance(params, dict):
            return make_jsonrpc_error(
                request_id,
                JSONRPC_INVALID_PARAMS,
                "Request params must be an object.",
            )

        if method == "initialize":
            return self._handle_initialize(request_id, params)

        if not self._initialized:
            return make_jsonrpc_error(
                request_id,
                JSONRPC_SERVER_NOT_INITIALIZED,
                "Server not initialized. Call initialize first.",
            )

        if method == "ping":
            return make_jsonrpc_result(request_id, {})
        if method == "tools/list":
            return make_jsonrpc_result(request_id, self._handle_tools_list(params))
        if method == "tools/call":
            return await self._handle_tools_call(request_id, params)
        if method == "resources/list":
            return make_jsonrpc_result(request_id, {"resources": []})
        if method == "resources/templates/list":
            return make_jsonrpc_result(request_id, {"resourceTemplates": []})
        if method == "prompts/list":
            return make_jsonrpc_result(request_id, {"prompts": []})

        return make_jsonrpc_error(
            request_id,
            JSONRPC_METHOD_NOT_FOUND,
            f"Method not found: {method}",
        )

    def handle_notification(self, request: dict[str, Any]) -> None:
        method = request.get("method")
        if method == "notifications/initialized":
            LOGGER.info("MCP client initialized notification received.")

    def _handle_initialize(
        self, request_id: str | int | None, params: dict[str, Any]
    ) -> dict[str, Any]:
        protocol_version = params.get("protocolVersion")
        if not isinstance(protocol_version, str) or not protocol_version:
            protocol_version = DEFAULT_MCP_PROTOCOL_VERSION

        self._initialized = True
        return make_jsonrpc_result(
            request_id,
            {
                "protocolVersion": protocol_version,
                "capabilities": {
                    "tools": {
                        "listChanged": False,
                    }
                },
                "serverInfo": {
                    "name": "ue-mcp-server",
                    "version": __version__,
                },
            },
        )

    def _handle_tools_list(self, params: dict[str, Any]) -> dict[str, Any]:
        cursor = params.get("cursor")
        if cursor not in (None, ""):
            return {
                "tools": [],
            }

        tools = [self._build_mcp_tool(tool) for tool in self._pass_through.list_tools()]
        tools.extend(self._umg_orchestration.list_virtual_tools())
        return {
            "tools": tools,
        }

    async def _handle_tools_call(
        self, request_id: str | int | None, params: dict[str, Any]
    ) -> dict[str, Any]:
        tool_name = params.get("name")
        arguments = params.get("arguments", {})

        if not isinstance(tool_name, str) or not tool_name:
            return make_jsonrpc_error(
                request_id,
                JSONRPC_INVALID_PARAMS,
                "tools/call requires 'name' as non-empty string.",
            )
        if arguments is None:
            arguments = {}
        if not isinstance(arguments, dict):
            return make_jsonrpc_error(
                request_id,
                JSONRPC_INVALID_PARAMS,
                "tools/call 'arguments' must be an object.",
            )

        tool_request_id = f"mcp-{request_id}" if request_id is not None else None
        try:
            if self._umg_orchestration.is_virtual_tool(tool_name):
                result = await self._umg_orchestration.call_virtual_tool(
                    tool_name=tool_name,
                    arguments=arguments,
                    request_id=tool_request_id,
                )
            else:
                result = await self._pass_through.call_tool(
                    tool=tool_name,
                    params=arguments,
                    request_id=tool_request_id,
                )
            is_error = result.status == "error" or not result.ok
            structured_content = {
                "ok": result.ok,
                "status": result.status,
                "request_id": result.request_id,
                "result": result.result,
                "diagnostics": result.diagnostics,
            }
        except UnknownToolError as exc:
            is_error = True
            structured_content = {
                "ok": False,
                "status": "error",
                "request_id": tool_request_id or "",
                "result": {},
                "diagnostics": {
                    "errors": [
                        {
                            "code": "MCP.SERVER.TOOL_NOT_FOUND",
                            "message": str(exc),
                            "retriable": False,
                        }
                    ]
                },
            }
        except ValueError as exc:
            return make_jsonrpc_error(
                request_id,
                JSONRPC_INVALID_PARAMS,
                str(exc),
            )
        except Exception as exc:
            LOGGER.exception("tools/call failed. tool=%s", tool_name)
            is_error = True
            structured_content = {
                "ok": False,
                "status": "error",
                "request_id": tool_request_id or "",
                "result": {},
                "diagnostics": {
                    "errors": [
                        {
                            "code": "MCP.SERVER.INTERNAL",
                            "message": str(exc),
                            "retriable": isinstance(exc, (ConnectionError, TimeoutError)),
                        }
                    ]
                },
            }

        return make_jsonrpc_result(
            request_id,
            {
                "isError": is_error,
                "structuredContent": structured_content,
                "content": [
                    {
                        "type": "text",
                        "text": json.dumps(structured_content, ensure_ascii=False),
                    }
                ],
            },
        )

    @staticmethod
    def _build_mcp_tool(tool: ToolDefinition) -> dict[str, Any]:
        description = f"[{tool.domain}] version={tool.version} write={tool.write}"
        tool_object: dict[str, Any] = {
            "name": tool.name,
            "description": description,
            "inputSchema": tool.params_schema
            if isinstance(tool.params_schema, dict)
            else {"type": "object", "additionalProperties": True},
            "annotations": {
                "readOnlyHint": not tool.write,
            },
        }
        if isinstance(tool.result_schema, dict):
            tool_object["outputSchema"] = tool.result_schema
        return tool_object


class MCPStdioServer:
    def __init__(self, dispatcher: MCPRequestDispatcher) -> None:
        self._dispatcher = dispatcher
        self._write_lock = asyncio.Lock()
        self._output_mode = "framed"

    async def run(self) -> int:
        pending_tasks: set[asyncio.Task[None]] = set()
        try:
            while True:
                try:
                    raw_message, input_mode = await asyncio.to_thread(
                        read_stdio_message, sys.stdin.buffer
                    )
                    if input_mode == "jsonl":
                        self._output_mode = "jsonl"
                except Exception as exc:
                    LOGGER.warning("Invalid MCP frame received: %s", exc)
                    await self._send(
                        make_jsonrpc_error(
                            None,
                            JSONRPC_PARSE_ERROR,
                            "Invalid MCP frame.",
                            data=str(exc),
                        )
                    )
                    continue

                if raw_message is None:
                    return 0

                if isinstance(raw_message, list):
                    await self._send(
                        make_jsonrpc_error(
                            None,
                            JSONRPC_INVALID_REQUEST,
                            "Batch request is not supported.",
                        )
                    )
                    continue

                if not isinstance(raw_message, dict):
                    await self._send(
                        make_jsonrpc_error(
                            None,
                            JSONRPC_INVALID_REQUEST,
                            "MCP payload must be a JSON object.",
                        )
                    )
                    continue

                if raw_message.get("jsonrpc") != JSONRPC_VERSION:
                    request_id = raw_message.get("id")
                    await self._send(
                        make_jsonrpc_error(
                            request_id if isinstance(request_id, (str, int)) else None,
                            JSONRPC_INVALID_REQUEST,
                            "jsonrpc must be '2.0'.",
                        )
                    )
                    continue

                if "method" not in raw_message:
                    continue

                if "id" not in raw_message:
                    self._dispatcher.handle_notification(raw_message)
                    continue

                task = asyncio.create_task(self._handle_request_message(raw_message))
                pending_tasks.add(task)
                task.add_done_callback(pending_tasks.discard)
        finally:
            if pending_tasks:
                for task in pending_tasks:
                    task.cancel()
                await asyncio.gather(*pending_tasks, return_exceptions=True)

    async def _handle_request_message(self, message: dict[str, Any]) -> None:
        request_id = message.get("id")
        if not isinstance(request_id, (str, int)):
            response = make_jsonrpc_error(
                None,
                JSONRPC_INVALID_REQUEST,
                "Request id must be string or number.",
            )
            await self._send(response)
            return

        try:
            response = await self._dispatcher.handle_request(message)
        except Exception as exc:
            LOGGER.exception("Unhandled exception while handling MCP request.")
            response = make_jsonrpc_error(
                request_id,
                JSONRPC_INTERNAL_ERROR,
                "Unhandled server error.",
                data=str(exc),
            )
        await self._send(response)

    async def _send(self, response: dict[str, Any]) -> None:
        async with self._write_lock:
            await asyncio.to_thread(
                write_stdio_message,
                sys.stdout.buffer,
                response,
                self._output_mode,
            )


async def run_stdio(
    config: AppConfig,
    *,
    endpoint_selector: WsEndpointSelector | None = None,
) -> int:
    try:
        endpoint_resolution = resolve_ws_endpoint(config, selector=endpoint_selector)
    except WsEndpointSelectionError as exc:
        LOGGER.error("UE endpoint selection failed: %s", exc)
        _log_endpoint_candidates_for_debug(config, endpoint_selector=endpoint_selector)
        return 2
    config = replace(config, ue=replace(config.ue, ws_url=endpoint_resolution.ws_url))

    LOGGER.info("Resolved UE WS endpoint: %s (source=%s)", config.ue.ws_url, endpoint_resolution.source)
    if endpoint_resolution.connection_file:
        LOGGER.info("Using UE connection file: %s", endpoint_resolution.connection_file)
    if endpoint_resolution.instance_id:
        LOGGER.info(
            "Resolved UE instance: instance_id=%s project_dir=%s process_id=%s project_name=%s",
            endpoint_resolution.instance_id,
            endpoint_resolution.project_dir or "-",
            endpoint_resolution.process_id if endpoint_resolution.process_id is not None else "-",
            endpoint_resolution.project_name or "-",
        )

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

    await transport.start()
    try:
        await transport.wait_until_connected(timeout_s=config.ue.connect_timeout_s)
    except TimeoutError:
        LOGGER.error("Failed to connect UE WS: %s", config.ue.ws_url)
        await transport.stop()
        return 3

    try:
        try:
            await pass_through.start()
        except CatalogGuardError as exc:
            LOGGER.error("Catalog guard failed: %s", exc)
            return 4
        dispatcher = MCPRequestDispatcher(pass_through)
        stdio_server = MCPStdioServer(dispatcher)
        return await stdio_server.run()
    finally:
        await pass_through.stop()
        await transport.stop()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="UE MCP stdio server")
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
    parser.add_argument(
        "--once-endpoints",
        action="store_true",
        help="List discovered UE endpoint candidates and exit",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        config = load_config(args.config)
    except ConfigError as exc:
        print(f"Config error: {exc}", file=sys.stderr)
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

    if args.once_endpoints:
        payload = _build_endpoint_listing_payload(config, endpoint_selector=endpoint_selector)
        print(json.dumps(payload, ensure_ascii=False))
        return 0

    try:
        return asyncio.run(run_stdio(config, endpoint_selector=endpoint_selector))
    except KeyboardInterrupt:
        LOGGER.info("Interrupted by user.")
        return 130


def _build_endpoint_listing_payload(
    config: AppConfig,
    *,
    endpoint_selector: WsEndpointSelector | None,
    env: Mapping[str, str] | None = None,
    cwd: Path | None = None,
) -> dict[str, Any]:
    candidates = list_ws_endpoint_candidates(
        config,
        selector=endpoint_selector,
        env=env,
        cwd=cwd,
    )
    payload: dict[str, Any] = {
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


def _selector_to_payload(selector: WsEndpointSelector | None) -> dict[str, Any]:
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


def _candidate_to_payload(candidate: WsEndpointCandidate) -> dict[str, Any]:
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


def _candidate_selector_hint(candidate: WsEndpointCandidate) -> dict[str, Any]:
    env: dict[str, Any] = {}
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


def _log_endpoint_candidates_for_debug(
    config: AppConfig,
    *,
    endpoint_selector: WsEndpointSelector | None,
) -> None:
    payload = _build_endpoint_listing_payload(config, endpoint_selector=endpoint_selector)
    LOGGER.error(
        "UE endpoint candidates snapshot: %s",
        json.dumps(payload, ensure_ascii=False),
    )


if __name__ == "__main__":
    raise SystemExit(main())
