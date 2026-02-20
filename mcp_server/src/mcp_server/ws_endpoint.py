from __future__ import annotations

from dataclasses import dataclass
import json
import logging
import os
from pathlib import Path
import re
from typing import Any, Mapping
from urllib.parse import urlparse, urlunparse

from mcp_server.config import AppConfig
from mcp_server.utils.wsl import is_wsl

LOGGER = logging.getLogger("mcp_server.ws_endpoint")

_WILDCARD_HOSTS = {"0.0.0.0", "::", "[::]", "*", ""}
_WINDOWS_ABS_PATH_RE = re.compile(r"^[a-zA-Z]:[\\/]")


@dataclass(frozen=True)
class WsEndpointResolution:
    ws_url: str
    source: str
    connection_file: str | None = None


def resolve_ws_endpoint(
    config: AppConfig,
    *,
    env: Mapping[str, str] | None = None,
    cwd: Path | None = None,
) -> WsEndpointResolution:
    environment = dict(os.environ if env is None else env)
    current_dir = (cwd or Path.cwd()).resolve()

    env_ws_url = environment.get("UE_MCP_WS_URL", "").strip()
    if env_ws_url:
        normalized_env_url = _normalize_ws_url(env_ws_url)
        if normalized_env_url:
            return WsEndpointResolution(
                ws_url=normalized_env_url,
                source="env:UE_MCP_WS_URL",
            )
        LOGGER.warning("Ignoring invalid UE_MCP_WS_URL: %s", env_ws_url)

    for connection_file in _iter_connection_file_candidates(config, environment, current_dir):
        resolved = _resolve_ws_url_from_connection_file(connection_file)
        if resolved:
            return WsEndpointResolution(
                ws_url=resolved,
                source="connection_file",
                connection_file=str(connection_file),
            )

    return WsEndpointResolution(
        ws_url=config.ue.ws_url,
        source="config:ue.ws_url",
    )


def _iter_connection_file_candidates(
    config: AppConfig,
    env: Mapping[str, str],
    cwd: Path,
) -> list[Path]:
    candidates: list[Path] = []
    seen: set[str] = set()

    def _append(raw_path: str) -> None:
        if not raw_path:
            return
        path = _normalize_fs_path(raw_path, cwd)
        if path is None:
            return
        key = str(path)
        if key in seen:
            return
        seen.add(key)
        candidates.append(path)

    _append(env.get("UE_MCP_CONNECTION_FILE", "").strip())
    _append(config.ue.connection_file.strip())

    for project_root in _iter_project_root_candidates(config, env, cwd):
        _append(str(project_root / "Saved" / "UnrealMCP" / "connection.json"))

    return candidates


def _iter_project_root_candidates(
    config: AppConfig,
    env: Mapping[str, str],
    cwd: Path,
) -> list[Path]:
    roots: list[Path] = []
    seen: set[str] = set()

    def _append(root: Path | None) -> None:
        if root is None:
            return
        key = str(root)
        if key in seen:
            return
        seen.add(key)
        roots.append(root)

    env_root = _normalize_fs_path(env.get("UE_MCP_PROJECT_ROOT", "").strip(), cwd)
    _append(env_root)
    config_root = _normalize_fs_path(config.ue.project_root.strip(), cwd)
    _append(config_root)

    discovered = _discover_nearby_project_roots(cwd)
    for discovered_root in discovered:
        _append(discovered_root)

    return roots


def _discover_nearby_project_roots(cwd: Path) -> list[Path]:
    roots: list[Path] = []
    seen: set[str] = set()

    def _append_if_project(dir_path: Path) -> None:
        if not dir_path.is_dir():
            return
        try:
            has_uproject = any(dir_path.glob("*.uproject"))
        except OSError:
            return
        if not has_uproject:
            return
        key = str(dir_path)
        if key in seen:
            return
        seen.add(key)
        roots.append(dir_path)

    bases: list[Path] = [cwd]
    parent = cwd
    for _ in range(3):
        parent = parent.parent
        if parent == parent.parent:
            break
        bases.append(parent)

    for base in bases:
        _append_if_project(base)
        try:
            children = sorted(
                [entry for entry in base.iterdir() if entry.is_dir()],
                key=lambda entry: entry.name,
            )
        except OSError:
            continue
        for child in children[:128]:
            _append_if_project(child)

    return roots


def _normalize_fs_path(raw_path: str, cwd: Path) -> Path | None:
    if not raw_path:
        return None

    text = raw_path.strip()
    if not text:
        return None

    if is_wsl() and _WINDOWS_ABS_PATH_RE.match(text):
        drive_letter = text[0].lower()
        rest = text[2:].replace("\\", "/")
        text = f"/mnt/{drive_letter}{rest}"

    path = Path(text).expanduser()
    if not path.is_absolute():
        path = cwd / path

    try:
        return path.resolve()
    except OSError:
        return path


def _resolve_ws_url_from_connection_file(connection_file: Path) -> str | None:
    if not connection_file.is_file():
        return None

    try:
        payload = json.loads(connection_file.read_text(encoding="utf-8"))
    except Exception as exc:
        LOGGER.warning("Failed to parse UE connection file. path=%s error=%s", connection_file, exc)
        return None

    if not isinstance(payload, dict):
        LOGGER.warning("Invalid UE connection file root. path=%s", connection_file)
        return None

    direct_ws_url = payload.get("ws_url")
    if isinstance(direct_ws_url, str):
        normalized_direct = _normalize_ws_url(direct_ws_url)
        if normalized_direct:
            return normalized_direct

    transport = payload.get("transport")
    if isinstance(transport, dict):
        transport_ws_url = transport.get("ws_url")
        if isinstance(transport_ws_url, str):
            normalized_transport = _normalize_ws_url(transport_ws_url)
            if normalized_transport:
                return normalized_transport

    bind_address = _first_string(payload, "bind_address")
    port = _first_int(payload, "port")
    if isinstance(transport, dict):
        bind_address = _first_string(transport, "bind_address") or bind_address
        port = _first_int(transport, "port") or port

    if not bind_address or not port:
        return None

    connect_host = _normalize_connect_host(bind_address)
    return f"ws://{connect_host}:{port}"


def _first_string(payload: dict[str, Any], key: str) -> str | None:
    value = payload.get(key)
    return value if isinstance(value, str) and value else None


def _first_int(payload: dict[str, Any], key: str) -> int | None:
    value = payload.get(key)
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    if isinstance(value, str):
        try:
            return int(value)
        except ValueError:
            return None
    return None


def _normalize_connect_host(host: str) -> str:
    normalized = host.strip().lower()
    if normalized in _WILDCARD_HOSTS or normalized == "localhost":
        return "127.0.0.1"
    return host


def _normalize_ws_url(ws_url: str) -> str | None:
    parsed = urlparse(ws_url)
    if parsed.scheme not in {"ws", "wss"}:
        return None
    if not parsed.netloc:
        return None

    host = parsed.hostname
    if host is None:
        return None

    try:
        port = parsed.port
    except ValueError:
        return None
    if port is None:
        port = 443 if parsed.scheme == "wss" else 80

    normalized_host = _normalize_connect_host(host)
    if ":" in normalized_host and not normalized_host.startswith("["):
        netloc = f"[{normalized_host}]:{port}"
    else:
        netloc = f"{normalized_host}:{port}"

    return urlunparse(
        (
            parsed.scheme,
            netloc,
            parsed.path or "",
            parsed.params or "",
            parsed.query or "",
            parsed.fragment or "",
        )
    )
