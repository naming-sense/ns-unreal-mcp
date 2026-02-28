from __future__ import annotations

import asyncio
import json
import logging
from typing import TYPE_CHECKING
from typing import Any

import websockets
from urllib.parse import urlparse, urlunparse

from mcp_server.event_router import EventRouter
from mcp_server.models.ue_messages import MessageParseError, UeResponse, parse_json_message
from mcp_server.request_broker import RequestBroker
from mcp_server.utils.wsl import get_wsl_default_gateway_ip, is_wsl

if TYPE_CHECKING:
    from mcp_server.metrics import RuntimeMetrics

LOGGER = logging.getLogger("mcp_server.ue_transport")


class UeWsTransport:
    def __init__(
        self,
        *,
        ws_url: str,
        request_broker: RequestBroker,
        event_router: EventRouter,
        connect_timeout_s: float = 10.0,
        ping_interval_s: float = 10.0,
        reconnect_initial_delay_s: float = 0.5,
        reconnect_max_delay_s: float = 10.0,
        metrics: "RuntimeMetrics | None" = None,
        expected_instance_id: str | None = None,
        expected_process_id: int | None = None,
        expected_project_dir: str | None = None,
    ) -> None:
        self._ws_url = ws_url
        self._ws_url_candidates = self._build_ws_url_candidates(ws_url)
        self._request_broker = request_broker
        self._event_router = event_router

        self._connect_timeout_s = connect_timeout_s
        self._ping_interval_s = ping_interval_s
        self._reconnect_initial_delay_s = reconnect_initial_delay_s
        self._reconnect_max_delay_s = reconnect_max_delay_s
        self._metrics = metrics

        self._expected_instance_id = expected_instance_id.strip() if isinstance(expected_instance_id, str) and expected_instance_id.strip() else None
        self._expected_process_id = expected_process_id if isinstance(expected_process_id, int) and expected_process_id > 0 else None
        self._expected_project_dir = (
            self._normalize_project_dir(expected_project_dir)
            if isinstance(expected_project_dir, str) and expected_project_dir.strip()
            else None
        )
        self._require_handshake_validation = any(
            (
                self._expected_instance_id is not None,
                self._expected_process_id is not None,
                self._expected_project_dir is not None,
            )
        )

        self._stop_event = asyncio.Event()
        self._connected_event = asyncio.Event()
        self._handshake_valid_event = asyncio.Event()
        self._send_lock = asyncio.Lock()
        self._run_task: asyncio.Task[None] | None = None
        self._ws: Any | None = None

    @property
    def is_connected(self) -> bool:
        ws = self._ws
        if not self._connected_event.is_set() or ws is None:
            return False
        return not self._is_ws_closed(ws)

    @property
    def ws_url_candidates(self) -> tuple[str, ...]:
        return tuple(self._ws_url_candidates)

    async def start(self) -> None:
        if self._run_task is not None and not self._run_task.done():
            return

        self._stop_event.clear()
        self._run_task = asyncio.create_task(self._run_forever(), name="ue-ws-transport")
        if self._metrics is not None:
            self._metrics.inc("ue_transport.start_called")

    async def stop(self) -> None:
        self._stop_event.set()

        ws = self._ws
        if ws is not None:
            await ws.close()

        if self._run_task is not None:
            try:
                await self._run_task
            finally:
                self._run_task = None
        if self._metrics is not None:
            self._metrics.inc("ue_transport.stop_called")
            self._metrics.set_gauge("ue_transport.connected", 0)

    async def wait_until_connected(self, timeout_s: float | None = None) -> None:
        if timeout_s is None:
            await self._connected_event.wait()
            return
        await asyncio.wait_for(self._connected_event.wait(), timeout=timeout_s)

    async def request(
        self,
        *,
        tool: str,
        params: dict[str, Any] | None = None,
        context: dict[str, Any] | None = None,
        timeout_ms: int | None = None,
        request_id: str | None = None,
        session_id: str | None = None,
    ) -> UeResponse:
        await self.wait_until_connected(timeout_s=self._connect_timeout_s)
        if self._require_handshake_validation:
            await asyncio.wait_for(self._handshake_valid_event.wait(), timeout=self._connect_timeout_s)
        return await self._request_broker.send_request(
            send_json=self.send_json,
            tool=tool,
            params=params,
            context=context,
            timeout_ms=timeout_ms,
            request_id=request_id,
            session_id=session_id or "default-session",
        )

    async def send_json(self, message: dict[str, Any]) -> None:
        payload = json.dumps(message, separators=(",", ":"), ensure_ascii=False)

        async with self._send_lock:
            ws = self._ws
            if ws is None or self._is_ws_closed(ws):
                raise ConnectionError("UE WS transport is not connected.")

            try:
                await ws.send(payload)
            except Exception as exc:
                raise ConnectionError(f"UE WS send failed: {exc}") from exc
            if self._metrics is not None:
                self._metrics.inc("ue_transport.message_out")

    async def _run_forever(self) -> None:
        reconnect_delay = self._reconnect_initial_delay_s

        while not self._stop_event.is_set():
            ws: Any | None = None
            connected_url: str | None = None

            last_error: Exception | None = None
            for candidate_url in self._ws_url_candidates:
                if self._stop_event.is_set():
                    break
                if self._metrics is not None:
                    self._metrics.inc("ue_transport.connect_attempt")
                try:
                    ws = await websockets.connect(
                        candidate_url,
                        open_timeout=self._connect_timeout_s,
                        ping_interval=None,
                        close_timeout=1,
                    )
                    connected_url = candidate_url
                    break
                except Exception as exc:
                    last_error = exc
                    if not self._stop_event.is_set():
                        LOGGER.warning("UE WS connect failed. url=%s error=%s", candidate_url, exc)
                        if self._metrics is not None:
                            self._metrics.inc("ue_transport.connect_failure")

            if ws is None or connected_url is None:
                if self._metrics is not None:
                    self._metrics.set_gauge("ue_transport.connected", 0)
                    self._metrics.inc("ue_transport.disconnected")
                if last_error is not None and not self._stop_event.is_set():
                    LOGGER.debug("Last UE WS connect error: %s", last_error)
                await asyncio.sleep(reconnect_delay)
                reconnect_delay = min(reconnect_delay * 2.0, self._reconnect_max_delay_s)
                continue

            # Prefer the last known working endpoint for reconnect.
            if connected_url != self._ws_url_candidates[0]:
                self._ws_url_candidates = [connected_url] + [
                    url for url in self._ws_url_candidates if url != connected_url
                ]

            self._ws = ws
            self._handshake_valid_event.clear()
            self._connected_event.set()
            reconnect_delay = self._reconnect_initial_delay_s
            LOGGER.info("Connected to UE WS transport: %s", connected_url)
            if self._metrics is not None:
                self._metrics.inc("ue_transport.connect_success")
                self._metrics.set_gauge("ue_transport.connected", 1)

            ping_task = asyncio.create_task(self._ping_loop(), name="ue-ws-ping")
            try:
                await self._receive_loop(ws)
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                if not self._stop_event.is_set():
                    LOGGER.warning("UE WS transport disconnected: %s", exc)
            finally:
                ping_task.cancel()
                await asyncio.gather(ping_task, return_exceptions=True)
                self._connected_event.clear()
                self._handshake_valid_event.clear()
                self._ws = None
                await self._request_broker.fail_all(ConnectionError("UE WS transport disconnected."))
                if self._metrics is not None:
                    self._metrics.set_gauge("ue_transport.connected", 0)
                    self._metrics.inc("ue_transport.disconnected")
                try:
                    await ws.close()
                except Exception:
                    pass

            if self._stop_event.is_set():
                break

            await asyncio.sleep(reconnect_delay)
            reconnect_delay = min(reconnect_delay * 2.0, self._reconnect_max_delay_s)

    async def _receive_loop(self, ws: Any) -> None:
        async for raw_message in ws:
            if self._stop_event.is_set():
                break

            if isinstance(raw_message, bytes):
                if self._metrics is not None:
                    self._metrics.inc("ue_transport.message_in_binary")
                try:
                    text_message = raw_message.decode("utf-8")
                except UnicodeDecodeError:
                    LOGGER.warning(
                        "Invalid binary WS message (utf-8 decode failed). size=%d",
                        len(raw_message),
                    )
                    continue

                await self._handle_text_message(text_message)
                continue

            if self._metrics is not None:
                self._metrics.inc("ue_transport.message_in_text")
            await self._handle_text_message(raw_message)

    async def _handle_text_message(self, raw_message: str) -> None:
        try:
            message = parse_json_message(raw_message)
        except MessageParseError as exc:
            LOGGER.warning("Invalid WS JSON payload: %s", exc)
            return

        try:
            if await self._request_broker.resolve_from_message(message):
                return
        except MessageParseError as exc:
            LOGGER.warning("Invalid mcp.response payload: %s", exc)
            return

        msg_type = message.get("type")
        if msg_type == "pong":
            LOGGER.debug("Received UE pong.")
            if self._metrics is not None:
                self._metrics.inc("ue_transport.pong_received")
            return

        if msg_type == "mcp.transport.connected":
            mismatch_reason = self._handshake_mismatch_reason(message)
            if mismatch_reason is not None:
                if self._metrics is not None:
                    self._metrics.inc("ue_transport.handshake_mismatch")
                raise ConnectionError(mismatch_reason)

            if self._require_handshake_validation:
                self._handshake_valid_event.set()

            LOGGER.info("UE transport handshake: %s", message)
            if self._metrics is not None:
                self._metrics.inc("ue_transport.handshake")
            return

        if msg_type == "mcp.transport.error":
            LOGGER.warning("UE transport error: %s", message)
            if self._metrics is not None:
                self._metrics.inc("ue_transport.transport_error")
            return

        if "event_type" in message:
            self._event_router.publish(message)
            if self._metrics is not None:
                self._metrics.inc("ue_transport.event_forwarded")
            return

        LOGGER.debug("Unhandled WS message: %s", message)

    async def _ping_loop(self) -> None:
        while not self._stop_event.is_set() and self.is_connected:
            await asyncio.sleep(self._ping_interval_s)
            if self._stop_event.is_set() or not self.is_connected:
                return

            try:
                await self.send_json({"type": "ping"})
                if self._metrics is not None:
                    self._metrics.inc("ue_transport.ping_sent")
            except Exception as exc:
                LOGGER.debug("Ping loop stopped: %s", exc)
                return

    @staticmethod
    def _build_ws_url_candidates(ws_url: str) -> list[str]:
        candidates = [ws_url]

        if not is_wsl():
            return candidates

        parsed = urlparse(ws_url)
        host = parsed.hostname or ""
        if host not in {"127.0.0.1", "localhost"}:
            return candidates

        gateway_ip = get_wsl_default_gateway_ip()
        if not gateway_ip:
            return candidates

        port = parsed.port
        if port is None:
            if parsed.scheme == "wss":
                port = 443
            else:
                port = 80

        fallback_netloc = f"{gateway_ip}:{port}"
        fallback_url = urlunparse(
            (
                parsed.scheme,
                fallback_netloc,
                parsed.path or "",
                parsed.params or "",
                parsed.query or "",
                parsed.fragment or "",
            )
        )
        if fallback_url != ws_url:
            candidates.append(fallback_url)
            LOGGER.info(
                "WSL detected; adding Windows-host fallback endpoint. primary=%s fallback=%s",
                ws_url,
                fallback_url,
            )
        return candidates

    @staticmethod
    def _is_ws_closed(ws: Any) -> bool:
        # websockets <=14: WebSocketClientProtocol.closed -> bool
        closed_attr = getattr(ws, "closed", None)
        if isinstance(closed_attr, bool):
            return closed_attr

        # websockets 15+: ClientConnection.state -> enum State
        state = getattr(ws, "state", None)
        if state is not None:
            state_name = getattr(state, "name", None)
            if isinstance(state_name, str) and state_name:
                return state_name.upper() == "CLOSED"
            state_value = getattr(state, "value", None)
            if state_value is not None:
                try:
                    return int(state_value) == 3
                except Exception:
                    pass

        # Fallback: close_code becomes non-None after closing.
        if getattr(ws, "close_code", None) is not None:
            return True

        transport = getattr(ws, "transport", None)
        if transport is not None:
            try:
                return bool(transport.is_closing())
            except Exception:
                pass

        return False

    def _handshake_mismatch_reason(self, message: dict[str, Any]) -> str | None:
        if not self._require_handshake_validation:
            return None

        actual_instance_id = message.get("instance_id")
        if self._expected_instance_id is not None:
            if not isinstance(actual_instance_id, str) or actual_instance_id != self._expected_instance_id:
                return (
                    "UE handshake instance mismatch: "
                    f"expected={self._expected_instance_id} actual={actual_instance_id}"
                )

        actual_process_id = message.get("process_id")
        if self._expected_process_id is not None:
            try:
                parsed_process_id = int(actual_process_id)
            except Exception:
                parsed_process_id = None
            if parsed_process_id != self._expected_process_id:
                return (
                    "UE handshake process mismatch: "
                    f"expected={self._expected_process_id} actual={actual_process_id}"
                )

        if self._expected_project_dir is not None:
            actual_project_dir = message.get("project_dir")
            normalized_actual_project_dir = (
                self._normalize_project_dir(actual_project_dir)
                if isinstance(actual_project_dir, str)
                else None
            )
            if normalized_actual_project_dir != self._expected_project_dir:
                return (
                    "UE handshake project_dir mismatch: "
                    f"expected={self._expected_project_dir} actual={normalized_actual_project_dir or actual_project_dir}"
                )

        return None

    @staticmethod
    def _normalize_project_dir(raw_value: str) -> str:
        value = raw_value.strip().replace("\\", "/")
        if len(value) >= 2 and value[1] == ":":
            drive = value[0].lower()
            suffix = value[2:].lstrip("/")
            value = f"/mnt/{drive}/{suffix}"
        return value.rstrip("/").lower()
