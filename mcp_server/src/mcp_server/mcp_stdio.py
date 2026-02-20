from __future__ import annotations

import argparse
import asyncio
from dataclasses import replace
import json
import logging
from pathlib import Path
import sys
from typing import Any, BinaryIO

from mcp_server.config import AppConfig, ConfigError, load_config
from mcp_server.event_router import EventRouter
from mcp_server.logging_setup import configure_logging
from mcp_server.mcp_facade import MCPFacade
from mcp_server.metrics import RuntimeMetrics
from mcp_server.request_broker import RequestBroker
from mcp_server.tool_catalog import ToolCatalog, ToolDefinition
from mcp_server.tool_passthrough import MCPPassThroughService, UnknownToolError
from mcp_server.ue_transport import UeWsTransport
from mcp_server.ws_endpoint import resolve_ws_endpoint
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
    def __init__(self, pass_through: MCPPassThroughService) -> None:
        self._pass_through = pass_through
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


async def run_stdio(config: AppConfig) -> int:
    endpoint_resolution = resolve_ws_endpoint(config)
    config = replace(config, ue=replace(config.ue, ws_url=endpoint_resolution.ws_url))

    LOGGER.info("Resolved UE WS endpoint: %s (source=%s)", config.ue.ws_url, endpoint_resolution.source)
    if endpoint_resolution.connection_file:
        LOGGER.info("Using UE connection file: %s", endpoint_resolution.connection_file)

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
        await pass_through.start()
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

    try:
        return asyncio.run(run_stdio(config))
    except KeyboardInterrupt:
        LOGGER.info("Interrupted by user.")
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
