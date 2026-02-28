from __future__ import annotations

from dataclasses import dataclass
import json
import logging
import os
from pathlib import Path
import re
import time
from typing import Any, Mapping
from urllib.parse import urlparse, urlunparse

from mcp_server.config import AppConfig
from mcp_server.utils.wsl import is_wsl

LOGGER = logging.getLogger("mcp_server.ws_endpoint")

_WILDCARD_HOSTS = {"0.0.0.0", "::", "[::]", "*", ""}
_WINDOWS_ABS_PATH_RE = re.compile(r"^[a-zA-Z]:[\\/]")
_INSTANCE_HEARTBEAT_STALE_MS = 30_000
_MAX_INSTANCE_REGISTRY_FILES = 512


class WsEndpointSelectionError(ValueError):
    """여러 UE endpoint 중 대상을 특정하지 못한 경우."""


@dataclass(frozen=True)
class WsEndpointSelector:
    instance_id: str | None = None
    project_dir: str | None = None
    process_id: int | None = None

    def has_any(self) -> bool:
        return bool(self.instance_id or self.project_dir or self.process_id)


@dataclass(frozen=True)
class WsEndpointResolution:
    ws_url: str
    source: str
    connection_file: str | None = None
    instance_id: str | None = None
    project_dir: str | None = None
    process_id: int | None = None
    project_name: str | None = None


@dataclass(frozen=True)
class WsEndpointCandidate:
    ws_url: str
    source: str
    descriptor_file: str
    connection_file: str | None = None
    instance_id: str | None = None
    project_dir: str | None = None
    process_id: int | None = None
    project_name: str | None = None
    heartbeat_at_ms: int | None = None
    updated_at_ms: int | None = None
    stale: bool = False


@dataclass(frozen=True)
class _EndpointCandidate:
    ws_url: str
    source: str
    descriptor_file: str
    connection_file: str | None
    instance_id: str | None
    project_dir: str | None
    process_id: int | None
    project_name: str | None
    heartbeat_at_ms: int | None
    updated_at_ms: int | None


def resolve_ws_endpoint(
    config: AppConfig,
    *,
    env: Mapping[str, str] | None = None,
    cwd: Path | None = None,
    selector: WsEndpointSelector | None = None,
) -> WsEndpointResolution:
    environment = dict(os.environ if env is None else env)
    current_dir = (cwd or Path.cwd()).resolve()
    effective_selector = selector or _selector_from_env(environment, current_dir)

    env_ws_url = environment.get("UE_MCP_WS_URL", "").strip()
    if env_ws_url:
        normalized_env_url = _normalize_ws_url(env_ws_url)
        if normalized_env_url:
            return WsEndpointResolution(
                ws_url=normalized_env_url,
                source="env:UE_MCP_WS_URL",
            )
        LOGGER.warning("Ignoring invalid UE_MCP_WS_URL: %s", env_ws_url)

    candidates = _collect_endpoint_candidates(config, environment, current_dir)
    if candidates:
        selected_candidates = _filter_candidates_by_selector(
            candidates,
            effective_selector,
            current_dir,
        )

        if effective_selector.has_any() and not selected_candidates:
            raise WsEndpointSelectionError(
                _build_no_match_error_message(effective_selector, candidates)
            )

        if not effective_selector.has_any():
            selected_candidates = candidates

        if selected_candidates:
            if len(selected_candidates) > 1:
                raise WsEndpointSelectionError(
                    _build_ambiguous_error_message(selected_candidates)
                )

            candidate = selected_candidates[0]
            return WsEndpointResolution(
                ws_url=candidate.ws_url,
                source=candidate.source,
                connection_file=candidate.connection_file,
                instance_id=candidate.instance_id,
                project_dir=candidate.project_dir,
                process_id=candidate.process_id,
                project_name=candidate.project_name,
            )

    if effective_selector.has_any():
        raise WsEndpointSelectionError(
            "No UE endpoint candidate is available for the requested selector."
        )

    return WsEndpointResolution(
        ws_url=config.ue.ws_url,
        source="config:ue.ws_url",
    )


def list_ws_endpoint_candidates(
    config: AppConfig,
    *,
    env: Mapping[str, str] | None = None,
    cwd: Path | None = None,
    selector: WsEndpointSelector | None = None,
    include_stale: bool = False,
) -> list[WsEndpointCandidate]:
    environment = dict(os.environ if env is None else env)
    current_dir = (cwd or Path.cwd()).resolve()
    effective_selector = selector or _selector_from_env(environment, current_dir)

    raw_candidates = _collect_endpoint_candidates(
        config,
        environment,
        current_dir,
        include_stale=include_stale,
    )
    filtered = _filter_candidates_by_selector(raw_candidates, effective_selector, current_dir)
    return [_to_public_candidate(candidate) for candidate in filtered]


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


def _iter_instance_registry_candidates(
    config: AppConfig,
    env: Mapping[str, str],
    cwd: Path,
    connection_file_candidates: list[Path],
) -> list[Path]:
    candidates: list[Path] = []
    seen: set[str] = set()

    def _append(path: Path) -> None:
        key = str(path)
        if key in seen:
            return
        seen.add(key)
        candidates.append(path)

    for connection_file in connection_file_candidates:
        _append(connection_file.parent / "instances")

    for project_root in _iter_project_root_candidates(config, env, cwd):
        _append(project_root / "Saved" / "UnrealMCP" / "instances")

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


def _selector_from_env(
    env: Mapping[str, str],
    cwd: Path,
) -> WsEndpointSelector:
    instance_id = env.get("UE_MCP_INSTANCE_ID", "").strip() or None
    raw_project_dir = env.get("UE_MCP_PROJECT_DIR", "").strip()
    project_dir = None
    if raw_project_dir:
        normalized = _normalize_fs_path(raw_project_dir, cwd)
        project_dir = str(normalized) if normalized is not None else raw_project_dir

    process_id: int | None = None
    process_id_raw = env.get("UE_MCP_PROCESS_ID", "").strip()
    if process_id_raw:
        try:
            parsed = int(process_id_raw)
        except ValueError:
            LOGGER.warning("Ignoring invalid UE_MCP_PROCESS_ID: %s", process_id_raw)
        else:
            if parsed > 0:
                process_id = parsed
            else:
                LOGGER.warning("Ignoring non-positive UE_MCP_PROCESS_ID: %s", process_id_raw)

    return WsEndpointSelector(
        instance_id=instance_id,
        project_dir=project_dir,
        process_id=process_id,
    )


def _collect_endpoint_candidates(
    config: AppConfig,
    env: Mapping[str, str],
    cwd: Path,
    *,
    include_stale: bool = False,
) -> list[_EndpointCandidate]:
    connection_file_candidates = _iter_connection_file_candidates(config, env, cwd)
    instance_registry_dirs = _iter_instance_registry_candidates(
        config,
        env,
        cwd,
        connection_file_candidates,
    )

    deduplicated: dict[tuple[str], _EndpointCandidate] = {}

    for connection_file in connection_file_candidates:
        candidate = _resolve_endpoint_candidate_from_file(
            descriptor_file=connection_file,
            source="connection_file",
            fallback_connection_file=str(connection_file),
            cwd=cwd,
        )
        if candidate is None:
            continue
        deduplicated[_candidate_key(candidate)] = _select_fresher_candidate(
            deduplicated.get(_candidate_key(candidate)),
            candidate,
        )

    for registry_dir in instance_registry_dirs:
        if not registry_dir.is_dir():
            continue

        instance_files = sorted(
            [entry for entry in registry_dir.glob("*.json") if entry.is_file()],
            key=lambda entry: entry.name,
        )
        for instance_file in instance_files[:_MAX_INSTANCE_REGISTRY_FILES]:
            candidate = _resolve_endpoint_candidate_from_file(
                descriptor_file=instance_file,
                source="instance_registry",
                fallback_connection_file=str(instance_file.parent.parent / "connection.json"),
                cwd=cwd,
            )
            if candidate is None:
                continue
            if not include_stale and _is_stale_instance_candidate(candidate):
                continue
            deduplicated[_candidate_key(candidate)] = _select_fresher_candidate(
                deduplicated.get(_candidate_key(candidate)),
                candidate,
            )

    candidates = list(deduplicated.values())
    candidates.sort(key=_candidate_sort_key, reverse=True)
    return candidates


def _resolve_endpoint_candidate_from_file(
    *,
    descriptor_file: Path,
    source: str,
    fallback_connection_file: str | None,
    cwd: Path,
) -> _EndpointCandidate | None:
    if not descriptor_file.is_file():
        return None

    try:
        payload = json.loads(descriptor_file.read_text(encoding="utf-8"))
    except Exception as exc:
        LOGGER.warning(
            "Failed to parse UE endpoint descriptor. path=%s error=%s",
            descriptor_file,
            exc,
        )
        return None

    if not isinstance(payload, dict):
        LOGGER.warning("Invalid UE endpoint descriptor root. path=%s", descriptor_file)
        return None

    resolved_ws_url = _extract_ws_url_from_payload(payload)
    if resolved_ws_url is None:
        return None

    project_dir = _first_string(payload, "project_dir")
    normalized_project_dir = None
    if project_dir:
        normalized = _normalize_fs_path(project_dir, cwd)
        normalized_project_dir = str(normalized) if normalized is not None else project_dir

    process_id = _first_int(payload, "process_id")
    instance_id = _first_string(payload, "instance_id")
    heartbeat_at_ms = _first_int(payload, "heartbeat_at_ms")
    updated_at_ms = _first_int(payload, "updated_at_ms")
    project_name = _first_string(payload, "project_name")
    connection_file = _first_string(payload, "connection_file") or fallback_connection_file

    return _EndpointCandidate(
        ws_url=resolved_ws_url,
        source=source,
        descriptor_file=str(descriptor_file),
        connection_file=connection_file,
        instance_id=instance_id,
        project_dir=normalized_project_dir,
        process_id=process_id,
        project_name=project_name,
        heartbeat_at_ms=heartbeat_at_ms,
        updated_at_ms=updated_at_ms,
    )


def _to_public_candidate(candidate: _EndpointCandidate) -> WsEndpointCandidate:
    return WsEndpointCandidate(
        ws_url=candidate.ws_url,
        source=candidate.source,
        descriptor_file=candidate.descriptor_file,
        connection_file=candidate.connection_file,
        instance_id=candidate.instance_id,
        project_dir=candidate.project_dir,
        process_id=candidate.process_id,
        project_name=candidate.project_name,
        heartbeat_at_ms=candidate.heartbeat_at_ms,
        updated_at_ms=candidate.updated_at_ms,
        stale=_is_stale_instance_candidate(candidate),
    )


def _extract_ws_url_from_payload(payload: dict[str, Any]) -> str | None:
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


def _is_stale_instance_candidate(candidate: _EndpointCandidate) -> bool:
    if candidate.source != "instance_registry":
        return False

    heartbeat = candidate.heartbeat_at_ms or candidate.updated_at_ms
    if heartbeat is None or heartbeat <= 0:
        return False

    now_ms = int(round(time.time() * 1000))
    return (now_ms - heartbeat) > _INSTANCE_HEARTBEAT_STALE_MS


def _candidate_key(candidate: _EndpointCandidate) -> tuple[str, ...]:
    if candidate.instance_id:
        return ("instance_id", candidate.instance_id)
    if candidate.process_id is not None:
        return ("process_id", str(candidate.process_id), candidate.ws_url)
    if candidate.descriptor_file:
        return ("descriptor_file", candidate.descriptor_file)
    return ("ws_url", candidate.ws_url)


def _candidate_sort_key(candidate: _EndpointCandidate) -> tuple[int, int]:
    heartbeat = candidate.heartbeat_at_ms or 0
    updated = candidate.updated_at_ms or 0
    return (heartbeat, updated)


def _select_fresher_candidate(
    existing: _EndpointCandidate | None,
    incoming: _EndpointCandidate,
) -> _EndpointCandidate:
    if existing is None:
        return incoming
    if _candidate_sort_key(incoming) >= _candidate_sort_key(existing):
        return incoming
    return existing


def _filter_candidates_by_selector(
    candidates: list[_EndpointCandidate],
    selector: WsEndpointSelector,
    cwd: Path,
) -> list[_EndpointCandidate]:
    if not selector.has_any():
        return candidates

    normalized_selector_project_dir: str | None = None
    if selector.project_dir:
        normalized = _normalize_fs_path(selector.project_dir, cwd)
        normalized_selector_project_dir = str(normalized) if normalized is not None else selector.project_dir

    filtered: list[_EndpointCandidate] = []
    for candidate in candidates:
        if selector.instance_id and candidate.instance_id != selector.instance_id:
            continue
        if selector.process_id and candidate.process_id != selector.process_id:
            continue
        if normalized_selector_project_dir:
            if not candidate.project_dir:
                continue
            if candidate.project_dir.lower() != normalized_selector_project_dir.lower():
                continue
        filtered.append(candidate)

    return filtered


def _build_ambiguous_error_message(candidates: list[_EndpointCandidate]) -> str:
    lines = [
        "Multiple UE endpoints matched. Set UE_MCP_INSTANCE_ID, UE_MCP_PROJECT_DIR, or UE_MCP_PROCESS_ID.",
        "Matched endpoints:",
    ]
    for candidate in candidates[:10]:
        lines.append(
            "- ws_url={ws_url} instance_id={instance_id} process_id={process_id} project_dir={project_dir} source={source} descriptor={descriptor}".format(
                ws_url=candidate.ws_url,
                instance_id=candidate.instance_id or "-",
                process_id=candidate.process_id if candidate.process_id is not None else "-",
                project_dir=candidate.project_dir or "-",
                source=candidate.source,
                descriptor=candidate.descriptor_file,
            )
        )
    return "\n".join(lines)


def _build_no_match_error_message(
    selector: WsEndpointSelector,
    candidates: list[_EndpointCandidate],
) -> str:
    lines = [
        "No UE endpoint matched selector: "
        f"instance_id={selector.instance_id or '-'} "
        f"project_dir={selector.project_dir or '-'} "
        f"process_id={selector.process_id if selector.process_id is not None else '-'}",
        "Available endpoints:",
    ]
    for candidate in candidates[:10]:
        lines.append(
            "- ws_url={ws_url} instance_id={instance_id} process_id={process_id} project_dir={project_dir} source={source} descriptor={descriptor}".format(
                ws_url=candidate.ws_url,
                instance_id=candidate.instance_id or "-",
                process_id=candidate.process_id if candidate.process_id is not None else "-",
                project_dir=candidate.project_dir or "-",
                source=candidate.source,
                descriptor=candidate.descriptor_file,
            )
        )
    return "\n".join(lines)


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
