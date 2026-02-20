from __future__ import annotations

from dataclasses import dataclass
import json
from typing import Any


class MessageParseError(ValueError):
    """UE WS 메시지 파싱 실패."""


@dataclass(frozen=True)
class UeResponse:
    request_id: str
    status: str
    ok: bool
    envelope: dict[str, Any]
    raw_message: dict[str, Any]


def parse_json_message(raw_message: str) -> dict[str, Any]:
    try:
        parsed = json.loads(raw_message)
    except json.JSONDecodeError as exc:
        raise MessageParseError(f"Invalid JSON payload: {exc}") from exc

    if not isinstance(parsed, dict):
        raise MessageParseError("JSON payload must be an object.")
    return parsed


def parse_mcp_response(message: dict[str, Any]) -> UeResponse | None:
    if message.get("type") != "mcp.response":
        return None

    response_json = message.get("response_json")
    if not isinstance(response_json, str):
        raise MessageParseError("mcp.response.response_json must be a string.")

    try:
        envelope = json.loads(response_json)
    except json.JSONDecodeError as exc:
        raise MessageParseError(f"Invalid response_json payload: {exc}") from exc

    if not isinstance(envelope, dict):
        raise MessageParseError("mcp.response.response_json must decode to an object.")

    request_id = envelope.get("request_id")
    if not isinstance(request_id, str) or not request_id:
        raise MessageParseError("mcp.response.response_json.request_id is missing.")

    status = envelope.get("status")
    if not isinstance(status, str):
        status = "error"

    ok_value = message.get("ok")
    ok = bool(ok_value) if isinstance(ok_value, bool) else status != "error"

    return UeResponse(
        request_id=request_id,
        status=status,
        ok=ok,
        envelope=envelope,
        raw_message=message,
    )
