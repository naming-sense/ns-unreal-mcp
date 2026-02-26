from __future__ import annotations

import json
from pathlib import Path

import pytest

from mcp_server.config import AppConfig, UeConfig
from mcp_server.ws_endpoint import (
    WsEndpointSelectionError,
    WsEndpointSelector,
    list_ws_endpoint_candidates,
    resolve_ws_endpoint,
)


def test_resolve_ws_endpoint_prefers_env_override(tmp_path: Path) -> None:
    config = AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:19090"))

    resolved = resolve_ws_endpoint(
        config,
        env={"UE_MCP_WS_URL": "ws://10.0.0.10:19190"},
        cwd=tmp_path,
    )

    assert resolved.ws_url == "ws://10.0.0.10:19190"
    assert resolved.source == "env:UE_MCP_WS_URL"
    assert resolved.connection_file is None


def test_resolve_ws_endpoint_from_connection_file_transport_fields(tmp_path: Path) -> None:
    connection_file = tmp_path / "Saved" / "UnrealMCP" / "connection.json"
    connection_file.parent.mkdir(parents=True, exist_ok=True)
    connection_file.write_text(
        json.dumps(
            {
                "version": 1,
                "transport": {
                    "bind_address": "0.0.0.0",
                    "port": 19090,
                },
            }
        ),
        encoding="utf-8",
    )

    config = AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:19090"))
    resolved = resolve_ws_endpoint(
        config,
        env={"UE_MCP_CONNECTION_FILE": str(connection_file)},
        cwd=tmp_path,
    )

    assert resolved.ws_url == "ws://127.0.0.1:19090"
    assert resolved.source == "connection_file"
    assert resolved.connection_file == str(connection_file)


def test_resolve_ws_endpoint_from_project_root_connection_file(tmp_path: Path) -> None:
    project_root = tmp_path / "DemoProject"
    project_root.mkdir(parents=True, exist_ok=True)
    (project_root / "DemoProject.uproject").write_text("{}", encoding="utf-8")

    connection_file = project_root / "Saved" / "UnrealMCP" / "connection.json"
    connection_file.parent.mkdir(parents=True, exist_ok=True)
    connection_file.write_text(
        json.dumps(
            {
                "ws_url": "ws://172.16.1.20:19095",
            }
        ),
        encoding="utf-8",
    )

    config = AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:19090"))
    resolved = resolve_ws_endpoint(
        config,
        env={"UE_MCP_PROJECT_ROOT": str(project_root)},
        cwd=tmp_path,
    )

    assert resolved.ws_url == "ws://172.16.1.20:19095"
    assert resolved.source == "connection_file"
    assert resolved.connection_file == str(connection_file)


def test_resolve_ws_endpoint_falls_back_to_config_ws_url(tmp_path: Path) -> None:
    config = AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:19090"))

    resolved = resolve_ws_endpoint(
        config,
        env={"UE_MCP_WS_URL": "not-a-ws-url"},
        cwd=tmp_path,
    )

    assert resolved.ws_url == "ws://127.0.0.1:19090"
    assert resolved.source == "config:ue.ws_url"
    assert resolved.connection_file is None


def test_resolve_ws_endpoint_selects_instance_registry_by_selector(tmp_path: Path) -> None:
    project_a = tmp_path / "ProjectA"
    project_b = tmp_path / "ProjectB"
    project_a.mkdir(parents=True, exist_ok=True)
    project_b.mkdir(parents=True, exist_ok=True)
    (project_a / "ProjectA.uproject").write_text("{}", encoding="utf-8")
    (project_b / "ProjectB.uproject").write_text("{}", encoding="utf-8")

    instance_a = project_a / "Saved" / "UnrealMCP" / "instances" / "instance-a.json"
    instance_a.parent.mkdir(parents=True, exist_ok=True)
    instance_a.write_text(
        json.dumps(
            {
                "instance_id": "instance-a",
                "ws_url": "ws://127.0.0.1:19090",
                "project_dir": str(project_a),
                "process_id": 1111,
                "heartbeat_at_ms": 9_999_999_999_999,
                "updated_at_ms": 9_999_999_999_999,
            }
        ),
        encoding="utf-8",
    )

    instance_b = project_b / "Saved" / "UnrealMCP" / "instances" / "instance-b.json"
    instance_b.parent.mkdir(parents=True, exist_ok=True)
    instance_b.write_text(
        json.dumps(
            {
                "instance_id": "instance-b",
                "ws_url": "ws://127.0.0.1:19190",
                "project_dir": str(project_b),
                "process_id": 2222,
                "heartbeat_at_ms": 9_999_999_999_999,
                "updated_at_ms": 9_999_999_999_999,
            }
        ),
        encoding="utf-8",
    )

    config = AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:18080"))
    resolved = resolve_ws_endpoint(
        config,
        env={
            "UE_MCP_PROJECT_ROOT": str(project_a),
        },
        selector=WsEndpointSelector(instance_id="instance-a"),
        cwd=tmp_path,
    )

    assert resolved.ws_url == "ws://127.0.0.1:19090"
    assert resolved.instance_id == "instance-a"
    assert resolved.source == "instance_registry"


def test_resolve_ws_endpoint_raises_when_ambiguous(tmp_path: Path) -> None:
    project_root = tmp_path / "ProjectMulti"
    project_root.mkdir(parents=True, exist_ok=True)
    (project_root / "ProjectMulti.uproject").write_text("{}", encoding="utf-8")

    instances_dir = project_root / "Saved" / "UnrealMCP" / "instances"
    instances_dir.mkdir(parents=True, exist_ok=True)
    (instances_dir / "instance-a.json").write_text(
        json.dumps(
            {
                "instance_id": "instance-a",
                "ws_url": "ws://127.0.0.1:19090",
                "project_dir": str(project_root),
                "process_id": 1234,
                "heartbeat_at_ms": 9_999_999_999_999,
                "updated_at_ms": 9_999_999_999_999,
            }
        ),
        encoding="utf-8",
    )
    (instances_dir / "instance-b.json").write_text(
        json.dumps(
            {
                "instance_id": "instance-b",
                "ws_url": "ws://127.0.0.1:19091",
                "project_dir": str(project_root),
                "process_id": 1235,
                "heartbeat_at_ms": 9_999_999_999_999,
                "updated_at_ms": 9_999_999_999_999,
            }
        ),
        encoding="utf-8",
    )

    config = AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:18080"))

    with pytest.raises(WsEndpointSelectionError):
        resolve_ws_endpoint(
            config,
            env={"UE_MCP_PROJECT_ROOT": str(project_root)},
            cwd=tmp_path,
        )


def test_resolve_ws_endpoint_raises_when_selector_has_no_match(tmp_path: Path) -> None:
    project_root = tmp_path / "ProjectSingle"
    project_root.mkdir(parents=True, exist_ok=True)
    (project_root / "ProjectSingle.uproject").write_text("{}", encoding="utf-8")

    instances_dir = project_root / "Saved" / "UnrealMCP" / "instances"
    instances_dir.mkdir(parents=True, exist_ok=True)
    (instances_dir / "instance-a.json").write_text(
        json.dumps(
            {
                "instance_id": "instance-a",
                "ws_url": "ws://127.0.0.1:19090",
                "project_dir": str(project_root),
                "process_id": 1234,
                "heartbeat_at_ms": 9_999_999_999_999,
                "updated_at_ms": 9_999_999_999_999,
            }
        ),
        encoding="utf-8",
    )

    config = AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:18080"))

    with pytest.raises(WsEndpointSelectionError):
        resolve_ws_endpoint(
            config,
            env={"UE_MCP_PROJECT_ROOT": str(project_root)},
            selector=WsEndpointSelector(instance_id="unknown-instance"),
            cwd=tmp_path,
        )


def test_list_ws_endpoint_candidates_filters_stale_by_default(tmp_path: Path) -> None:
    project_root = tmp_path / "ProjectEndpoints"
    project_root.mkdir(parents=True, exist_ok=True)
    (project_root / "ProjectEndpoints.uproject").write_text("{}", encoding="utf-8")

    instances_dir = project_root / "Saved" / "UnrealMCP" / "instances"
    instances_dir.mkdir(parents=True, exist_ok=True)
    (instances_dir / "alive.json").write_text(
        json.dumps(
            {
                "instance_id": "alive",
                "ws_url": "ws://127.0.0.1:19090",
                "project_dir": str(project_root),
                "process_id": 1111,
                "heartbeat_at_ms": 9_999_999_999_999,
                "updated_at_ms": 9_999_999_999_999,
            }
        ),
        encoding="utf-8",
    )
    (instances_dir / "stale.json").write_text(
        json.dumps(
            {
                "instance_id": "stale",
                "ws_url": "ws://127.0.0.1:19091",
                "project_dir": str(project_root),
                "process_id": 2222,
                "heartbeat_at_ms": 1,
                "updated_at_ms": 1,
            }
        ),
        encoding="utf-8",
    )

    config = AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:18080"))
    default_candidates = list_ws_endpoint_candidates(
        config,
        env={"UE_MCP_PROJECT_ROOT": str(project_root)},
        cwd=tmp_path,
    )
    assert len(default_candidates) == 1
    assert default_candidates[0].instance_id == "alive"

    include_stale_candidates = list_ws_endpoint_candidates(
        config,
        env={"UE_MCP_PROJECT_ROOT": str(project_root)},
        cwd=tmp_path,
        include_stale=True,
    )
    assert len(include_stale_candidates) == 2
    assert {candidate.instance_id for candidate in include_stale_candidates} == {"alive", "stale"}
